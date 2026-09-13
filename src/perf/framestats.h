#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTimer>
#include <QVector>

#include <atomic>

// Инструментация кадров. Бюджеты плавности из canvas-view и латентности из
// ink-input сформулированы в спеке как проверяемые требования, поэтому измерять
// их надо изнутри приложения, а не на глаз.
//
// Меряется три вещи:
//   * интервал между `frameSwapped` - реальная частота выдачи кадров;
//   * длительность отрисовки кадра (`beforeRendering` .. `afterFrameEnd`);
//   * латентность от события ввода до кадра, в котором оно стало видимым.
class FrameStats : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(bool active READ isActive WRITE setActive NOTIFY activeChanged)
    // Замер включён переменной окружения UQ_FRAME_LOG. По ней же показывается
    // экранная сводка: в обычной работе её быть не должно.
    Q_PROPERTY(bool logging READ isLogging CONSTANT)
    Q_PROPERTY(qreal refreshIntervalMs READ refreshIntervalMs NOTIFY statsChanged)
    Q_PROPERTY(qreal lastIntervalMs READ lastIntervalMs NOTIFY statsChanged)
    Q_PROPERTY(qreal medianIntervalMs READ medianIntervalMs NOTIFY statsChanged)
    Q_PROPERTY(qreal p99IntervalMs READ p99IntervalMs NOTIFY statsChanged)
    Q_PROPERTY(qreal maxIntervalMs READ maxIntervalMs NOTIFY statsChanged)
    Q_PROPERTY(qreal maxRenderMs READ maxRenderMs NOTIFY statsChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY statsChanged)
    Q_PROPERTY(int longFrameCount READ longFrameCount NOTIFY statsChanged)
    Q_PROPERTY(int idleGapCount READ idleGapCount NOTIFY statsChanged)
    Q_PROPERTY(qreal lastLatencyMs READ lastLatencyMs NOTIFY statsChanged)
    Q_PROPERTY(qreal maxLatencyMs READ maxLatencyMs NOTIFY statsChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY statsChanged)

public:
    explicit FrameStats(QObject *parent = nullptr);

    QQuickWindow *window() const { return m_window; }
    void setWindow(QQuickWindow *window);

    bool isActive() const { return m_active; }
    bool isLogging() const { return m_logTimer != nullptr; }
    void setActive(bool active);

    qreal refreshIntervalMs() const { return m_refreshIntervalMs; }
    qreal lastIntervalMs() const { return m_lastIntervalMs; }
    qreal medianIntervalMs() const;
    qreal p99IntervalMs() const;
    void ensureSorted() const;
    qreal maxIntervalMs() const { return m_maxIntervalMs; }
    qreal maxRenderMs() const { return m_maxRenderMs; }
    int frameCount() const { return m_frameCount; }
    // Кадр длиннее двух кадровых интервалов - то, чего canvas-view требует
    // не допускать вовсе. Считаются только настоящие пропуски: промежутки,
    // когда приложению нечего было перерисовывать, сюда не попадают.
    int longFrameCount() const { return m_longFrameCount; }
    // Промежутки простоя: кадр не выдавался, потому что ничего не менялось.
    // Учитываются отдельно, чтобы не выдавать паузу пользователя за просадку.
    int idleGapCount() const { return m_idleGapCount; }
    qreal lastLatencyMs() const { return m_lastLatencyMs; }
    qreal maxLatencyMs() const { return m_maxLatencyMs; }
    QString summary() const;

    // Помечает момент прихода события ввода. Латентность закрывается в
    // следующем показанном кадре.
    Q_INVOKABLE void markInput();
    Q_INVOKABLE void reset();

    // Монотонные наносекунды от общего для всего приложения начала отсчёта.
    static qint64 nowNs();

signals:
    void windowChanged();
    void activeChanged();
    void statsChanged();

private:
    void onFrameSwapped();
    void onBeforeRendering();
    void onAfterFrameEnd();
    void refreshScreenInterval();

    QQuickWindow *m_window = nullptr;
    bool m_active = true;

    qint64 m_lastSwapNs = 0;
    // Пишется из потока ввода, читается из потока рендера.
    std::atomic<qint64> m_pendingInputNs{0};
    qint64 m_renderStartNs = 0;

    qreal m_refreshIntervalMs = 16.67;
    qreal m_lastIntervalMs = 0.0;
    qreal m_maxIntervalMs = 0.0;
    qreal m_maxRenderMs = 0.0;
    qreal m_lastRenderMs = 0.0;
    qint64 m_startNs = 0;
    qreal m_lastLatencyMs = 0.0;
    qreal m_maxLatencyMs = 0.0;
    int m_frameCount = 0;
    int m_longFrameCount = 0;
    int m_idleGapCount = 0;

    QVector<qreal> m_intervals;
    // Медиана и p99 требуют сортировки всей выборки. Считать их на каждом кадре
    // значит нагружать поток UI ровно тем, что измеряешь, поэтому результат
    // кэшируется и пересчитывается не чаще нескольких раз в секунду.
    mutable QVector<qreal> m_sorted;
    mutable int m_sortedAtCount = -1;
    mutable qint64 m_sortedAtNs = 0;
    QTimer *m_logTimer = nullptr;
};
