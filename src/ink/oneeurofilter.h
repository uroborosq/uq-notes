#pragma once

#include <QPointF>

// Фильтр One Euro: сглаживает дрожание, не добавляя заметной задержки. Срез
// растёт вместе со скоростью, поэтому медленное ведение пера сглаживается
// сильно, а быстрое почти не трогается - именно это позволяет убрать дрожь,
// не искажая форму буквы.
//
// Сейчас им сглаживается только давление: траекторию ведёт модель штриха в
// StrokeSampler, а скачок давления виден ступенькой толщины.
class OneEuroFilter
{
public:
    OneEuroFilter() = default;
    OneEuroFilter(qreal minCutoff, qreal beta, qreal derivativeCutoff);

    void reset();
    qreal filter(qreal value, qreal timeSeconds);

    bool isPrimed() const { return m_primed; }

private:
    qreal alpha(qreal cutoff, qreal dt) const;

    qreal m_minCutoff = 1.2;
    qreal m_beta = 0.02;
    qreal m_derivativeCutoff = 1.0;

    bool m_primed = false;
    qreal m_lastTime = 0.0;
    qreal m_lastValue = 0.0;
    qreal m_lastDerivative = 0.0;
};
