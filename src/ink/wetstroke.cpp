#include "wetstroke.h"

#include <QtGlobal>

#include <cmath>

namespace {

// Порог случайного задева (design.md, D5).
constexpr qreal kGrazeSeconds = 0.030;
constexpr qreal kGrazePressure = 0.05;

QPointF normalizedDirection(const QPointF &from, const QPointF &to, const QPointF &fallback)
{
    const QPointF delta = to - from;
    const qreal length = std::hypot(delta.x(), delta.y());
    if (length < 1e-9)
        return fallback;
    return delta / length;
}

// Последние точки осевой вместе с провизорным хвостом - не больше, чем
// торец просматривает назад. tail[0] - последняя окончательная точка, она
// берётся из хвоста. На отрыве окно совпадает с последними точками штриха,
// поэтому мокрый торец совпадает с сухим вершина в вершину.
QList<InkPoint> endCapWindow(const QList<InkPoint> &points, const QList<InkPoint> &tail)
{
    const int lookback = StrokeTessellator::kCapLookback;
    const int fromTail = qMin(int(tail.size()), lookback);
    const int fromPoints = qMin(int(points.size()) - 1, lookback - fromTail);
    QList<InkPoint> window;
    window.reserve(fromPoints + fromTail);
    for (int i = int(points.size()) - 1 - fromPoints; i < int(points.size()) - 1; ++i)
        window.append(points[i]);
    for (int j = int(tail.size()) - fromTail; j < int(tail.size()); ++j)
        window.append(tail[j]);
    return window;
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
    // Мокрый штрих рисуется тем же законом толщины, с каким штрих потом
    // ляжет в страницу и в файл (ink-beauty-pass, design.md, D3).
    m_tessellator.setStyle(prototype.style);
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
    m_settledArc = 0.0;
}

int WetStroke::addPoint(const InkPoint &raw)
{
    if (!m_active)
        return 0;

    const int before = m_settledVertices;
    m_sampler.append(raw);
    // Перестраивается и тогда, когда окончательных точек не прибавилось:
    // провизорный хвост меняется с каждым отсчётом.
    rebuildTail();
    return m_settledVertices - before;
}

// Дописывает стыки для окончательных точек, которые их ещё не получили, и
// заново строит провизорную часть. Работа пропорциональна числу новых точек и
// длине хвоста.
void WetStroke::rebuildTail()
{
    const QList<InkPoint> &points = m_sampler.points();
    const QList<InkPoint> &provisional = m_sampler.provisionalPoints();

    // Снимаем провизорную часть.
    m_strip.resize(m_settledVertices);
    if (points.isEmpty())
        return;

    // Направления новых окончательных сегментов и длина окончательной осевой.
    QPointF previous = m_directions.isEmpty() ? QPointF(1.0, 0.0) : m_directions.constLast();
    for (int i = m_directions.size(); i + 1 < points.size(); ++i) {
        const QPointF delta = points[i + 1].position - points[i].position;
        m_settledArc += std::hypot(delta.x(), delta.y());
        previous = normalizedDirection(points[i].position, points[i + 1].position, previous);
        m_directions.append(previous);
    }

    if (m_jointedPoints == 0) {
        if (!startCapIsSettled(points)) {
            // Штрих ещё может оказаться точкой (design.md, D5), и хорда для
            // начального торца ещё не известна: вся полоса провизорная и
            // строится тем же правилом, что и сухая. Касание показывает точку
            // сразу, а не пустое место до отрыва, и той толщины, которой её
            // оставит finish(), - по наибольшему нажиму касания.
            QList<InkPoint> all = points;
            all.append(provisional);
            m_strip = m_tessellator.tessellate(withDotTwin(all));
            return;
        }
        const qreal radius = m_tessellator.halfWidthAt(points.first());
        m_tessellator.emitCap(
            m_strip, points.first().position, radius,
            StrokeTessellator::capDirection(points, StrokeTessellator::CapEnd::Start, radius),
            StrokeTessellator::CapEnd::Start);
        m_jointedPoints = 1;
    }

    // Стык в точке i опирается на сегменты i-1 и i, поэтому становится
    // окончательным ровно тогда, когда появилась окончательная точка i+1.
    for (int i = m_jointedPoints; i + 1 < points.size(); ++i) {
        m_tessellator.emitJoint(m_strip, points[i].position, m_tessellator.halfWidthAt(points[i]),
                                m_directions[i - 1], m_directions[i]);
    }
    m_jointedPoints = qMax(m_jointedPoints, int(points.size()) - 1);
    m_settledVertices = m_strip.size();

    // Провизорная часть: последняя окончательная точка, хвост за ней и торец.
    QList<InkPoint> tail;
    tail.reserve(provisional.size() + 1);
    tail.append(points.constLast());
    tail.append(provisional);

    QPointF incoming = m_directions.constLast();
    for (int j = 0; j + 1 < tail.size(); ++j) {
        const QPointF outgoing =
            normalizedDirection(tail[j].position, tail[j + 1].position, incoming);
        m_tessellator.emitJoint(m_strip, tail[j].position, m_tessellator.halfWidthAt(tail[j]),
                                incoming, outgoing);
        incoming = outgoing;
    }
    const InkPoint &end = tail.constLast();
    const qreal radius = m_tessellator.halfWidthAt(end);
    m_tessellator.emitCap(m_strip, end.position, radius,
                          StrokeTessellator::capDirection(endCapWindow(points, tail),
                                                          StrokeTessellator::CapEnd::End, radius),
                          StrokeTessellator::CapEnd::End);
}

bool WetStroke::startCapIsSettled(const QList<InkPoint> &points) const
{
    // Хорда либо уже найдена среди первых kCapLookback точек, либо искать её
    // дальше незачем - тем же правилом, что и StrokeTessellator::capDirection.
    // Точкой штрих перестаёт быть навсегда: окончательные точки только
    // добавляются, длина осевой только растёт.
    if (points.isEmpty() || m_settledArc < m_tessellator.width())
        return false;
    return StrokeTessellator::capChordIndex(points, StrokeTessellator::CapEnd::Start,
                                            m_tessellator.halfWidthAt(points.constFirst()))
        >= 0;
}

Stroke WetStroke::finish()
{
    if (!m_active)
        return {};

    m_sampler.finish();
    rebuildTail();
    m_active = false;

    // Случайный задев, а не точка: короче 30 мс и почти без нажима
    // (design.md, D5). Нажим - сырой, а не отфильтрованный: фильтр отстаёт на
    // нарастании, и быстрое намеренное касание по нему выглядело бы задевом.
    // У мыши давления нет, её касание не отбрасывается.
    const qreal peak = m_sampler.peakRawPressure();
    if (peak >= 0.0 && peak < kGrazePressure && m_sampler.contactDuration() < kGrazeSeconds) {
        m_strip.clear();
        m_settledVertices = 0;
        return {};
    }

    Stroke stroke = m_prototype;
    stroke.points = withDotTwin(m_sampler.points());
    return stroke;
}

QList<InkPoint> WetStroke::withDotTwin(QList<InkPoint> points) const
{
    // Касание начинается и кончается почти без нажима, а точки касания со
    // сдвигом - это его края: без этой точки капсула вышла бы тоньше нажима.
    // Касание из одной точки - частный случай: Stroke::isValid требует двух.
    if (points.isEmpty() || !m_tessellator.isDot(points))
        return points;
    InkPoint twin = points.constLast();
    if (twin.hasPressure())
        twin.pressure = m_sampler.peakPressure();
    points.append(twin);
    return points;
}
