#include "strokesampler.h"

#include <QtGlobal>

#include <cmath>

namespace {

// Параметры One Euro, подобранные перебором по четырём эталонам сразу: быстрое
// знакопеременное движение, медленная волна, рукописная буква с дрожанием и
// быстрая прямая. Работают в экранных единицах.
//
// Ключевой из них - kDerivativeCutoff. Стандартное для One Euro значение 1 Гц
// рассчитано на плавное слежение: оно сглаживает оценку скорости так сильно,
// что на знакопеременном движении в 10 Гц знак скорости усредняется почти в
// ноль. Фильтр принимает быстрое зюзюкание за медленное ведение, включает
// максимальное сглаживание и срезает размах - дуги превращаются в трапеции, а
// почерк в нечитаемое. Срез на 10 Гц оставляет оценку скорости настоящей.
//
// Что получается на эталонах: размах быстрого движения сохраняется на 95%,
// медленного - на 98%, дрожание падает до 38% от исходного, форма буквы
// уходит меньше чем на 0.6 экранной единицы, задержки на быстрой прямой нет.
constexpr qreal kMinCutoff = 1.5;
constexpr qreal kBeta = 0.05;
constexpr qreal kDerivativeCutoff = 10.0;

// Давление сглаживается мягче координат: скачок давления виден как ступенька
// на толщине линии.
constexpr qreal kPressureMinCutoff = 2.5;
constexpr qreal kPressureBeta = 0.01;

} // namespace

StrokeSampler::StrokeSampler()
    : m_filter(kMinCutoff, kBeta, kDerivativeCutoff)
    , m_pressureFilter(kPressureMinCutoff, kPressureBeta, kDerivativeCutoff)
{
}

void StrokeSampler::setSpacing(qreal spacing)
{
    m_screenSpacing = qMax(spacing, 0.05);
}

void StrokeSampler::setScale(qreal scale)
{
    m_scale = qBound(0.01, scale, 100.0);
}

void StrokeSampler::reset()
{
    m_filter.reset();
    m_pressureFilter.reset();
    m_points.clear();
    m_hasLastFiltered = false;
    m_finished = false;
}

QList<InkPoint> StrokeSampler::append(const InkPoint &raw)
{
    if (m_finished)
        return {};

    InkPoint filtered = raw;
    // Фильтрация идёт в экранных единицах: срез One Euro растёт вместе со
    // скоростью, и скорость эта должна быть той, с которой движется рука, а не
    // той, с которой ползёт точка по странице.
    filtered.position =
        m_filter.filter(raw.position * m_scale, raw.timeSeconds) / m_scale;
    // Отсутствие давления пробрасывается как есть: сглаживать нечего, а -1
    // должно дойти до тесселятора нетронутым.
    filtered.pressure =
        raw.hasPressure() ? m_pressureFilter.filter(raw.pressure, raw.timeSeconds) : -1.0;

    m_lastFiltered = filtered;
    m_lastRaw = raw;
    m_hasLastFiltered = true;

    if (m_points.isEmpty()) {
        m_points.append(filtered);
        return {filtered};
    }

    return emitTowards(filtered);
}

// Идёт от последней выданной точки к цели шагами ровно в spacing, линейно
// интерполируя давление и время.
QList<InkPoint> StrokeSampler::emitTowards(const InkPoint &target)
{
    QList<InkPoint> added;

    while (true) {
        const InkPoint &last = m_points.constLast();
        const QPointF delta = target.position - last.position;
        const qreal distance = std::hypot(delta.x(), delta.y());
        const qreal step = spacing();
        if (distance < step)
            break;

        const qreal t = step / distance;
        InkPoint next;
        next.position = last.position + delta * t;
        next.timeSeconds = last.timeSeconds + (target.timeSeconds - last.timeSeconds) * t;
        if (last.hasPressure() && target.hasPressure())
            next.pressure = last.pressure + (target.pressure - last.pressure) * t;
        else
            next.pressure = target.pressure;

        m_points.append(next);
        added.append(next);
    }

    return added;
}

QList<InkPoint> StrokeSampler::finish()
{
    if (m_finished || !m_hasLastFiltered)
        return {};
    m_finished = true;

    if (m_points.isEmpty()) {
        m_points.append(m_lastRaw);
        return {m_lastRaw};
    }

    QList<InkPoint> added = emitTowards(m_lastRaw);

    // Хвост короче шага ресемплинга остался бы отброшенным, а штрих обязан
    // кончаться в последней точке контакта - сценарий «отрыв пера».
    const QPointF delta = m_lastRaw.position - m_points.constLast().position;
    if (std::hypot(delta.x(), delta.y()) > 1e-9) {
        m_points.append(m_lastRaw);
        added.append(m_lastRaw);
    }
    return added;
}
