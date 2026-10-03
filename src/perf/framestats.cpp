#include "framestats.h"

#include <QQuickWindow>
#include <QTextStream>
#include <QScreen>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "presentationfeedback.h"

namespace {
constexpr int kMaxSamples = 4096;
// Окно правдоподобия метки события относительно момента обработки, мс.
constexpr qint64 kStampFutureToleranceMs = 2;
constexpr qint64 kStampMaxAgeMs = 5000;
// Метка дальше этого - заведомо чужая (около 31 года аптайма); заодно
// llround ниже не переполняется.
constexpr qreal kStampMaxSeconds = 1e9;
}

qint64 FrameStats::nowNs()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

FrameStats::FrameStats(QObject *parent)
    : QObject(parent)
{
    m_intervals.reserve(kMaxSamples);
    m_startNs = nowNs();

    // Прогоны бюджетов плавности (9.1, 9.2) идут без человека у экрана,
    // поэтому те же цифры должны уходить в stdout.
    if (qEnvironmentVariableIsSet("UQ_FRAME_LOG")) {
        m_logTimer = new QTimer(this);
        m_logTimer->setInterval(1000);
        connect(m_logTimer, &QTimer::timeout, this, [this] {
            QTextStream(stdout) << "[frames] " << summary() << Qt::endl;
            if (m_presentation) {
                m_presentation->dispatchPending();
                const PresentationSummary present = m_presentation->takeSummary();
                if (!present.isEmpty())
                    QTextStream(stdout) << "[present] " << present.toString() << Qt::endl;
            }
        });
        m_logTimer->start();
    }
}

void FrameStats::setWindow(QQuickWindow *window)
{
    if (m_window == window)
        return;
    if (m_window)
        m_window->disconnect(this);
    // Подписка на показ привязана к wl_surface прежнего окна.
    delete m_presentation;
    m_presentation = nullptr;
    m_presentationTried = false;

    m_window = window;

    if (m_window) {
        // frameSwapped приходит из потока рендера; соединение по умолчанию
        // ставит вызов в очередь к нашему потоку, чтобы не трогать состояние
        // из двух потоков сразу.
        connect(m_window, &QQuickWindow::frameSwapped, this, &FrameStats::onFrameSwapped);
        connect(m_window, &QQuickWindow::beforeRendering, this, &FrameStats::onBeforeRendering,
                Qt::DirectConnection);
        connect(m_window, &QQuickWindow::afterFrameEnd, this, &FrameStats::onAfterFrameEnd,
                Qt::DirectConnection);
        connect(m_window, &QQuickWindow::screenChanged, this, &FrameStats::refreshScreenInterval);
        refreshScreenInterval();
    }
    reset();
    emit windowChanged();
}

void FrameStats::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (m_active)
        reset();
    emit activeChanged();
}

void FrameStats::refreshScreenInterval()
{
    QScreen *screen = m_window ? m_window->screen() : nullptr;
    const qreal rate = screen && screen->refreshRate() > 1.0 ? screen->refreshRate() : 60.0;
    m_refreshIntervalMs = 1000.0 / rate;
}

void FrameStats::reset()
{
    m_lastSwapNs = 0;
    m_renderStartNs = 0;
    m_pendingInputNs.store(0, std::memory_order_relaxed);
    m_lastIntervalMs = m_maxIntervalMs = m_maxRenderMs = 0.0;
    m_lastLatencyMs = m_maxLatencyMs = 0.0;
    m_frameCount = m_longFrameCount = m_idleGapCount = 0;
    m_foreignInputStamps = 0;
    m_intervals.clear();
    m_sorted.clear();
    m_sortedAtCount = -1;
    m_sortedAtNs = 0;
    emit statsChanged();
}

qint64 FrameStats::eventTimeToNs(qreal eventTimeSeconds, qint64 nowNs)
{
    // Отрицание сравнения ловит и NaN.
    if (!(eventTimeSeconds >= 0.0 && eventTimeSeconds <= kStampMaxSeconds) || nowNs <= 0)
        return -1;
    const qint64 nowMs = nowNs / 1'000'000;
    const auto stampMs32 = uint32_t(std::llround(eventTimeSeconds * 1000.0) & 0xffffffffLL);
    // Разница по модулю 2^32 со знаком: метка из прошлого даёт положительный
    // возраст и тогда, когда младшие биты часов уже перевалили через ноль.
    const qint64 ageMs = qint64(int32_t(uint32_t(nowMs) - stampMs32));
    if (ageMs < -kStampFutureToleranceMs || ageMs > kStampMaxAgeMs)
        return -1;
    return (nowMs - ageMs) * 1'000'000;
}

FrameStats::InputMark FrameStats::classifyInput(qreal eventTimeSeconds, qint64 nowNs)
{
    InputMark mark;
    const qint64 stamp = eventTimeToNs(eventTimeSeconds, nowNs);
    if (stamp >= 0) {
        mark.latencyFromNs = mark.presentationNs = stamp;
    } else if (eventTimeSeconds >= 0.0) {
        // Метка не из CLOCK_MONOTONIC (другой композитор, синтетика тестов) -
        // латентность до swap остаётся от момента обработки, а счётчик
        // показывает, что замер занижен.
        mark.latencyFromNs = nowNs;
        mark.foreign = true;
    } else {
        // Метки нет (сценарный ввод) - всё от момента обработки.
        mark.latencyFromNs = mark.presentationNs = nowNs;
    }
    return mark;
}

void FrameStats::markInput(qreal eventTimeSeconds)
{
    if (!m_active)
        return;
    const InputMark mark = classifyInput(eventTimeSeconds, nowNs());
    if (mark.foreign)
        ++m_foreignInputStamps;
    // Держим самое раннее ещё не показанное событие: латентность считается по
    // нему, иначе поток промежуточных событий занижал бы результат.
    qint64 expected = 0;
    m_pendingInputNs.compare_exchange_strong(expected, mark.latencyFromNs,
                                             std::memory_order_relaxed);
    if (m_presentation && mark.presentationNs > 0)
        m_presentation->noteInput(mark.presentationNs);
}

void FrameStats::onBeforeRendering()
{
    m_renderStartNs = nowNs();
}

void FrameStats::onAfterFrameEnd()
{
    if (m_renderStartNs == 0)
        return;
    const qreal renderMs = (nowNs() - m_renderStartNs) / 1e6;
    m_lastRenderMs = renderMs;
    m_maxRenderMs = qMax(m_maxRenderMs, renderMs);
    m_renderStartNs = 0;
}

void FrameStats::onFrameSwapped()
{
    if (!m_active)
        return;

    const qint64 now = nowNs();
    if (m_logTimer && !m_presentationTried && m_window) {
        // Первый показанный кадр: wl_surface у окна уже есть.
        m_presentationTried = true;
        m_presentation = PresentationFeedback::create(m_window, this);
    }
    if (m_presentation)
        m_presentation->dispatchPending();

    if (const qint64 input = m_pendingInputNs.exchange(0, std::memory_order_relaxed)) {
        m_lastLatencyMs = (now - input) / 1e6;
        m_maxLatencyMs = qMax(m_maxLatencyMs, m_lastLatencyMs);
    }

    if (m_lastSwapNs != 0) {
        m_lastIntervalMs = (now - m_lastSwapNs) / 1e6;
        m_maxIntervalMs = qMax(m_maxIntervalMs, m_lastIntervalMs);
        // Пропуском считается только то, за что отвечаем мы. Qt Quick не
        // выдаёт кадр, когда перерисовывать нечего, поэтому долгий промежуток
        // при почти нулевой отрисовке - это простой, а не просадка: разрыв в
        // 170 мс при отрисовке в 0.4 мс означает, что пользователь смотрел на
        // неподвижный экран, а не что мы не успели.
        const bool overBudget = m_lastIntervalMs > 2.0 * m_refreshIntervalMs;
        const bool weWereTheBottleneck = m_lastRenderMs > m_refreshIntervalMs;
        const bool plausibleMiss = m_lastIntervalMs <= 4.0 * m_refreshIntervalMs;
        const bool idleGap = overBudget && !weWereTheBottleneck && !plausibleMiss;

        if (overBudget && idleGap)
            ++m_idleGapCount;

        if (overBudget && !idleGap) {
            ++m_longFrameCount;
            if (m_logTimer) {
                QTextStream(stdout)
                    << "[long frame] " << QString::number(m_lastIntervalMs, 'f', 1) << " ms at "
                    << QString::number((now - m_startNs) / 1e9, 'f', 1) << " s, render "
                    << QString::number(m_lastRenderMs, 'f', 1) << " ms" << Qt::endl;
            }
        }
        if (m_intervals.size() < kMaxSamples)
            m_intervals.append(m_lastIntervalMs);
        ++m_frameCount;
    }
    m_lastSwapNs = now;

    // Показатели собираются всегда - это дёшево и нужно markInput(), - но
    // будить на каждом кадре QML нельзя: пересчёт привязок сводки меняет её
    // ширину, окно снова становится грязным, и приложение никогда не приходит
    // в покой (specs/idle-power). Сводку никто не читает, пока замер не
    // запрошен явно, так что и сигнала тогда быть не должно.
    if (m_logTimer)
        emit statsChanged();
}

void FrameStats::ensureSorted() const
{
    if (m_intervals.isEmpty()) {
        m_sorted.clear();
        return;
    }
    const qint64 now = nowNs();
    // Пересчёт не чаще четырёх раз в секунду и только если выборка выросла.
    if (m_sortedAtCount == m_intervals.size() || now - m_sortedAtNs < 250'000'000)
        if (!m_sorted.isEmpty())
            return;

    m_sorted = m_intervals;
    std::sort(m_sorted.begin(), m_sorted.end());
    m_sortedAtCount = m_intervals.size();
    m_sortedAtNs = now;
}

qreal FrameStats::medianIntervalMs() const
{
    ensureSorted();
    return m_sorted.isEmpty() ? 0.0 : m_sorted[m_sorted.size() / 2];
}

qreal FrameStats::p99IntervalMs() const
{
    ensureSorted();
    if (m_sorted.isEmpty())
        return 0.0;
    const int index = qMin(m_sorted.size() - 1, int(m_sorted.size() * 0.99));
    return m_sorted[index];
}

QString FrameStats::summary() const
{
    return QStringLiteral("refresh %1 ms | frames %2 | median %3 | p99 %4 | max %5 | render max %6"
                          "\nlong frames (>2x refresh): %7 | idle gaps: %10"
                          " | latency (event stamp -> swap) last %8 max %9 ms | foreign stamps %11")
        .arg(m_refreshIntervalMs, 0, 'f', 2)
        .arg(m_frameCount)
        .arg(medianIntervalMs(), 0, 'f', 2)
        .arg(p99IntervalMs(), 0, 'f', 2)
        .arg(m_maxIntervalMs, 0, 'f', 2)
        .arg(m_maxRenderMs, 0, 'f', 2)
        .arg(m_longFrameCount)
        .arg(m_lastLatencyMs, 0, 'f', 1)
        .arg(m_maxLatencyMs, 0, 'f', 1)
        .arg(m_idleGapCount)
        .arg(m_foreignInputStamps);
}
