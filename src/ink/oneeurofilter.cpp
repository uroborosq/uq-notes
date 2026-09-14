#include "oneeurofilter.h"

#include <QtGlobal>
#include <QtMath>

#include <cmath>

namespace {
// Наименьший интервал между событиями, который принимается всерьёз: события
// пера иногда приходят пачкой с одинаковой меткой времени.
constexpr qreal kMinDeltaSeconds = 1.0 / 1000.0;
}

OneEuroFilter::OneEuroFilter(qreal minCutoff, qreal beta, qreal derivativeCutoff)
    : m_minCutoff(minCutoff)
    , m_beta(beta)
    , m_derivativeCutoff(derivativeCutoff)
{
}

void OneEuroFilter::reset()
{
    m_primed = false;
    m_lastTime = 0.0;
    m_lastValue = 0.0;
    m_lastDerivative = 0.0;
}

qreal OneEuroFilter::alpha(qreal cutoff, qreal dt) const
{
    const qreal tau = 1.0 / (2.0 * M_PI * cutoff);
    return 1.0 / (1.0 + tau / dt);
}

qreal OneEuroFilter::filter(qreal value, qreal timeSeconds)
{
    if (!m_primed) {
        m_primed = true;
        m_lastTime = timeSeconds;
        m_lastValue = value;
        m_lastDerivative = 0.0;
        return value;
    }

    const qreal dt = qMax(timeSeconds - m_lastTime, kMinDeltaSeconds);
    m_lastTime = timeSeconds;

    const qreal rawDerivative = (value - m_lastValue) / dt;
    const qreal aDerivative = alpha(m_derivativeCutoff, dt);
    m_lastDerivative = aDerivative * rawDerivative + (1.0 - aDerivative) * m_lastDerivative;

    // Чем быстрее движение, тем выше срез и тем меньше сглаживания: так
    // задержка не накапливается на быстрых участках.
    const qreal cutoff = m_minCutoff + m_beta * std::abs(m_lastDerivative);
    const qreal a = alpha(cutoff, dt);
    m_lastValue = a * value + (1.0 - a) * m_lastValue;
    return m_lastValue;
}
