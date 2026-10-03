// Гейт D6 (openspec/changes/ink-beauty-pass/design.md, D6): узел острия в
// нарезке и окно [1, 2, 1]/4 на входе следования - вместе, поверх траектории
// приложения после шагов 1 и 2.
//
// GateSampler - копия src/ink/strokesampler.cpp (давление по шагу 1: One Euro
// 12 Гц / 0.5, давление узла по его времени, отсчёт без давления с давлением
// не смешивается, отфильтрованное давление на отрыве; задев - по сырому пику
// нажима и точка-касание - по WetStroke::finish шага 2) с двумя
// переключателями:
//  - raw121: следование тянет узел модели не к сырому отсчёту, а к отсчёту,
//    сглаженному центрированным окном [1, 2, 1]/4; узлы отсчёта n
//    окончательны, когда пришёл отсчёт n + 1 (exp/rnote, e3-raw121,
//    rnote_kit.h: smoothAt / finalizeSmooth / followTarget);
//  - cusp: точка плотного сплайна, пропущенная шагом нарезки, становится
//    точкой нарезки, если отходит от следующей хорды больше чем на 0.1
//    экранного px при повороте круче 45° (exp/dry-refine, dr-resample-tol:
//    Walker / farSkipped / walk / flushTowards).
//
// Варианты:
//  d6-copy        - оба переключателя выключены. Обязан совпасть с baseline по
//                   осевой (dev, facets, corner, curvTV): так проверяется, что
//                   копия не разошлась с src/ink/strokesampler.cpp.
//  d6-raw121-cusp - оба включены: то, что пойдёт в приложение, если гейт пройден.
//
// Геометрия - StrokeTessellator приложения на окончательных точках, как у
// baseline. Мокрая геометрия не строится: гейт смотрит на осевую, а время на
// отсчёт здесь - только сэмплер, без тесселяции мокрой полосы.

#include "lab.h"

#include <QtGlobal>

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "ink/inktypes.h"
#include "ink/oneeurofilter.h"
#include "ink/stroketessellator.h"

#include "ink_stroke_modeler/params.h"
#include "ink_stroke_modeler/stroke_modeler.h"
#include "ink_stroke_modeler/types.h"

namespace {

namespace ism = ink::stroke_model;

// --- Параметры: ровно как в src/ink/strokesampler.cpp -------------------------

constexpr float kSpringMassConstant = 2.2e-4f;
constexpr float kDragConstant = 94.0f;
constexpr qreal kFollowSpeedLower = 20.0;
constexpr qreal kFollowSpeedUpper = 200.0;
constexpr qreal kFollowSpeedTimeConstant = 0.01;
constexpr int kRawHistory = 64;
constexpr double kWobbleTimeout = 0.04;
constexpr float kWobbleSpeedFloor = 1.31f;
constexpr float kWobbleSpeedCeiling = 1.44f;
constexpr double kMinOutputRate = 100.0;
constexpr float kEndOfStrokeStoppingDistance = 0.01f;
constexpr int kEndOfStrokeMaxIterations = 20;
constexpr int kMaxOutputsPerCall = 100000;
// Шаг 1 (D2): One Euro давления 12 Гц, beta 0.5. Срез производной - прежний.
constexpr qreal kPressureMinCutoff = 12.0;
constexpr qreal kPressureBeta = 0.5;
constexpr qreal kPressureDerivativeCutoff = 10.0;
constexpr qreal kFlattenFraction = 0.5;
constexpr qreal kEndSnapDistance = 1e-4;

// D6.
constexpr qreal kCuspTolerance = 0.1;   // экранные px
constexpr qreal kCuspTurn = M_PI / 4.0; // 45°
constexpr int kMaxSkipped = 256;

// Шаг 2 (D5): порог случайного задева, как в src/ink/wetstroke.cpp. Нажим -
// сырой пик касания (StrokeSampler::peakRawPressure), длительность - по меткам
// отсчётов (StrokeSampler::contactDuration).
constexpr qreal kGrazeSeconds = 0.030;
constexpr qreal kGrazePressure = 0.05;

struct Options
{
    bool raw121 = false;
    bool cusp = false;
};

ism::StrokeModelParams modelParams()
{
    ism::StrokeModelParams params;
    params.wobble_smoother_params.is_enabled = true;
    params.wobble_smoother_params.timeout = ism::Duration(kWobbleTimeout);
    params.wobble_smoother_params.speed_floor = kWobbleSpeedFloor;
    params.wobble_smoother_params.speed_ceiling = kWobbleSpeedCeiling;
    params.position_modeler_params.spring_mass_constant = kSpringMassConstant;
    params.position_modeler_params.drag_constant = kDragConstant;
    params.sampling_params.min_output_rate = kMinOutputRate;
    params.sampling_params.end_of_stroke_stopping_distance = kEndOfStrokeStoppingDistance;
    params.sampling_params.end_of_stroke_max_iterations = kEndOfStrokeMaxIterations;
    params.sampling_params.max_outputs_per_call = kMaxOutputsPerCall;
    params.prediction_params = ism::StrokeEndPredictorParams{};
    return params;
}

qreal distance(const QPointF &a, const QPointF &b)
{
    return std::hypot(b.x() - a.x(), b.y() - a.y());
}

qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const qreal lengthSquared = QPointF::dotProduct(ab, ab);
    const qreal u = lengthSquared < 1e-18
        ? 0.0
        : qBound(0.0, QPointF::dotProduct(p - a, ab) / lengthSquared, 1.0);
    return distance(p, a + ab * u);
}

qreal turnAngle(const QPointF &in, const QPointF &out)
{
    return std::abs(std::atan2(in.x() * out.y() - in.y() * out.x(), QPointF::dotProduct(in, out)));
}

QPointF catmullRom(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3,
                   qreal u)
{
    const auto knot = [](const QPointF &a, const QPointF &b) {
        return std::sqrt(qMax(distance(a, b), 1e-6));
    };
    const qreal t0 = 0.0;
    const qreal t1 = t0 + knot(p0, p1);
    const qreal t2 = t1 + knot(p1, p2);
    const qreal t3 = t2 + knot(p2, p3);
    const qreal t = t1 + (t2 - t1) * u;
    const QPointF a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
    const QPointF a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
    const QPointF a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
    const QPointF b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
    const QPointF b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
    return b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
}

qreal lerpPressure(const InkPoint &from, const InkPoint &to, qreal u)
{
    if (from.hasPressure() && to.hasPressure())
        return from.pressure + (to.pressure - from.pressure) * u;
    return to.pressure;
}

// Положение пера в момент time по треку: между соседними отсчётами - по
// сплайну, у последнего интервала сосед отражается.
QPointF penAt(const QList<InkPoint> &track, qreal time)
{
    if (track.isEmpty())
        return {};
    if (time >= track.constLast().timeSeconds)
        return track.constLast().position;
    for (qsizetype i = track.size() - 1; i > 0; --i) {
        const InkPoint &a = track[i - 1];
        const InkPoint &b = track[i];
        if (time >= a.timeSeconds) {
            const qreal span = b.timeSeconds - a.timeSeconds;
            const qreal u = span > 0.0 ? (time - a.timeSeconds) / span : 1.0;
            const QPointF p0 = i >= 2 ? track[i - 2].position : a.position * 2.0 - b.position;
            const QPointF p3 =
                i + 1 < track.size() ? track[i + 1].position : b.position * 2.0 - a.position;
            return catmullRom(p0, a.position, b.position, p3, u);
        }
    }
    return track.constFirst().position;
}

class GateSampler
{
public:
    explicit GateSampler(Options options)
        : m_options(options)
        , m_modeler(std::make_unique<ism::StrokeModeler>())
        , m_pressureFilter(kPressureMinCutoff, kPressureBeta, kPressureDerivativeCutoff)
    {
        [[maybe_unused]] const absl::Status status = m_modeler->Reset(modelParams());
    }

    void setScale(qreal scale) { m_scale = qBound(0.01, scale, 100.0); }
    qreal spacing() const { return m_screenSpacing / m_scale; }

    void reset()
    {
        [[maybe_unused]] const absl::Status status = m_modeler->Reset();
        m_pressureFilter.reset();
        m_started = false;
        m_finished = false;
        m_timeOrigin = 0.0;
        m_lastInputTime = 0.0;
        m_lastInputScreen = QPointF();
        m_lastInputPressure = -1.0;
        m_peakPressure = -1.0;
        m_peakRawPressure = -1.0;
        m_lastRaw = InkPoint();
        m_follow = FollowState();
        m_rawHistory.clear();
        m_rawCount = 0;
        m_smoothedCount = 0;
        m_smoothTrack.clear();
        m_pendingResults.clear();
        m_skipped.clear();
        m_nodes.clear();
        m_nextSegment = 0;
        m_points.clear();
        m_provisional.clear();
    }

    const QList<InkPoint> &points() const { return m_points; }
    const QList<InkPoint> &provisionalPoints() const { return m_provisional; }

    // Шаг 2 (D5), WetStroke::finish: случайный задев штриха не оставляет, а к
    // точке (осевая короче толщины пера) дописывается точка с пиком
    // отфильтрованного нажима (WetStroke::withDotTwin).
    bool isGraze() const
    {
        const qreal contactDuration = m_started ? m_lastInputTime : 0.0;
        return m_peakRawPressure >= 0.0 && m_peakRawPressure < kGrazePressure
            && contactDuration < kGrazeSeconds;
    }
    void discard() { m_points.clear(); }
    void appendDotTwin(const StrokeTessellator &tessellator)
    {
        if (m_points.isEmpty() || !tessellator.isDot(m_points))
            return;
        InkPoint twin = m_points.constLast();
        if (twin.hasPressure())
            twin.pressure = m_peakPressure;
        m_points.append(twin);
    }

    void append(const InkPoint &raw)
    {
        if (m_finished)
            return;
        const QPointF screen = raw.position * m_scale;
        qreal time = raw.timeSeconds;
        if (m_started) {
            time = qMax(time - m_timeOrigin, m_lastInputTime);
            if (time == m_lastInputTime && screen == m_lastInputScreen)
                return;
        } else {
            m_timeOrigin = time;
            time = 0.0;
        }
        const qreal pressure = raw.hasPressure()
            ? qBound(0.0, m_pressureFilter.filter(raw.pressure, time), 1.0)
            : -1.0;

        ism::Input input;
        input.event_type = m_started ? ism::Input::EventType::kMove : ism::Input::EventType::kDown;
        input.position = {float(screen.x()), float(screen.y())};
        input.time = ism::Time(time);
        input.pressure = float(pressure);
        m_results.clear();
        if (!m_modeler->Update(input, m_results).ok())
            return;

        m_started = true;
        m_lastInputTime = time;
        m_lastInputScreen = screen;
        m_lastInputPressure = pressure;
        m_peakPressure = qMax(m_peakPressure, pressure);
        if (raw.hasPressure())
            m_peakRawPressure = qMax(m_peakRawPressure, qBound(0.0, raw.pressure, 1.0));
        m_lastRaw = raw;
        m_rawHistory.append({screen, pressure, time});
        while (m_rawHistory.size() > kRawHistory)
            m_rawHistory.removeFirst();
        ++m_rawCount;

        if (m_options.raw121) {
            m_pendingResults.emplace_back(m_rawCount - 1, m_results);
            settleSmoothedSamples(false);
        } else {
            for (const ism::Result &result : m_results)
                appendNode(result);
        }
        emitSettled(false);
        rebuildProvisional();
    }

    void finish()
    {
        if (m_finished || !m_started)
            return;
        m_finished = true;
        m_provisional.clear();

        ism::Input up;
        up.event_type = ism::Input::EventType::kUp;
        up.position = {float(m_lastInputScreen.x()), float(m_lastInputScreen.y())};
        up.time = ism::Time(m_lastInputTime);
        up.pressure = float(m_lastInputPressure);
        m_results.clear();
        const bool caughtUp = m_modeler->Update(up, m_results).ok();
        if (m_options.raw121)
            settleSmoothedSamples(true);
        if (caughtUp) {
            for (const ism::Result &result : m_results)
                appendNode(result);
        }
        emitSettled(true);

        // Шаг 1: последняя точка - с отфильтрованным давлением.
        InkPoint end = m_lastRaw;
        if (end.hasPressure())
            end.pressure = m_lastInputPressure;
        InkPoint last = m_points.constLast();
        QList<InkPoint> tail;
        walkTowards(last, m_skipped, end, tail);
        flushCusps(last, m_skipped, end, tail);
        if (distance(last.position, end.position) > kEndSnapDistance)
            tail.append(end);
        m_points.append(tail);
    }

private:
    struct FollowState
    {
        qreal speed = 0.0;
        qreal time = 0.0;
        bool primed = false;
    };

    // Шаг 1 (D2): отфильтрованное давление пера в момент time.
    qreal pressureAt(qreal time) const
    {
        const QList<InkPoint> &h = m_rawHistory;
        if (h.isEmpty())
            return -1.0;
        if (time >= h.constLast().timeSeconds)
            return h.constLast().pressure;
        for (qsizetype i = h.size() - 1; i > 0; --i) {
            if (time >= h[i - 1].timeSeconds) {
                // Отсчёт без давления (-1) с настоящим давлением не смешивается.
                if (!h[i - 1].hasPressure() || !h[i].hasPressure())
                    return h[i].pressure;
                const qreal span = h[i].timeSeconds - h[i - 1].timeSeconds;
                const qreal u = span > 0.0 ? (time - h[i - 1].timeSeconds) / span : 1.0;
                return h[i - 1].pressure + (h[i].pressure - h[i - 1].pressure) * u;
            }
        }
        return h.constFirst().pressure;
    }

    InkPoint smoothedSample(int i, int last) const
    {
        const int first = m_rawCount - int(m_rawHistory.size());
        const InkPoint &sample = m_rawHistory[i - first];
        if (i <= 0 || i >= last)
            return sample;
        InkPoint smoothed = sample;
        smoothed.position = (m_rawHistory[i - 1 - first].position + sample.position * 2.0
                             + m_rawHistory[i + 1 - first].position)
            / 4.0;
        return smoothed;
    }

    void settleSmoothedSamples(bool finishing)
    {
        const int last = m_rawCount - 1;
        while (m_smoothedCount <= last
               && (finishing || m_smoothedCount == 0 || m_smoothedCount < last)) {
            m_smoothTrack.append(smoothedSample(m_smoothedCount, last));
            ++m_smoothedCount;
        }
        while (m_smoothTrack.size() > kRawHistory)
            m_smoothTrack.removeFirst();
        while (!m_pendingResults.empty() && m_pendingResults.front().first < m_smoothedCount) {
            for (const ism::Result &result : m_pendingResults.front().second)
                appendNode(result);
            m_pendingResults.erase(m_pendingResults.begin());
        }
    }

    QList<InkPoint> provisionalPenTrack() const
    {
        if (!m_options.raw121)
            return m_rawHistory;
        QList<InkPoint> track = m_smoothTrack;
        for (int i = m_smoothedCount; i < m_rawCount; ++i)
            track.append(smoothedSample(i, m_rawCount - 1));
        return track;
    }

    InkPoint followedNode(const ism::Result &result, FollowState &state,
                          const QList<InkPoint> &penTrack) const
    {
        const qreal time = result.time.Value();
        const qreal speed = std::hypot(result.velocity.x, result.velocity.y);
        if (state.primed) {
            const qreal dt = qMax(0.0, time - state.time);
            state.speed += (1.0 - std::exp(-dt / kFollowSpeedTimeConstant)) * (speed - state.speed);
        } else {
            state.speed = speed;
            state.primed = true;
        }
        state.time = time;
        const qreal x = qBound(
            0.0, (state.speed - kFollowSpeedLower) / (kFollowSpeedUpper - kFollowSpeedLower), 1.0);
        const qreal share = x * x * (3.0 - 2.0 * x);
        const QPointF modeled(result.position.x, result.position.y);
        InkPoint node;
        node.position = modeled + (penAt(penTrack, time) - modeled) * share;
        node.pressure = m_lastInputPressure < 0.0 ? -1.0 : pressureAt(time);
        node.timeSeconds = qMin(time, m_lastInputTime);
        return node;
    }

    void appendNode(const ism::Result &result)
    {
        const InkPoint node =
            followedNode(result, m_follow, m_options.raw121 ? m_smoothTrack : m_rawHistory);
        if (!m_nodes.isEmpty() && distance(m_nodes.constLast().position, node.position) < 1e-3
            && m_nextSegment + 1 < m_nodes.size()) {
            m_nodes.last().pressure = node.pressure;
            m_nodes.last().timeSeconds = node.timeSeconds;
            return;
        }
        m_nodes.append(node);
    }

    InkPoint toPage(const InkPoint &node) const
    {
        return {node.position / m_scale, node.pressure, node.timeSeconds + m_timeOrigin};
    }

    template <typename Sink>
    void flattenSegment(const QList<InkPoint> &nodes, int k, Sink sink) const
    {
        const InkPoint &n1 = nodes[k];
        const InkPoint &n2 = nodes[k + 1];
        const QPointF p1 = n1.position;
        const QPointF p2 = n2.position;
        const QPointF p0 = k > 0 ? nodes[k - 1].position : p1 * 2.0 - p2;
        const QPointF p3 = k + 2 < nodes.size() ? nodes[k + 2].position : p2 * 2.0 - p1;
        const int steps = qBound(
            1, int(std::ceil(distance(p1, p2) / (m_screenSpacing * kFlattenFraction))), 4096);
        for (int i = 1; i <= steps; ++i) {
            const qreal u = qreal(i) / steps;
            InkPoint dense;
            dense.position = i == steps ? p2 : catmullRom(p0, p1, p2, p3, u);
            dense.pressure = lerpPressure(n1, n2, u);
            dense.timeSeconds = n1.timeSeconds + (n2.timeSeconds - n1.timeSeconds) * u;
            sink(toPage(dense));
        }
    }

    int cuspAmong(const InkPoint &last, const QList<InkPoint> &skipped, const QPointF &to) const
    {
        if (!m_options.cusp)
            return -1;
        qreal farthest = kCuspTolerance / m_scale;
        int cusp = -1;
        for (int i = 0; i < skipped.size(); ++i) {
            const QPointF p = skipped[i].position;
            const qreal offset = distanceToSegment(p, last.position, to);
            if (offset <= farthest || turnAngle(p - last.position, to - p) < kCuspTurn)
                continue;
            farthest = offset;
            cusp = i;
        }
        return cusp;
    }

    void walkTowards(InkPoint &last, QList<InkPoint> &skipped, const InkPoint &target,
                     QList<InkPoint> &out) const
    {
        const qreal step = spacing();
        while (true) {
            const QPointF delta = target.position - last.position;
            const qreal length = std::hypot(delta.x(), delta.y());
            if (length < step) {
                if (m_options.cusp) {
                    skipped.append(target);
                    if (skipped.size() > kMaxSkipped)
                        skipped.remove(0, kMaxSkipped / 2);
                }
                break;
            }
            const int cusp = cuspAmong(last, skipped, target.position);
            if (cusp >= 0) {
                out.append(skipped[cusp]);
                last = skipped[cusp];
                skipped.remove(0, cusp + 1);
                continue;
            }
            const qreal t = step / length;
            InkPoint next;
            next.position = last.position + delta * t;
            next.timeSeconds = last.timeSeconds + (target.timeSeconds - last.timeSeconds) * t;
            next.pressure = lerpPressure(last, target, t);
            out.append(next);
            last = next;
            skipped.clear();
        }
    }

    void flushCusps(InkPoint &last, QList<InkPoint> &skipped, const InkPoint &end,
                    QList<InkPoint> &out) const
    {
        while (!skipped.isEmpty()) {
            const int cusp = cuspAmong(last, skipped, end.position);
            if (cusp < 0)
                break;
            out.append(skipped[cusp]);
            last = skipped[cusp];
            skipped.remove(0, cusp + 1);
        }
    }

    void emitSettled(bool toEnd)
    {
        if (m_nodes.isEmpty())
            return;
        QList<InkPoint> added;
        if (m_points.isEmpty())
            m_points.append(toPage(m_nodes.constFirst()));
        InkPoint last = m_points.constLast();
        while (m_nextSegment + 1 < m_nodes.size() && (toEnd || m_nextSegment + 2 < m_nodes.size())) {
            flattenSegment(m_nodes, m_nextSegment, [&](const InkPoint &dense) {
                walkTowards(last, m_skipped, dense, added);
            });
            ++m_nextSegment;
        }
        m_points.append(added);
        const int drop = m_nextSegment - 1;
        if (drop > 0) {
            m_nodes.remove(0, drop);
            m_nextSegment -= drop;
        }
    }

    void rebuildProvisional()
    {
        m_provisional.clear();
        if (m_points.isEmpty())
            return;
        QList<InkPoint> tail = m_nodes.mid(qMax(0, m_nextSegment - 1));
        const int first = m_nextSegment > 0 ? 1 : 0;
        FollowState follow = m_follow;
        const QList<InkPoint> track = provisionalPenTrack();
        const auto push = [&](const ism::Result &result) {
            const InkPoint node = followedNode(result, follow, track);
            if (!tail.isEmpty() && distance(tail.constLast().position, node.position) < 1e-3)
                return;
            tail.append(node);
        };
        for (const auto &pending : m_pendingResults) {
            for (const ism::Result &result : pending.second)
                push(result);
        }
        m_prediction.clear();
        if (m_modeler->Predict(m_prediction).ok()) {
            for (const ism::Result &result : m_prediction)
                push(result);
        }
        InkPoint last = m_points.constLast();
        QList<InkPoint> skipped = m_skipped;
        for (int k = first; k + 1 < tail.size(); ++k) {
            flattenSegment(tail, k, [&](const InkPoint &dense) {
                walkTowards(last, skipped, dense, m_provisional);
            });
        }
        if (tail.size() > first) {
            const InkPoint end = toPage(tail.constLast());
            flushCusps(last, skipped, end, m_provisional);
            if (distance(last.position, end.position) > 1e-9)
                m_provisional.append(end);
        }
    }

    Options m_options;
    qreal m_screenSpacing = 1.2;
    qreal m_scale = 1.0;
    std::unique_ptr<ism::StrokeModeler> m_modeler;
    std::vector<ism::Result> m_results;
    std::vector<ism::Result> m_prediction;
    OneEuroFilter m_pressureFilter;
    bool m_started = false;
    bool m_finished = false;
    qreal m_timeOrigin = 0.0;
    qreal m_lastInputTime = 0.0;
    QPointF m_lastInputScreen;
    qreal m_lastInputPressure = -1.0;
    qreal m_peakPressure = -1.0;
    qreal m_peakRawPressure = -1.0;
    InkPoint m_lastRaw;
    FollowState m_follow;
    QList<InkPoint> m_rawHistory;
    int m_rawCount = 0;
    int m_smoothedCount = 0;
    QList<InkPoint> m_smoothTrack;
    std::vector<std::pair<int, std::vector<ism::Result>>> m_pendingResults;
    QList<InkPoint> m_skipped;
    QList<InkPoint> m_nodes;
    int m_nextSegment = 0;
    QList<InkPoint> m_points;
    QList<InkPoint> m_provisional;
};

class GateVariant : public lab::Variant
{
public:
    explicit GateVariant(Options options) : m_sampler(options) {}

    void begin(const lab::StrokeSetup &setup) override
    {
        m_sampler.reset();
        m_sampler.setScale(setup.scale);
        m_tessellator.setWidth(setup.width);
        // Новые штрихи пера - заострённые (шаг 1, D3), как в DocumentCanvas::beginInkAt.
        m_tessellator.setStyle(StrokeStyle::Tapered);
    }

    void addEvent(const InkPoint &raw) override { m_sampler.append(raw); }

    void wet(int from, lab::WetState &out) const override
    {
        const QList<InkPoint> &points = m_sampler.points();
        out.settledCount = int(points.size());
        for (int i = qMax(0, from); i < points.size(); ++i)
            out.settledFrom.push_back(toCenter(points[i]));
        for (const InkPoint &p : m_sampler.provisionalPoints())
            out.provisional.push_back(toCenter(p));
    }

    void finish() override
    {
        m_sampler.finish();
        if (m_sampler.isGraze())
            m_sampler.discard();
        else
            m_sampler.appendDotTwin(m_tessellator);
    }

    std::vector<lab::CenterPoint> centerline() const override
    {
        std::vector<lab::CenterPoint> out;
        for (const InkPoint &p : m_sampler.points())
            out.push_back(toCenter(p));
        return out;
    }

    lab::Geometry geometry() const override
    {
        lab::Geometry g;
        const QVector<StrokeTessellator::Vertex> strip = m_tessellator.tessellate(m_sampler.points());
        if (StrokeTessellator::isDegenerate(strip))
            return g;
        g.kind = lab::Geometry::Kind::TriangleStrip;
        for (const auto &v : strip)
            g.vertices.emplace_back(v.x, v.y);
        return g;
    }

private:
    lab::CenterPoint toCenter(const InkPoint &p) const
    {
        return {p.position, 2.0 * m_tessellator.halfWidthAt(p), p.pressure, p.timeSeconds};
    }

    GateSampler m_sampler;
    StrokeTessellator m_tessellator;
};

struct D6Copy : GateVariant
{
    D6Copy() : GateVariant(Options{false, false}) {}
};

struct D6Raw121Cusp : GateVariant
{
    D6Raw121Cusp() : GateVariant(Options{true, true}) {}
};

} // namespace

STROKE_LAB_VARIANT(D6Copy, "d6-copy",
                   "Копия StrokeSampler без D6: сверка копии с baseline по осевой");
STROKE_LAB_VARIANT(D6Raw121Cusp, "d6-raw121-cusp",
                   "D6: узел острия в нарезке (0.1 px, 45°) + окно [1,2,1]/4 на входе следования");
