#include "stroketessellator.h"

#include <QtGlobal>
#include <QtMath>

#include <cmath>

namespace {

// Порог угла, за которым стык скругляется дугой, а не бисcектрисой.
constexpr qreal kRoundJoinThreshold = 0.35; // радиан, ~20 градусов
// Шаг дуги скругления.
constexpr qreal kRoundJoinStep = 0.28; // радиан, ~16 градусов
// Ниже этой длины биссектриса считается вырожденной (разворот почти на 180).
constexpr qreal kBisectorEpsilon = 0.2;

QPointF normalOf(const QPointF &direction)
{
    return QPointF(-direction.y(), direction.x());
}

QPointF normalized(const QPointF &v)
{
    const qreal length = std::hypot(v.x(), v.y());
    if (length < 1e-9)
        return QPointF();
    return v / length;
}

StrokeTessellator::Vertex toVertex(const QPointF &p)
{
    return {float(p.x()), float(p.y())};
}

qreal cross(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

} // namespace

void StrokeTessellator::setWidth(qreal width)
{
    m_width = qMax(width, 0.05);
}

void StrokeTessellator::setMinWidthFactor(qreal factor)
{
    m_minWidthFactor = qBound(0.0, factor, 1.0);
}

void StrokeTessellator::setConstantPressure(qreal pressure)
{
    m_constantPressure = qBound(0.0, pressure, 1.0);
}

qreal StrokeTessellator::halfWidthAt(const InkPoint &point) const
{
    // Устройство не сообщает давление - штрих идёт постоянной толщиной.
    const qreal pressure = point.hasPressure() ? qBound(0.0, point.pressure, 1.0)
                                               : m_constantPressure;
    const qreal factor = m_minWidthFactor + (1.0 - m_minWidthFactor) * pressure;
    return m_width * factor * 0.5;
}


void StrokeTessellator::emitCap(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                                const QPointF &direction) const
{
    const QPointF normal = normalOf(direction);
    strip.append(toVertex(position + normal * halfWidth));
    strip.append(toVertex(position - normal * halfWidth));
}

void StrokeTessellator::emitJoint(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                                  const QPointF &incoming, const QPointF &outgoing) const
{
    const auto emitPair = [&strip](const QPointF &left, const QPointF &right) {
        strip.append(toVertex(left));
        strip.append(toVertex(right));
    };

    const qreal turn =
        std::atan2(cross(incoming, outgoing), QPointF::dotProduct(incoming, outgoing));

    if (std::abs(turn) < kRoundJoinThreshold) {
        // Пологий стык: хватает биссектрисы.
        QPointF bisector = normalized(normalOf(incoming) + normalOf(outgoing));
        if (bisector.isNull())
            bisector = normalOf(outgoing);
        emitPair(position + bisector * halfWidth, position - bisector * halfWidth);
        return;
    }

    // Резкий стык: снаружи проходим дугой, внутри держим одну точку.
    // Дуга и есть скругление; внутренняя точка на биссектрисе не даёт
    // внутренней кромке пересечь саму себя.
    const bool turnsLeft = turn > 0.0;
    const QPointF startNormal = normalOf(incoming) * (turnsLeft ? -1.0 : 1.0);
    const QPointF endNormal = normalOf(outgoing) * (turnsLeft ? -1.0 : 1.0);

    const QPointF normalSum = normalOf(incoming) + normalOf(outgoing);
    QPointF innerDirection = normalized(normalSum);
    if (turnsLeft)
        innerDirection = -innerDirection;
    const bool innerDegenerate =
        std::hypot(normalSum.x(), normalSum.y()) < kBisectorEpsilon;
    const QPointF inner = innerDegenerate ? position : position - innerDirection * halfWidth;

    // Дуга идёт от нормали входящего сегмента к нормали исходящего по
    // короткой стороне. Направление берётся из знака векторного
    // произведения самих нормалей, а не из знака поворота: иначе на
    // повороте в одну из сторон дуга уходит длинным путём и оставляет
    // на стыке непокрытый клин.
    const qreal arcAngle =
        std::atan2(cross(startNormal, endNormal), QPointF::dotProduct(startNormal, endNormal));
    const int steps = qMax(1, int(std::ceil(std::abs(arcAngle) / kRoundJoinStep)));

    for (int step = 0; step <= steps; ++step) {
        const qreal angle = arcAngle * (qreal(step) / steps);
        const qreal c = std::cos(angle);
        const qreal s = std::sin(angle);
        const QPointF rotated(startNormal.x() * c - startNormal.y() * s,
                              startNormal.x() * s + startNormal.y() * c);
        const QPointF outer = position + rotated * halfWidth;
        // Порядок в полосе фиксирован: сначала «левая» сторона, потом
        // «правая», иначе треугольники вывернутся.
        if (turnsLeft)
            emitPair(inner, outer);
        else
            emitPair(outer, inner);
    }
}

QVector<StrokeTessellator::Vertex> StrokeTessellator::tessellate(    const QList<InkPoint> &points) const
{
    QVector<Vertex> strip;
    if (points.size() < 2)
        return strip;

    // Направления сегментов. Вырожденные сегменты (две точки в одном месте)
    // наследуют направление предыдущего, иначе нормаль была бы неопределена.
    QList<QPointF> directions;
    directions.reserve(points.size() - 1);
    QPointF previous(1.0, 0.0);
    for (int i = 0; i + 1 < points.size(); ++i) {
        QPointF direction = normalized(points[i + 1].position - points[i].position);
        if (direction.isNull())
            direction = previous;
        directions.append(direction);
        previous = direction;
    }

    strip.reserve(points.size() * 4);

    emitCap(strip, points.first().position, halfWidthAt(points.first()), directions.first());
    for (int i = 1; i + 1 < points.size(); ++i) {
        emitJoint(strip, points[i].position, halfWidthAt(points[i]), directions[i - 1],
                  directions[i]);
    }
    emitCap(strip, points.last().position, halfWidthAt(points.last()), directions.last());

    return strip;
}

QPolygonF StrokeTessellator::outline(const QList<InkPoint> &points) const
{
    const QVector<Vertex> strip = tessellate(points);
    QPolygonF polygon;
    if (isDegenerate(strip))
        return polygon;

    polygon.reserve(strip.size());
    // Чётные вершины полосы - одна сторона штриха, нечётные - другая.
    for (int i = 0; i < strip.size(); i += 2)
        polygon.append(QPointF(strip[i].x, strip[i].y));
    for (int i = strip.size() - 1; i > 0; i -= 2)
        polygon.append(QPointF(strip[i].x, strip[i].y));
    if (!polygon.isEmpty())
        polygon.append(polygon.first());
    return polygon;
}

bool StrokeTessellator::isDegenerate(const QVector<Vertex> &strip)
{
    if (strip.size() < 4)
        return true;
    for (const Vertex &vertex : strip) {
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y))
            return true;
    }
    return false;
}
