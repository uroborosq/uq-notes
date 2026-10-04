#include "stroketessellator.h"

#include <QtGlobal>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

// Порог угла, за которым стык скругляется дугой, а не бисcектрисой.
constexpr qreal kRoundJoinThreshold = 0.35; // радиан, ~20 градусов
// Шаг дуги скругления.
constexpr qreal kRoundJoinStep = 0.28; // радиан, ~16 градусов
// Ниже этой длины биссектриса считается вырожденной (разворот почти на 180).
constexpr qreal kBisectorEpsilon = 0.2;

// Доля толщины при нулевом нажиме по законам штриха (ink-beauty-pass,
// design.md, D3). 0.08, а не 0, как у rnote (W·p): на стенде конец 0.57 и
// заход 15 px против 0.544 и 13.7 px у эталона, а касание почти без нажима
// не пропадает совсем.
constexpr qreal kLegacyMinWidthFactor = 0.25;
constexpr qreal kTaperedMinWidthFactor = 0.08;

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

qreal cross(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

// Вершина кромки: направление выноса единичное, s - доля выноса в d.
AxisVertex axisVertex(const QPointF &center, const QPointF &direction, qreal halfWidth, qreal side)
{
    return {float(center.x()), float(center.y()), float(direction.x()), float(direction.y()),
            float(halfWidth), float(side)};
}

// Центр веера: d = 0, вынос нулевой.
AxisVertex axisCenter(const QPointF &center, qreal halfWidth)
{
    return axisVertex(center, QPointF(), halfWidth, 0.0);
}

QPointF rotated(const QPointF &v, qreal angle)
{
    const qreal c = std::cos(angle);
    const qreal s = std::sin(angle);
    return QPointF(v.x() * c - v.y() * s, v.x() * s + v.y() * c);
}

qreal turnAngle(const QPointF &incoming, const QPointF &outgoing)
{
    return std::atan2(cross(incoming, outgoing), QPointF::dotProduct(incoming, outgoing));
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

void StrokeTessellator::setStyle(StrokeStyle style)
{
    setMinWidthFactor(style == StrokeStyle::Tapered ? kTaperedMinWidthFactor
                                                    : kLegacyMinWidthFactor);
}

qreal StrokeTessellator::halfWidthAt(const InkPoint &point) const
{
    // Устройство не сообщает давление - штрих идёт постоянной толщиной, и
    // всегда по прежнему закону: W·0.7375 при давлении по умолчанию 0.65,
    // какой бы ни был стиль штриха (D3).
    if (!point.hasPressure()) {
        const qreal factor =
            kLegacyMinWidthFactor + (1.0 - kLegacyMinWidthFactor) * m_constantPressure;
        return m_width * factor * 0.5;
    }
    const qreal pressure = qBound(0.0, point.pressure, 1.0);
    const qreal factor = m_minWidthFactor + (1.0 - m_minWidthFactor) * pressure;
    return m_width * factor * 0.5;
}


void StrokeTessellator::emitCap(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                                const QPointF &direction, CapEnd end) const
{
    // Полувеер вокруг точки (design.md, D7): у плоской пары нет бахромы вдоль
    // штриха, кромка торца осталась бы ступенчатой. Пары D4 заменены,
    // круглый торец остался.
    const QPointF normal = normalOf(direction);
    const Vertex center = axisCenter(position, halfWidth);
    const auto arc = [&](int step, qreal sign) {
        const qreal phi = M_PI * step / kCapFanSteps;
        const QPointF v = normal * (sign * std::cos(phi)) + direction * (sign * std::sin(phi));
        return axisVertex(position, normalized(v), halfWidth, 1.0);
    };

    if (end == CapEnd::Start) {
        // От правой стороны назад, за начало штриха, к левой:
        // v(φ) = −n·cosφ − t·sinφ.
        for (int step = 0; step <= kCapFanSteps; ++step) {
            strip.append(center);
            strip.append(arc(step, -1.0));
        }
        // Последняя вершина дуги - левая вершина первой пары; правая - следом.
        // Треугольник «центр, левая, правая» вырожден по площади.
        strip.append(axisVertex(position, -normal, halfWidth, -1.0));
        return;
    }

    // Порядок в паре тот же, что у стыка: сначала «левая» сторона.
    strip.append(axisVertex(position, normal, halfWidth, 1.0));
    strip.append(axisVertex(position, -normal, halfWidth, -1.0));
    // От левой стороны вперёд, за конец штриха, к правой: v(φ) = n·cosφ + t·sinφ.
    for (int step = 0; step <= kCapFanSteps; ++step) {
        strip.append(center);
        strip.append(arc(step, 1.0));
    }
}

int StrokeTessellator::capChordIndex(const QList<InkPoint> &points, CapEnd end, qreal radius)
{
    const int count = int(points.size());
    if (count < 2)
        return -1;
    // Индекс k-й точки от того конца, у которого строится торец.
    const auto indexOf = [count, end](int k) { return end == CapEnd::Start ? k : count - 1 - k; };
    const QPointF tip = points[indexOf(0)].position;
    const int reach = qMin(count, kCapLookback);
    for (int k = 1; k < reach; ++k) {
        const QPointF delta = points[indexOf(k)].position - tip;
        if (std::hypot(delta.x(), delta.y()) >= radius)
            return indexOf(k);
    }
    // Дальше kCapLookback точек не ищем: хорда - до последней из них.
    return count >= kCapLookback ? indexOf(kCapLookback - 1) : -1;
}

QPointF StrokeTessellator::capDirection(const QList<InkPoint> &points, CapEnd end, qreal radius)
{
    const int count = int(points.size());
    if (count < 2)
        return QPointF(1.0, 0.0);
    const QPointF tip = end == CapEnd::Start ? points.constFirst().position
                                             : points.constLast().position;
    // Хорда ещё не определена - торец смотрит на самую дальнюю от конца точку.
    const int index = capChordIndex(points, end, radius);
    const QPointF far = index >= 0 ? points[index].position
                                   : (end == CapEnd::Start ? points.constLast().position
                                                           : points.constFirst().position);
    // Направление движения пера: в начале - от касания вперёд, в конце - к отрыву.
    const QPointF direction = normalized(end == CapEnd::Start ? far - tip : tip - far);
    return direction.isNull() ? QPointF(1.0, 0.0) : direction;
}

bool StrokeTessellator::isDot(const QList<InkPoint> &points) const
{
    // Длина осевой копится в том же порядке, что и у мокрого штриха
    // (WetStroke::rebuildTail): решение «точка или нет» обязано совпасть.
    qreal arc = 0.0;
    for (int i = 1; i < points.size(); ++i) {
        const QPointF delta = points[i].position - points[i - 1].position;
        arc += std::hypot(delta.x(), delta.y());
        if (arc >= m_width)
            return false;
    }
    return true;
}

void StrokeTessellator::emitJoint(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                                  const QPointF &incoming, const QPointF &outgoing) const
{
    const qreal turn = turnAngle(incoming, outgoing);
    // Внутренняя сторона - та, куда поворачивает штрих: при turn > 0 это
    // сторона нормали normalOf(direction).
    const bool turnsLeft = turn > 0.0;
    const qreal cosHalf = std::cos(turn / 2.0);

    if (std::abs(turn) < kRoundJoinThreshold) {
        // Пологий стык - пара на биссектрисе. Вершина на биссектрисе стоит на
        // расстоянии вынос·cos(θ/2) от прямых обоих сегментов, отсюда s.
        QPointF bisector = normalized(normalOf(incoming) + normalOf(outgoing));
        if (bisector.isNull())
            bisector = normalOf(outgoing);
        strip.append(axisVertex(position, bisector, halfWidth, cosHalf));
        strip.append(axisVertex(position, -bisector, halfWidth, -cosHalf));
        return;
    }

    // Резкий стык (design.md, D7, «Стыки точные»). Треугольник
    // (внутренняя, O_0, центр) точен относительно прямой входящего сегмента,
    // веер (O_k, центр, O_k+1) вокруг центра стыка - радиально,
    // (центр, O_K, внутренняя) - относительно исходящего. Веер из внутренней
    // точки, как было, на «M» давал p99 ошибки 108/255 против 54.
    const QPointF startNormal = normalOf(incoming) * (turnsLeft ? -1.0 : 1.0);
    const QPointF endNormal = normalOf(outgoing) * (turnsLeft ? -1.0 : 1.0);
    const QPointF normalSum = normalOf(incoming) + normalOf(outgoing);
    const bool innerDegenerate = std::hypot(normalSum.x(), normalSum.y()) < kBisectorEpsilon;
    const QPointF innerDirection = normalized(normalSum) * (turnsLeft ? 1.0 : -1.0);
    const qreal innerSide = turnsLeft ? 1.0 : -1.0;

    // На развороте почти на 180° биссектрисы нет: внутренняя вершина -
    // центр стыка.
    const Vertex inner = innerDegenerate
        ? axisCenter(position, halfWidth)
        : axisVertex(position, innerDirection, halfWidth, innerSide * cosHalf);
    const Vertex center = axisCenter(position, halfWidth);

    // Дуга идёт от нормали входящего сегмента к нормали исходящего по
    // короткой стороне. Направление берётся из знака векторного
    // произведения самих нормалей, а не из знака поворота: иначе на
    // повороте в одну из сторон дуга уходит длинным путём и оставляет
    // на стыке непокрытый клин.
    const qreal arcAngle = turnAngle(startNormal, endNormal);
    const int steps = qMax(1, int(std::ceil(std::abs(arcAngle) / kRoundJoinStep)));
    const auto outer = [&](int step) {
        return axisVertex(position, rotated(startNormal, arcAngle * (qreal(step) / steps)),
                          halfWidth, -innerSide);
    };

    // Порядок в полосе фиксирован: на границах стыка сначала «левая» сторона,
    // потом «правая», иначе сегмент между стыками вывернется.
    if (turnsLeft) {
        strip.append(inner);
        strip.append(outer(0));
        for (int step = 1; step <= steps; ++step) {
            strip.append(center);
            strip.append(outer(step));
        }
        strip.append(inner);
        strip.append(outer(steps));
    } else {
        strip.append(outer(0));
        strip.append(inner);
        strip.append(outer(0));
        for (int step = 1; step <= steps; ++step) {
            strip.append(center);
            strip.append(outer(step));
        }
        strip.append(inner);
    }
}

StrokeTessellator::Axis StrokeTessellator::axisOf(const QList<InkPoint> &points) const
{
    Axis axis;
    if (points.size() < 2)
        return axis;

    // Касание без движения или с едва заметным сдвигом - точка, а не
    // чёрточка (design.md, D5). Правило зависит только от сохранённых точек,
    // поэтому сухой вид совпадает с мокрым и с тем, что откроется из файла.
    if (isDot(points)) {
        // Капсула от первой до последней точки. Толщина точки - по самому
        // сильному нажиму касания, а не по слабым его краям: касание
        // начинается и кончается почти без давления. При совпадающих концах
        // два полувеера складываются в диск.
        qreal halfWidth = 0.0;
        for (const InkPoint &point : points)
            halfWidth = std::max(halfWidth, halfWidthAt(point));
        QPointF direction = normalized(points.constLast().position - points.constFirst().position);
        if (direction.isNull())
            direction = QPointF(1.0, 0.0);
        axis.positions = {points.constFirst().position, points.constLast().position};
        axis.halfWidths = {halfWidth, halfWidth};
        axis.directions = {direction};
        axis.startDirection = direction;
        axis.endDirection = direction;
        return axis;
    }

    axis.positions.reserve(points.size());
    axis.halfWidths.reserve(points.size());
    axis.directions.reserve(points.size() - 1);
    // Вырожденный сегмент (две точки в одном месте) наследует направление
    // предыдущего, иначе нормаль была бы неопределена.
    QPointF previous(1.0, 0.0);
    for (int i = 0; i < points.size(); ++i) {
        axis.positions.append(points[i].position);
        axis.halfWidths.append(halfWidthAt(points[i]));
        if (i + 1 < points.size()) {
            QPointF direction = normalized(points[i + 1].position - points[i].position);
            if (direction.isNull())
                direction = previous;
            axis.directions.append(direction);
            previous = direction;
        }
    }
    // Направление торца - хорда, а не последний сегмент (D4).
    axis.startDirection = capDirection(points, CapEnd::Start, axis.halfWidths.constFirst());
    axis.endDirection = capDirection(points, CapEnd::End, axis.halfWidths.constLast());
    return axis;
}

QVector<StrokeTessellator::Vertex> StrokeTessellator::tessellate(const QList<InkPoint> &points) const
{
    QVector<Vertex> strip;
    const Axis axis = axisOf(points);
    if (axis.positions.size() < 2)
        return strip;

    strip.reserve(axis.positions.size() * 4 + kStartCapVertices + kEndCapVertices);
    emitCap(strip, axis.positions.constFirst(), axis.halfWidths.constFirst(), axis.startDirection,
            CapEnd::Start);
    for (int i = 1; i + 1 < axis.positions.size(); ++i) {
        emitJoint(strip, axis.positions[i], axis.halfWidths[i], axis.directions[i - 1],
                  axis.directions[i]);
    }
    emitCap(strip, axis.positions.constLast(), axis.halfWidths.constLast(), axis.endDirection,
            CapEnd::End);
    return strip;
}

QPolygonF StrokeTessellator::outline(const QList<InkPoint> &points) const
{
    QPolygonF polygon;
    const Axis axis = axisOf(points);
    if (axis.positions.size() < 2)
        return polygon;

    // Точки контура лежат там, где у полосы на экране проходит кромка |d| = hw:
    // - вершина дуги (торец, внешняя сторона резкого стыка) - на полуширине;
    // - вершина на биссектрисе и внутренняя вершина резкого стыка - на
    //   пересечении кромок соседних сегментов, hw / cos(θ/2).
    QPolygonF left;
    QPolygonF right;
    for (int i = 1; i + 1 < axis.positions.size(); ++i) {
        const QPointF &c = axis.positions[i];
        const qreal hw = axis.halfWidths[i];
        const QPointF &incoming = axis.directions[i - 1];
        const QPointF &outgoing = axis.directions[i];
        const qreal turn = turnAngle(incoming, outgoing);
        const bool turnsLeft = turn > 0.0;
        const qreal miter = hw / std::max(std::cos(turn / 2.0), 1e-6);

        if (std::abs(turn) < kRoundJoinThreshold) {
            QPointF bisector = normalized(normalOf(incoming) + normalOf(outgoing));
            if (bisector.isNull())
                bisector = normalOf(outgoing);
            left.append(c + bisector * miter);
            right.append(c - bisector * miter);
            continue;
        }

        const QPointF normalSum = normalOf(incoming) + normalOf(outgoing);
        const bool innerDegenerate = std::hypot(normalSum.x(), normalSum.y()) < kBisectorEpsilon;
        const QPointF innerDirection = normalized(normalSum) * (turnsLeft ? 1.0 : -1.0);
        const QPointF inner = innerDegenerate ? c : c + innerDirection * miter;
        const QPointF startNormal = normalOf(incoming) * (turnsLeft ? -1.0 : 1.0);
        const QPointF endNormal = normalOf(outgoing) * (turnsLeft ? -1.0 : 1.0);
        const qreal arcAngle = turnAngle(startNormal, endNormal);
        const int steps = qMax(1, int(std::ceil(std::abs(arcAngle) / kRoundJoinStep)));

        (turnsLeft ? left : right).append(inner);
        QPolygonF &outerSide = turnsLeft ? right : left;
        for (int step = 0; step <= steps; ++step)
            outerSide.append(c + rotated(startNormal, arcAngle * (qreal(step) / steps)) * hw);
    }

    // Дуга торца - те же углы, что у полувеера полосы.
    const auto capArc = [&polygon](const QPointF &c, qreal hw, const QPointF &direction,
                                   qreal sign) {
        const QPointF normal = normalOf(direction);
        for (int step = 0; step <= kCapFanSteps; ++step) {
            const qreal phi = M_PI * step / kCapFanSteps;
            const QPointF v = normal * (sign * std::cos(phi)) + direction * (sign * std::sin(phi));
            polygon.append(c + normalized(v) * hw);
        }
    };

    polygon.reserve(left.size() + right.size() + 2 * (kCapFanSteps + 1) + 1);
    // От правой стороны начала назад к левой, левая сторона вперёд, от левой
    // стороны конца вперёд к правой, правая сторона назад.
    capArc(axis.positions.constFirst(), axis.halfWidths.constFirst(), axis.startDirection, -1.0);
    polygon += left;
    capArc(axis.positions.constLast(), axis.halfWidths.constLast(), axis.endDirection, 1.0);
    for (int i = int(right.size()) - 1; i >= 0; --i)
        polygon.append(right[i]);
    polygon.append(polygon.constFirst());
    return polygon;
}

bool StrokeTessellator::isDegenerate(const QVector<Vertex> &strip)
{
    if (strip.size() < 4)
        return true;
    for (const Vertex &v : strip) {
        if (!std::isfinite(v.cx) || !std::isfinite(v.cy) || !std::isfinite(v.nx)
            || !std::isfinite(v.ny) || !std::isfinite(v.hw) || !std::isfinite(v.s))
            return true;
    }
    return false;
}
