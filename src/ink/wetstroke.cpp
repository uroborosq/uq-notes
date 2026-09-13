#include "wetstroke.h"

#include <QtGlobal>

#include <cmath>

namespace {

QPointF normalizedDirection(const QPointF &from, const QPointF &to, const QPointF &fallback)
{
    const QPointF delta = to - from;
    const qreal length = std::hypot(delta.x(), delta.y());
    if (length < 1e-9)
        return fallback;
    return delta / length;
}

} // namespace

void WetStroke::begin(int page, const Stroke &prototype, qreal scale)
{
    reset();
    m_sampler.setScale(scale);
    m_active = true;
    m_page = page;
    m_prototype = prototype;
    m_prototype.page = page;
    m_prototype.points.clear();
    m_tessellator.setWidth(prototype.width);
}

void WetStroke::reset()
{
    m_active = false;
    m_page = -1;
    m_sampler.reset();
    m_strip.clear();
    m_directions.clear();
    m_settledVertices = 0;
    m_jointedPoints = 0;
}

int WetStroke::addPoint(const InkPoint &raw)
{
    if (!m_active)
        return 0;
    if (m_sampler.append(raw).isEmpty())
        return 0;

    const int before = m_strip.size();
    rebuildTail();
    return qMax(0, m_strip.size() - before);
}

// Дописывает стыки для точек, которые их ещё не получили, и переставляет
// провизорный торец в конец. Работа пропорциональна числу новых точек.
void WetStroke::rebuildTail()
{
    const QList<InkPoint> &points = m_sampler.points();
    if (points.size() < 2)
        return;

    QPointF previous = m_directions.isEmpty() ? QPointF(1.0, 0.0) : m_directions.constLast();
    for (int i = m_directions.size(); i + 1 < points.size(); ++i) {
        previous = normalizedDirection(points[i].position, points[i + 1].position, previous);
        m_directions.append(previous);
    }

    // Снимаем провизорный торец.
    m_strip.resize(m_settledVertices);

    if (m_jointedPoints == 0) {
        m_tessellator.emitCap(m_strip, points.first().position,
                              m_tessellator.halfWidthAt(points.first()), m_directions.first());
        m_jointedPoints = 1;
        m_settledVertices = m_strip.size();
    }

    // Стык в точке i опирается на сегменты i-1 и i, поэтому становится
    // окончательным ровно тогда, когда появилась точка i+1.
    for (int i = m_jointedPoints; i + 1 < points.size(); ++i) {
        m_tessellator.emitJoint(m_strip, points[i].position, m_tessellator.halfWidthAt(points[i]),
                                m_directions[i - 1], m_directions[i]);
    }
    m_jointedPoints = qMax(m_jointedPoints, points.size() - 1);
    m_settledVertices = m_strip.size();

    // И снова провизорный торец в последней точке.
    m_tessellator.emitCap(m_strip, points.constLast().position,
                          m_tessellator.halfWidthAt(points.constLast()), m_directions.constLast());
}

Stroke WetStroke::finish()
{
    if (!m_active)
        return {};

    m_sampler.finish();
    rebuildTail();

    Stroke stroke = m_prototype;
    stroke.points = m_sampler.points();
    m_active = false;
    return stroke;
}
