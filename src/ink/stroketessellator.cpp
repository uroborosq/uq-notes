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
    // Начальный торец идёт от кончика позади точки касания к боковой паре,
    // конечный - от боковой пары к кончику за точкой отрыва: так торец
    // стыкуется с полосой без разворота пар. Кончик - пара из двух совпадающих
    // вершин, её треугольники нулевой площади и ничего не рисуют.
    const QPointF normal = normalOf(direction);
    const qreal sign = end == CapEnd::Start ? -1.0 : 1.0;
    for (int i = 0; i <= kCapPairs; ++i) {
        const int k = end == CapEnd::Start ? i : kCapPairs - i;
        const qreal angle = M_PI_2 * k / kCapPairs;
        const QPointF base = position + direction * (sign * halfWidth * std::cos(angle));
        const QPointF side = normal * (halfWidth * std::sin(angle));
        // Порядок в паре тот же, что у стыка: сначала «левая» сторона.
        strip.append(toVertex(base + side));
        strip.append(toVertex(base - side));
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

void StrokeTessellator::emitDot(QVector<Vertex> &strip, const QList<InkPoint> &points) const
{
    // Толщина точки - по самому сильному нажиму касания, а не по слабым его
    // краям: касание начинается и кончается почти без давления.
    qreal radius = 0.0;
    for (const InkPoint &point : points)
        radius = qMax(radius, halfWidthAt(point));
    const QPointF from = points.constFirst().position;
    const QPointF to = points.constLast().position;
    QPointF direction = normalized(to - from);
    if (direction.isNull())
        direction = QPointF(1.0, 0.0);
    // Два полудиска: при совпадающих концах они складываются в диск.
    emitCap(strip, from, radius, direction, CapEnd::Start);
    emitCap(strip, to, radius, direction, CapEnd::End);
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

QVector<StrokeTessellator::Vertex> StrokeTessellator::tessellate(const QList<InkPoint> &points) const
{
    QVector<Vertex> strip;
    if (points.size() < 2)
        return strip;

    // Касание без движения или с едва заметным сдвигом - точка, а не
    // чёрточка (design.md, D5). Правило зависит только от сохранённых точек,
    // поэтому сухой вид совпадает с мокрым и с тем, что откроется из файла.
    if (isDot(points)) {
        strip.reserve(4 * (kCapPairs + 1));
        emitDot(strip, points);
        return strip;
    }

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

    strip.reserve(points.size() * 4 + 4 * (kCapPairs + 1));

    const qreal startRadius = halfWidthAt(points.first());
    emitCap(strip, points.first().position, startRadius,
            capDirection(points, CapEnd::Start, startRadius), CapEnd::Start);
    for (int i = 1; i + 1 < points.size(); ++i) {
        emitJoint(strip, points[i].position, halfWidthAt(points[i]), directions[i - 1],
                  directions[i]);
    }
    const qreal endRadius = halfWidthAt(points.last());
    emitCap(strip, points.last().position, endRadius,
            capDirection(points, CapEnd::End, endRadius), CapEnd::End);

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
