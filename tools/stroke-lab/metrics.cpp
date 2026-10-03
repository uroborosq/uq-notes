#include "metrics.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace lab {

namespace {

double len(const QPointF &v) { return std::hypot(v.x(), v.y()); }

// Расстояние до ломаной с сеточным индексом отрезков.
class PolylineIndex
{
public:
    explicit PolylineIndex(std::vector<QPointF> points, double cell = 2.0)
        : m_points(std::move(points)), m_cell(cell)
    {
        for (size_t i = 0; i + 1 < m_points.size(); ++i) {
            const QPointF a = m_points[i], b = m_points[i + 1];
            const int x0 = cellOf(std::min(a.x(), b.x())), x1 = cellOf(std::max(a.x(), b.x()));
            const int y0 = cellOf(std::min(a.y(), b.y())), y1 = cellOf(std::max(a.y(), b.y()));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    m_grid[key(x, y)].push_back(int(i));
        }
    }

    bool empty() const { return m_points.size() < 1; }

    // Ближайшее расстояние; seg / u - где именно.
    double distance(const QPointF &p, int *segOut = nullptr, double *uOut = nullptr) const
    {
        if (m_points.empty())
            return std::numeric_limits<double>::infinity();
        if (m_points.size() == 1)
            return len(p - m_points[0]);
        const int cx = cellOf(p.x()), cy = cellOf(p.y());
        double best = std::numeric_limits<double>::infinity();
        int bestSeg = -1;
        double bestU = 0;
        for (int r = 0; r < 4096; ++r) {
            // Кольцо r: ячейки с max(|dx|,|dy|) == r.
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r)
                        continue;
                    const auto it = m_grid.find(key(cx + dx, cy + dy));
                    if (it == m_grid.end())
                        continue;
                    for (int s : it->second) {
                        double u;
                        const double d = segDistance(p, m_points[size_t(s)], m_points[size_t(s) + 1], u);
                        if (d < best) {
                            best = d;
                            bestSeg = s;
                            bestU = u;
                        }
                    }
                }
            }
            if (best <= r * m_cell)
                break;
            if (r > 0 && r * m_cell > 1e4)
                break;
        }
        if (segOut)
            *segOut = bestSeg;
        if (uOut)
            *uOut = bestU;
        return best;
    }

private:
    static double segDistance(const QPointF &p, const QPointF &a, const QPointF &b, double &u)
    {
        const QPointF ab = b - a;
        const double l2 = ab.x() * ab.x() + ab.y() * ab.y();
        u = l2 < 1e-18 ? 0.0 : std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / l2, 0.0, 1.0);
        return len(p - (a + ab * u));
    }
    int cellOf(double v) const { return int(std::floor(v / m_cell)); }
    static long long key(int x, int y) { return (long long)(x) * 1000003LL + y; }

    std::vector<QPointF> m_points;
    double m_cell;
    std::unordered_map<long long, std::vector<int>> m_grid;
};

// Набор значений метрики. Пустой набор даёт NaN (в JSON - null, сводка его
// пропускает): ноль от сэмпла, где мерить нечего (касание короче профиля,
// перо медленнее порога), иначе разбавлял бы средние по корпусу.
struct Stats
{
    static constexpr double kNone = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> v;
    void add(double x)
    {
        if (std::isfinite(x))
            v.push_back(x);
    }
    double mean() const { return v.empty() ? kNone : std::accumulate(v.begin(), v.end(), 0.0) / v.size(); }
    double max() const { return v.empty() ? kNone : *std::max_element(v.begin(), v.end()); }
    double rms() const
    {
        if (v.empty())
            return kNone;
        double s = 0;
        for (double x : v)
            s += x * x;
        return std::sqrt(s / v.size());
    }
    double pct(double q) const
    {
        if (v.empty())
            return kNone;
        std::vector<double> c = v;
        const size_t k = std::min(c.size() - 1, size_t(std::floor(q * (c.size() - 1) + 0.5)));
        std::nth_element(c.begin(), c.begin() + long(k), c.end());
        return c[k];
    }
};

// Ломаная без совпадающих соседей.
std::vector<QPointF> dedup(const std::vector<QPointF> &in, double eps)
{
    std::vector<QPointF> out;
    for (const QPointF &p : in)
        if (out.empty() || len(p - out.back()) > eps)
            out.push_back(p);
    return out;
}

// Равномерная нарезка по длине дуги (линейно по ломаной) с переносом значения.
void resample(const std::vector<QPointF> &pts, const std::vector<double> &vals, double step,
              std::vector<QPointF> &outPts, std::vector<double> &outVals)
{
    outPts.clear();
    outVals.clear();
    if (pts.size() < 2)
        return;
    outPts.push_back(pts[0]);
    outVals.push_back(vals.empty() ? 0.0 : vals[0]);
    double carry = 0.0; // пройдено от последней выходной точки
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const double seg = len(pts[i + 1] - pts[i]);
        double pos = step - carry;
        while (pos <= seg) {
            const double u = seg > 0 ? pos / seg : 0;
            outPts.push_back(pts[i] + (pts[i + 1] - pts[i]) * u);
            outVals.push_back(vals.empty() ? 0.0 : vals[i] + (vals[i + 1] - vals[i]) * u);
            pos += step;
        }
        carry = seg - (pos - step);
    }
}

double turnAngle(const QPointF &a, const QPointF &b)
{
    return std::atan2(a.x() * b.y() - a.y() * b.x(), a.x() * b.x() + a.y() * b.y());
}

bool nearAny(const QPointF &p, const std::vector<QPointF> &pts, double r)
{
    for (const QPointF &q : pts)
        if (len(p - q) < r)
            return true;
    return false;
}

double shoelace(const std::vector<QPointF> &p)
{
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        const QPointF &u = p[i];
        const QPointF &v = p[(i + 1) % p.size()];
        a += u.x() * v.y() - v.x() * u.y();
    }
    return a / 2.0;
}

// TV кривизны на равномерной нарезке с шагом 1 px. Возвращает сумму |dk| и
// длину учтённого участка.
void curvatureTv(const std::vector<QPointF> &uniform, const std::vector<QPointF> &corners,
                 double &tv, double &length)
{
    tv = 0;
    length = 0;
    const size_t n = uniform.size();
    if (n < 5)
        return;
    std::vector<double> k(n, std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 1; i + 1 < n; ++i) {
        if (i < 2 || i + 3 > n)
            continue; // концы по 2 px
        if (nearAny(uniform[i], corners, 4.0))
            continue;
        k[i] = turnAngle(uniform[i] - uniform[i - 1], uniform[i + 1] - uniform[i]);
    }
    for (size_t i = 1; i < n; ++i) {
        if (std::isnan(k[i]) || std::isnan(k[i - 1]))
            continue;
        tv += std::abs(k[i] - k[i - 1]);
        length += 1.0;
    }
}

// ---------------------------------------------------------------------------
// Эмуляция кадров: что видит пишущий на экране.
//
// Кадр 120 Гц защёлкивает мокрое состояние в момент t_l: в него попадают
// отсчёты, чьё настоящее время + kInputDelay <= t_l (доставка ввода до
// клиента). Фотон выходит в t_l + (L - kInputDelay), то есть L - полная
// задержка «перо -> фотон» для отсчёта, пришедшего ровно к защёлке. Фаза кадров
// относительно штриха перебирается (kFramePhases равномерно по периоду).
//
// gap     - длина дуги эталона от проекции кончика до положения пера в момент
//           фотона (то, что глаз видит как «линия не успевает»); < 0 - кончик
//           впереди пера. Только кадры, где перо быстрее 50 px/s.
// ahead   - max(0, -gap) по всем кадрам: кончик впереди настоящего пера.
// lift    - то же для кадров, где перо уже оторвалось, а отрыв ещё не
//           обработан: перо стоит в точке отрыва, так ловится «запятая».
// offpath - самая дальняя точка провизорного хвоста от эталона в окне времени
//           [t_k - 80 мс, t_k + 60 мс] (вылет мимо траектории, за петлю).
// retract - насколько хвост кадра k не совпал с линией следующего другого
//           кадра (его окончательные точки + хвост): «втягивание».
// heading - угол между направлением кончика (последние 2 px мокрой линии) и
//           касательной эталона в точке проекции кончика, градусы.
// wag     - скачок heading между соседними разными кадрами: «виляние».
// tailfinal - самая дальняя точка хвоста кадра от итоговой осевой.
constexpr double kFramePeriod = 1.0 / 120.0;
constexpr double kInputDelay = 0.002;
constexpr int kFramePhases = 4;
constexpr double kFrameLatencies[] = {0.016, 0.022, 0.028};
constexpr int kLatencyCount = int(std::size(kFrameLatencies));
constexpr int kMainLatency = 1; // L = 22 мс - для метрик, мало зависящих от L

struct FrameAcc
{
    Stats gap[kLatencyCount], gapFast[kLatencyCount], gapMs[kLatencyCount],
        ahead[kLatencyCount];
    Stats offpath, retract, wag, heading, tailFinal, liftAhead;
};

// Эталон с шагом 0.25 px: точки, время, длина дуги.
struct CoarseTruth
{
    std::vector<QPointF> p;
    std::vector<double> t, s;

    explicit CoarseTruth(const TruePath &truth)
    {
        for (size_t i = 0; i < truth.p.size(); ++i) {
            const bool lastPoint = i + 1 == truth.p.size();
            if (p.empty() || len(truth.p[i] - p.back()) >= 0.25
                || (lastPoint && len(truth.p[i] - p.back()) > 1e-9)) {
                s.push_back(p.empty() ? 0.0 : s.back() + len(truth.p[i] - p.back()));
                p.push_back(truth.p[i]);
                t.push_back(truth.t[i]);
            }
        }
        if (p.size() == 1) {
            p.push_back(p.back());
            t.push_back(truth.t.back());
            s.push_back(0.0);
        }
    }

    size_t indexAt(double time) const
    {
        const auto it = std::lower_bound(t.begin(), t.end(), time);
        return std::min(size_t(it - t.begin()), t.size() - 1);
    }
    double arcAt(double time) const
    {
        if (time <= t.front())
            return 0.0;
        if (time >= t.back())
            return s.back();
        const size_t j = indexAt(time);
        const size_t i = j - 1;
        const double u = (time - t[i]) / std::max(t[j] - t[i], 1e-12);
        return s[i] + (s[j] - s[i]) * u;
    }
    // Ближайшая точка эталона в окне времени: расстояние, дуга, время.
    double nearest(const QPointF &q, double tlo, double thi, double *arc = nullptr,
                   double *time = nullptr) const
    {
        size_t a = indexAt(tlo);
        a = a > 0 ? a - 1 : 0;
        const size_t b = std::min(indexAt(thi) + 1, p.size() - 1);
        double best = len(q - p[a]);
        double bestS = s[a], bestT = t[a];
        for (size_t i = a; i < b; ++i) {
            const QPointF ab = p[i + 1] - p[i];
            const double l2 = ab.x() * ab.x() + ab.y() * ab.y();
            const double u = l2 < 1e-18 ? 0.0
                : std::clamp(((q.x() - p[i].x()) * ab.x() + (q.y() - p[i].y()) * ab.y()) / l2, 0.0, 1.0);
            const double d = len(q - (p[i] + ab * u));
            if (d < best) {
                best = d;
                bestS = s[i] + (s[i + 1] - s[i]) * u;
                bestT = t[i] + (t[i + 1] - t[i]) * u;
            }
        }
        if (arc)
            *arc = bestS;
        if (time)
            *time = bestT;
        return best;
    }
    QPointF pointAtArc(double arc) const
    {
        if (arc <= 0.0)
            return p.front();
        if (arc >= s.back())
            return p.back();
        const size_t j = size_t(std::lower_bound(s.begin(), s.end(), arc) - s.begin());
        const size_t i = j - 1;
        const double u = (arc - s[i]) / std::max(s[j] - s[i], 1e-12);
        return p[i] + (p[j] - p[i]) * u;
    }
    QPointF tangentAt(double time) const
    {
        const size_t j = indexAt(time);
        const size_t i = j > 0 ? j - 1 : 0;
        const size_t k = std::min(j + 1, p.size() - 1);
        const QPointF d = p[k] - p[i];
        const double l = len(d);
        return l > 1e-9 ? d / l : QPointF(1, 0);
    }
};

double distanceToPolyline(const QPointF &q, const std::vector<QPointF> &line)
{
    if (line.empty())
        return std::numeric_limits<double>::infinity();
    double best = len(q - line[0]);
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const QPointF ab = line[i + 1] - line[i];
        const double l2 = ab.x() * ab.x() + ab.y() * ab.y();
        const double u = l2 < 1e-18 ? 0.0
            : std::clamp(((q.x() - line[i].x()) * ab.x() + (q.y() - line[i].y()) * ab.y()) / l2, 0.0, 1.0);
        best = std::min(best, len(q - (line[i] + ab * u)));
    }
    return best;
}

// Мокрая линия кадра около кончика: окончательные точки с from и хвост.
std::vector<QPointF> wetLine(const std::vector<QPointF> &center, const EventRecord &e, int from)
{
    std::vector<QPointF> line;
    const int settled = std::min(e.settledCount, int(center.size()));
    for (int i = std::max(0, from); i < settled; ++i)
        line.push_back(center[size_t(i)]);
    line.insert(line.end(), e.provisional.begin(), e.provisional.end());
    if (line.empty())
        line.push_back(e.tip);
    return line;
}

// Направление кончика: от точки мокрой линии в 2 px позади до кончика.
QPointF tipDirection(const std::vector<QPointF> &center, const EventRecord &e)
{
    const std::vector<QPointF> line = wetLine(center, e, e.settledCount - 40);
    const QPointF tip = line.back();
    double acc = 0;
    for (size_t i = line.size() - 1; i > 0; --i) {
        acc += len(line[i] - line[i - 1]);
        if (acc >= 2.0) {
            const QPointF d = tip - line[i - 1];
            const double l = len(d);
            return l > 1e-9 ? d / l : QPointF();
        }
    }
    return QPointF();
}

void accumulateFrames(const SampleStroke &stroke, const StrokeRun &run,
                      const std::vector<QPointF> &center, const PolylineIndex &centerIndex,
                      FrameAcc &acc)
{
    const size_t n = std::min(run.events.size(), stroke.events.size());
    if (n < 2 || stroke.truth.p.size() < 2)
        return;
    const CoarseTruth truth(stroke.truth);
    const double tEnd = stroke.truth.t.back();

    // Свойства мокрого состояния после отсчёта k - не зависят от кадров.
    std::vector<double> offpath(n, -1.0), tailFinal(n, -1.0);
    const auto stateOffpath = [&](size_t k) {
        if (offpath[k] < 0) {
            const double tk = stroke.eventTrueTime[k];
            double worst = 0;
            for (const QPointF &p : run.events[k].provisional)
                worst = std::max(worst, truth.nearest(p, tk - 0.08, tk + 0.06));
            offpath[k] = worst;
        }
        return offpath[k];
    };
    const auto stateTailFinal = [&](size_t k) {
        if (tailFinal[k] < 0) {
            double worst = 0;
            for (const QPointF &p : run.events[k].provisional)
                worst = std::max(worst, centerIndex.distance(p));
            tailFinal[k] = worst;
        }
        return tailFinal[k];
    };

    for (int li = 0; li < kLatencyCount; ++li) {
        const double L = kFrameLatencies[li];
        for (int ph = 0; ph < kFramePhases; ++ph) {
            const double t0 = stroke.eventTrueTime[0] + kInputDelay + kFramePeriod * ph / kFramePhases;
            const double tLast = stroke.eventTrueTime[n - 1] + kInputDelay;
            long prevK = -1;
            bool prevValid = false;
            double prevHeading = 0;
            size_t k = 0;
            for (double tl = t0; tl < tLast; tl += kFramePeriod) {
                while (k + 1 < n && stroke.eventTrueTime[k + 1] + kInputDelay <= tl)
                    ++k;
                const EventRecord &e = run.events[k];
                const double tk = stroke.eventTrueTime[k];
                const double tph = tl + L - kInputDelay;
                const double sPen = truth.arcAt(tph);
                double sTip = 0, tTip = 0;
                // Окно проекции узкое (±30 мс от отсчёта): на развороте путь
                // идёт обратно по себе, и широкое окно цепляло бы чужой проход.
                truth.nearest(e.tip, tk - 0.03, tk + 0.03, &sTip, &tTip);
                const double gap = sPen - sTip;
                acc.ahead[li].add(std::max(0.0, -gap));
                if (tph >= tEnd) {
                    // Перо уже оторвалось, а отрыв ещё не обработан.
                    if (li == kMainLatency)
                        acc.liftAhead.add(std::max(0.0, -gap));
                    continue;
                }
                const size_t si = std::min(
                    size_t(std::lower_bound(stroke.truth.t.begin(), stroke.truth.t.end(), tph)
                           - stroke.truth.t.begin()),
                    stroke.truth.t.size() - 1);
                const double speed = stroke.truth.speed[si];
                if (speed > 50.0) {
                    acc.gap[li].add(gap);
                    acc.gapMs[li].add((tph - tTip) * 1000.0);
                    if (speed >= 150.0 && speed <= 600.0)
                        acc.gapFast[li].add(gap);
                }
                if (li != kMainLatency)
                    continue;

                acc.offpath.add(stateOffpath(k));
                acc.tailFinal.add(stateTailFinal(k));
                const bool changed = prevK >= 0 && size_t(prevK) != k;
                if (changed) {
                    // Втягивание: хвост прошлого кадра против линии этого.
                    const EventRecord &pe = run.events[size_t(prevK)];
                    const std::vector<QPointF> line = wetLine(center, e, pe.settledCount - 4);
                    double worst = 0;
                    for (const QPointF &p : pe.provisional)
                        worst = std::max(worst, distanceToPolyline(p, line));
                    acc.retract.add(worst);
                }
                // Курс кончика против касательной эталона в точке проекции
                // кончика; виляние - скачок этой ошибки между кадрами.
                const QPointF dir = tipDirection(center, e);
                // Хорда эталона на тех же 2 px до проекции кончика: на острие
                // сравнивается излом с изломом, а не с касательной.
                const QPointF td = truth.pointAtArc(sTip) - truth.pointAtArc(sTip - 2.0);
                const double penSpeed = stroke.truth.speed[std::min(
                    size_t(std::lower_bound(stroke.truth.t.begin(), stroke.truth.t.end(), tk)
                           - stroke.truth.t.begin()),
                    stroke.truth.t.size() - 1)];
                const bool valid = !dir.isNull() && len(td) > 1.0 && penSpeed > 50.0;
                const double heading = valid ? turnAngle(td, dir) : 0.0;
                if (valid)
                    acc.heading.add(std::abs(heading) * 180.0 / M_PI);
                if (changed && valid && prevValid)
                    acc.wag.add(std::abs(std::remainder(heading - prevHeading, 2 * M_PI)) * 180.0 / M_PI);
                prevValid = valid;
                prevHeading = heading;
                prevK = long(k);
            }
        }
    }
}

double median(const Stats &s) { return s.pct(0.5); }

} // namespace

double truthWidthPx(double pressure, double scale, const MetricsOptions &options)
{
    const double p = std::clamp(pressure, 0.0, 1.0);
    return options.width * (options.minWidthFactor + (1.0 - options.minWidthFactor) * p) * scale;
}

QJsonObject computeMetrics(const Sample &sample, const std::vector<StrokeRun> &runs, double scale,
                           const MetricsOptions &options)
{
    Stats dev, cover, lagPx, lagTruePx, lagMs, wetFinal, cpu, dirty, corner, facet, turn,
        widthHf, widthErr, widthErrRel, widthTouch, widthLift, widthEndJump, widthEndJumpGt,
        pressureLag, pressureErr;
    // Профиль толщины у концов (apps-survey): ошибка толщины относительно
    // эталона в той же точке эталона (проекция), отдельно в первых и последних
    // kEndZone px дуги эталона и в теле; наибольший скачок толщины на 1 px
    // дуги; коэффициент вариации толщины в теле.
    Stats widthStartErr, widthEndErr, widthBodyErr, widthCv, widthCvGt;
    double widthStepMax = Stats::kNone, widthStepMaxGt = Stats::kNone;
    int facetCount = 0;
    int mutations = 0;
    int finalVertices = 0;
    int centerPoints = 0;
    double curvTv = 0, curvLen = 0, curvTvGt = 0, curvLenGt = 0;
    double widthTv = 0, widthLen = 0, widthTvGt = 0, widthLenGt = 0;
    double widthSum = 0, widthSumGt = 0;
    int widthCount = 0, widthCountGt = 0;
    double finishUs = 0;
    // Профиль толщины у концов и слежение за эталонной толщиной.
    Stats riseV, riseGt, fallV, fallGt, endRatioV, endRatioGt, startRatioV, startRatioGt, trackAbs,
        trackRel;
    double areaRatio = std::numeric_limits<double>::quiet_NaN();
    FrameAcc frames;

    // Острия (corners) - на весь сэмпл.
    const std::vector<QPointF> &corners = sample.corners;
    std::vector<double> cornerBest(corners.size(), std::numeric_limits<double>::infinity());

    for (size_t si = 0; si < sample.strokes.size() && si < runs.size(); ++si) {
        const SampleStroke &stroke = sample.strokes[si];
        const StrokeRun &run = runs[si];

        // Эталон: без повторов, с временем.
        std::vector<QPointF> gt;
        std::vector<double> gtTime, gtPressure;
        for (size_t i = 0; i < stroke.truth.p.size(); ++i) {
            if (gt.empty() || len(stroke.truth.p[i] - gt.back()) > 0.02) {
                gt.push_back(stroke.truth.p[i]);
                gtTime.push_back(stroke.truth.t[i]);
                gtPressure.push_back(stroke.truth.pressure[i]);
            }
        }
        // Касание без движения (путь эталона < 0.02 px): вторая точка рядом,
        // с тем же временем и давлением - массивы эталона одной длины.
        if (gt.empty()) {
            gt.push_back(stroke.truth.p.front());
            gtTime.push_back(stroke.truth.t.front());
            gtPressure.push_back(stroke.truth.pressure.front());
        }
        if (gt.size() < 2) {
            gt.push_back(gt.back() + QPointF(1e-3, 0));
            gtTime.push_back(gtTime.back());
            gtPressure.push_back(gtPressure.back());
        }
        const PolylineIndex gtIndex(gt);

        std::vector<QPointF> center;
        std::vector<double> centerWidth;
        for (const CenterPoint &c : run.centerline) {
            center.push_back(c.pos * scale);
            centerWidth.push_back(c.width * scale);
        }
        centerPoints += int(center.size());
        finalVertices += run.finalVertices;
        mutations += run.settledMutations;
        finishUs += run.finishMicros;

        const std::vector<QPointF> centerClean = dedup(center, 1e-6);
        const PolylineIndex centerIndex(centerClean);

        // 1. Отклонение осевой от эталона и обратное (покрытие эталона).
        for (const QPointF &p : center)
            dev.add(gtIndex.distance(p));
        {
            std::vector<QPointF> gtU;
            std::vector<double> gtUv;
            resample(gt, {}, 0.5, gtU, gtUv);
            for (const QPointF &p : gtU)
                cover.add(centerIndex.distance(p));
        }

        // 2. Грани: скачок угла поворота относительно соседей.
        {
            const std::vector<QPointF> c = dedup(center, 0.02);
            std::vector<double> arc(c.size(), 0.0);
            for (size_t i = 1; i < c.size(); ++i)
                arc[i] = arc[i - 1] + len(c[i] - c[i - 1]);
            const double total = arc.empty() ? 0.0 : arc.back();
            std::vector<double> th(c.size(), std::numeric_limits<double>::quiet_NaN());
            for (size_t i = 1; i + 1 < c.size(); ++i)
                th[i] = turnAngle(c[i] - c[i - 1], c[i + 1] - c[i]);
            for (size_t i = 2; i + 2 < c.size(); ++i) {
                if (arc[i] < 2.0 || total - arc[i] < 2.0 || nearAny(c[i], corners, 4.0))
                    continue;
                const double f = std::abs(th[i] - (th[i - 1] + th[i + 1]) / 2.0) * 180.0 / M_PI;
                facet.add(f);
                if (f > 60.0 && qEnvironmentVariableIsSet("STROKE_LAB_DEBUG_FACETS"))
                    std::fprintf(stderr, "facet %s stroke %zu at (%.1f,%.1f): %.1f deg\n",
                                 sample.id.c_str(), si, c[i].x(), c[i].y(), f);
                turn.add(std::abs(th[i]) * 180.0 / M_PI);
                if (f > 2.0)
                    ++facetCount;
            }
        }

        // 3. Шероховатость кривизны, вариант и эталон, шаг 1 px.
        {
            std::vector<QPointF> u;
            std::vector<double> uv;
            resample(dedup(center, 1e-6), {}, 1.0, u, uv);
            double tv, l;
            curvatureTv(u, corners, tv, l);
            curvTv += tv;
            curvLen += l;
            resample(gt, {}, 1.0, u, uv);
            curvatureTv(u, corners, tv, l);
            curvTvGt += tv;
            curvLenGt += l;
        }

        // 4. Острые углы: от эталонной вершины до осевой.
        for (size_t ci = 0; ci < corners.size(); ++ci)
            cornerBest[ci] = std::min(cornerBest[ci], centerIndex.distance(corners[ci]));

        // 6. Толщина: TV и высокочастотная составляющая на шаге 1 px.
        {
            std::vector<QPointF> u;
            std::vector<double> w;
            // Совпадающие точки убираются вместе с толщиной.
            std::vector<QPointF> cp;
            std::vector<double> cw;
            for (size_t i = 0; i < center.size(); ++i) {
                if (cp.empty() || len(center[i] - cp.back()) > 1e-6) {
                    cp.push_back(center[i]);
                    cw.push_back(centerWidth[i]);
                }
            }
            resample(cp, cw, 1.0, u, w);
            const std::vector<double> wv = w;
            for (size_t i = 1; i < w.size(); ++i) {
                widthTv += std::abs(w[i] - w[i - 1]);
                widthLen += 1.0;
            }
            for (double x : w) {
                widthSum += x;
                ++widthCount;
            }
            for (size_t i = 4; i + 4 < w.size(); ++i) {
                double avg = 0;
                for (size_t j = i - 4; j <= i + 4; ++j)
                    avg += w[j];
                widthHf.add(w[i] - avg / 9.0);
            }
            std::vector<double> gw;
            for (double p : gtPressure)
                gw.push_back(truthWidthPx(p, scale, options));
            resample(gt, gw, 1.0, u, w);
            for (size_t i = 1; i < w.size(); ++i) {
                widthTvGt += std::abs(w[i] - w[i - 1]);
                widthLenGt += 1.0;
            }
            for (double x : w) {
                widthSumGt += x;
                ++widthCountGt;
            }
            // Концы: длина дуги до 90% медианы толщины от начала и от конца,
            // средняя толщина первых/последних 2 px к медиане. Слежение: RMS
            // разности толщины варианта и эталона при сопоставлении по доле
            // длины дуги.
            const auto profile = [](const std::vector<double> &x, Stats &rise, Stats &fall,
                                    Stats &startRatio, Stats &endRatio) {
                if (x.size() < 10)
                    return;
                std::vector<double> s = x;
                std::nth_element(s.begin(), s.begin() + long(s.size() / 2), s.end());
                const double med = s[s.size() / 2];
                if (med <= 0)
                    return;
                size_t i = 0;
                while (i < x.size() && x[i] < 0.9 * med)
                    ++i;
                rise.add(double(i));
                size_t j = 0;
                while (j < x.size() && x[x.size() - 1 - j] < 0.9 * med)
                    ++j;
                fall.add(double(j));
                startRatio.add((x[0] + x[1] + x[2]) / 3.0 / med);
                endRatio.add((x[x.size() - 1] + x[x.size() - 2] + x[x.size() - 3]) / 3.0 / med);
            };
            profile(wv, riseV, fallV, startRatioV, endRatioV);
            profile(w, riseGt, fallGt, startRatioGt, endRatioGt);
            if (wv.size() >= 10 && w.size() >= 10) {
                double sq = 0, mean = 0;
                for (size_t i = 0; i < wv.size(); ++i) {
                    const size_t j = size_t(std::lround(double(i) * double(w.size() - 1) / double(wv.size() - 1)));
                    sq += (wv[i] - w[j]) * (wv[i] - w[j]);
                    mean += w[j];
                }
                trackAbs.add(std::sqrt(sq / double(wv.size())));
                trackRel.add(std::sqrt(sq / double(wv.size())) / std::max(mean / double(wv.size()), 1e-9));
            }
        }

        // 6b. Профиль толщины: концы против тела, по проекции на эталон.
        {
            constexpr double kEndZone = 10.0; // px дуги эталона
            std::vector<double> gtArc(gt.size(), 0.0);
            for (size_t i = 1; i < gt.size(); ++i)
                gtArc[i] = gtArc[i - 1] + len(gt[i] - gt[i - 1]);
            const double total = gtArc.back();
            const double zone = std::min(kEndZone, total / 3.0);
            double maxGtW = 0.0;
            for (double p : gtPressure)
                maxGtW = std::max(maxGtW, truthWidthPx(p, scale, options));
            const double bodyMargin = 3.0 * maxGtW;
            std::vector<double> bodyW, bodyWGt;
            for (size_t i = 0; i < center.size(); ++i) {
                int seg = -1;
                double u = 0.0;
                gtIndex.distance(center[i], &seg, &u);
                if (seg < 0)
                    continue;
                const size_t a = size_t(seg), b = std::min(size_t(seg) + 1, gt.size() - 1);
                const double s = gtArc[a] + (gtArc[b] - gtArc[a]) * u;
                const double pr = gtPressure[a] + (gtPressure[b] - gtPressure[a]) * u;
                const double wGt = truthWidthPx(pr, scale, options);
                const double err = std::abs(centerWidth[i] - wGt);
                if (s < zone)
                    widthStartErr.add(err);
                else if (total - s < zone)
                    widthEndErr.add(err);
                else
                    widthBodyErr.add(err);
                if (s > bodyMargin && total - s > bodyMargin) {
                    bodyW.push_back(centerWidth[i]);
                    bodyWGt.push_back(wGt);
                }
            }
            const auto cv = [](const std::vector<double> &v) {
                if (v.size() < 4)
                    return std::numeric_limits<double>::quiet_NaN();
                const double m = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
                double q = 0.0;
                for (double x : v)
                    q += (x - m) * (x - m);
                return m > 0 ? std::sqrt(q / v.size()) / m : std::numeric_limits<double>::quiet_NaN();
            };
            widthCv.add(cv(bodyW));
            widthCvGt.add(cv(bodyWGt));
            // Скачок толщины на 1 px дуги осевой.
            std::vector<QPointF> cp, u;
            std::vector<double> cw, w;
            for (size_t i = 0; i < center.size(); ++i) {
                if (cp.empty() || len(center[i] - cp.back()) > 1e-6) {
                    cp.push_back(center[i]);
                    cw.push_back(centerWidth[i]);
                }
            }
            resample(cp, cw, 1.0, u, w);
            for (size_t i = 1; i < w.size(); ++i)
                widthStepMax = std::fmax(widthStepMax, std::abs(w[i] - w[i - 1]));
            std::vector<double> gw;
            for (double p : gtPressure)
                gw.push_back(truthWidthPx(p, scale, options));
            resample(gt, gw, 1.0, u, w);
            for (size_t i = 1; i < w.size(); ++i)
                widthStepMaxGt = std::fmax(widthStepMaxGt, std::abs(w[i] - w[i - 1]));
        }

        // 6б. Следование толщины за давлением (exp/dry-refine).
        //  - width_err: |толщина варианта − эталонная толщина в ближайшей
        //    точке эталона| вдоль осевой (шаг 1 px), экранные px;
        //  - pressure_lag_ms: сдвиг δ, при котором давление осевой лучше всего
        //    совпадает с эталоном p_gt(t − δ); время осевой переводится во
        //    время эталона через среднюю разность меток отсчётов;
        //  - торцы: толщина первой/последней точки осевой к эталонной толщине
        //    на касании/отрыве и наибольший скачок толщины на последних 3 точках.
        {
            std::vector<double> gw;
            for (double p : gtPressure)
                gw.push_back(truthWidthPx(p, scale, options));
            std::vector<QPointF> cp, u;
            std::vector<double> cw, w;
            for (size_t i = 0; i < center.size(); ++i) {
                if (cp.empty() || len(center[i] - cp.back()) > 1e-6) {
                    cp.push_back(center[i]);
                    cw.push_back(centerWidth[i]);
                }
            }
            resample(cp, cw, 1.0, u, w);
            for (size_t i = 0; i < u.size(); ++i) {
                int seg = -1;
                double uu = 0;
                gtIndex.distance(u[i], &seg, &uu);
                if (seg < 0)
                    continue;
                const double g = gw[size_t(seg)] + (gw[size_t(seg) + 1] - gw[size_t(seg)]) * uu;
                widthErr.add(std::abs(w[i] - g));
                widthErrRel.add(std::abs(w[i] - g) / std::max(g, 1e-6));
            }
            if (!centerWidth.empty() && !gw.empty()) {
                widthTouch.add(centerWidth.front() / std::max(gw.front(), 1e-6));
                widthLift.add(centerWidth.back() / std::max(gw.back(), 1e-6));
                double jump = 0;
                for (size_t i = centerWidth.size() >= 4 ? centerWidth.size() - 3 : 1;
                     i < centerWidth.size(); ++i)
                    jump = std::max(jump, std::abs(centerWidth[i] - centerWidth[i - 1])
                                              / std::max(centerWidth[i - 1], 1e-6));
                widthEndJump.add(jump);
                // То же у эталона, нарезанного с шагом 1.2 px (как осевая на
                // масштабе 1): насколько скачок на конце задан самим нажимом.
                std::vector<QPointF> gu;
                std::vector<double> gwu;
                resample(gt, gw, 1.2, gu, gwu);
                gwu.push_back(gw.back());
                double jumpGt = 0;
                for (size_t i = gwu.size() >= 4 ? gwu.size() - 3 : 1; i < gwu.size(); ++i)
                    jumpGt = std::max(jumpGt, std::abs(gwu[i] - gwu[i - 1]) / std::max(gwu[i - 1], 1e-6));
                widthEndJumpGt.add(jumpGt);
            }
            // Сдвиг давления.
            double offset = 0;
            int n = 0;
            for (size_t k = 0; k < stroke.events.size() && k < stroke.eventTrueTime.size(); ++k) {
                offset += stroke.events[k].timeSeconds - stroke.eventTrueTime[k];
                ++n;
            }
            if (n > 0)
                offset /= n;
            const auto pAt = [&](double t) {
                const std::vector<double> &tt = stroke.truth.t;
                if (t <= tt.front())
                    return stroke.truth.pressure.front();
                if (t >= tt.back())
                    return stroke.truth.pressure.back();
                const size_t j = size_t(std::upper_bound(tt.begin(), tt.end(), t) - tt.begin());
                const double a = (t - tt[j - 1]) / std::max(tt[j] - tt[j - 1], 1e-12);
                return stroke.truth.pressure[j - 1]
                    + (stroke.truth.pressure[j] - stroke.truth.pressure[j - 1]) * a;
            };
            double bestErr = std::numeric_limits<double>::infinity(), bestShift = 0, zeroErr = 0;
            for (int s = -60; s <= 240; ++s) {
                const double shift = s * 0.0005;
                double e = 0;
                int m = 0;
                for (const CenterPoint &c : run.centerline) {
                    if (c.pressure < 0)
                        continue;
                    e += std::abs(c.pressure - pAt(c.time - offset - shift));
                    ++m;
                }
                if (m == 0)
                    break;
                e /= m;
                if (s == 0)
                    zeroErr = e;
                if (e < bestErr) {
                    bestErr = e;
                    bestShift = shift;
                }
            }
            if (std::isfinite(bestErr)) {
                pressureLag.add(bestShift * 1000.0);
                pressureErr.add(zeroErr);
            }
        }

        // 5. Площадь замкнутой буквы - по первому штриху.
        if (si == 0 && sample.hasTag("closed") && center.size() > 3) {
            const double a = std::abs(shoelace(center));
            const double b = std::abs(shoelace(gt));
            if (b > 1e-6)
                areaRatio = a / b;
        }

        // 7. Мокрый штрих: лаг кончика и расхождение с итогом.
        for (size_t k = 0; k < run.events.size() && k < stroke.events.size(); ++k) {
            const EventRecord &e = run.events[k];
            cpu.add(e.cpuMicros);
            dirty.add(e.wetDirtyVertices);
            const QPointF pen = stroke.events[k].position;
            const double tk = stroke.eventTrueTime[k];
            lagPx.add(len(e.tip - pen));
            // Эталон в момент отсчёта.
            {
                const auto it = std::lower_bound(gtTime.begin(), gtTime.end(), tk);
                const size_t j = std::min(size_t(it - gtTime.begin()), gt.size() - 1);
                lagTruePx.add(len(e.tip - gt[j]));
                // Лаг во времени: ближайшая к кончику точка эталона в окне
                // [tk - 60 мс, tk + 10 мс]. Окно узкое, чтобы на повторных
                // петлях не схватить прошлый виток, и не только назад, чтобы
                // шум не давал систематически положительный лаг. Только там,
                // где перо движется быстрее 50 px/s: в покое время не
                // определено.
                const auto ts = std::lower_bound(stroke.truth.t.begin(), stroke.truth.t.end(), tk);
                const size_t ti = std::min(size_t(ts - stroke.truth.t.begin()), stroke.truth.t.size() - 1);
                if (stroke.truth.speed[ti] > 50.0) {
                    double best = std::numeric_limits<double>::infinity();
                    double bestT = tk;
                    const auto lo = std::lower_bound(gtTime.begin(), gtTime.end(), tk - 0.06);
                    for (size_t i = size_t(lo - gtTime.begin()); i < gt.size() && gtTime[i] <= tk + 0.01; ++i) {
                        const double d = len(e.tip - gt[i]);
                        if (d < best) {
                            best = d;
                            bestT = gtTime[i];
                        }
                    }
                    lagMs.add((tk - bestT) * 1000.0);
                }
            }
            double worst = 0;
            for (const QPointF &p : e.provisional)
                worst = std::max(worst, centerIndex.distance(p));
            wetFinal.add(worst);
        }

        // 8. Кадры: зазор перо -> кончик, перелёт, втягивание, виляние.
        accumulateFrames(stroke, run, center, centerIndex, frames);
    }

    for (size_t ci = 0; ci < cornerBest.size(); ++ci) {
        corner.add(cornerBest[ci]);
        if (qEnvironmentVariableIsSet("STROKE_LAB_DEBUG_CORNERS"))
            std::fprintf(stderr, "corner %s #%zu (%.1f,%.1f): %.3f\n", sample.id.c_str(), ci,
                         corners[ci].x(), corners[ci].y(), cornerBest[ci]);
    }

    QJsonObject m;
    const auto r = [](double v, int digits = 3) {
        if (!std::isfinite(v))
            return QJsonValue(QJsonValue::Null);
        const double f = std::pow(10.0, digits);
        return QJsonValue(std::round(v * f) / f);
    };
    m["centerline_dev_mean_px"] = r(dev.mean());
    m["centerline_dev_rms_px"] = r(dev.rms());
    m["centerline_dev_max_px"] = r(dev.max());
    m["truth_coverage_mean_px"] = r(cover.mean());
    m["truth_coverage_max_px"] = r(cover.max());
    m["facet_count_2deg"] = facetCount;
    m["facet_max_deg"] = r(facet.max());
    m["facet_p99_deg"] = r(facet.pct(0.99));
    m["turn_max_deg"] = r(turn.max());
    m["curvature_tv_per100px"] = r(curvLen > 0 ? curvTv / curvLen * 100.0 : Stats::kNone);
    m["curvature_tv_truth_per100px"] = r(curvLenGt > 0 ? curvTvGt / curvLenGt * 100.0 : Stats::kNone);
    {
        // Сумма |dk| в радианах на 1 px шага, на 100 px длины. Отношение - с
        // добавкой 1, иначе на прямой (у эталона почти 0) оно бесконечно.
        const double tv = curvLen > 0 ? curvTv / curvLen * 100.0 : Stats::kNone;
        const double tvGt = curvLenGt > 0 ? curvTvGt / curvLenGt * 100.0 : Stats::kNone;
        m["curvature_tv_excess_per100px"] = r(tv - tvGt);
        m["curvature_tv_ratio"] = r((tv + 1.0) / (tvGt + 1.0));
    }
    m["corner_count"] = int(corners.size());
    m["corner_dist_mean_px"] = corners.empty() ? QJsonValue() : r(corner.mean());
    m["corner_dist_max_px"] = corners.empty() ? QJsonValue() : r(corner.max());
    if (sample.hasTag("small")) {
        m["shape_dev_mean_pct"] = r(dev.mean() / sample.size * 100.0, 2);
        m["shape_hausdorff_pct"] = r(std::max(dev.max(), cover.max()) / sample.size * 100.0, 2);
    }
    if (sample.hasTag("closed"))
        m["closed_area_ratio"] = r(areaRatio);
    m["width_tv_per100px"] = r(widthLen > 0 ? widthTv / widthLen * 100.0 : Stats::kNone);
    m["width_tv_truth_per100px"] = r(widthLenGt > 0 ? widthTvGt / widthLenGt * 100.0 : Stats::kNone);
    m["width_hf_rms_px"] = r(widthHf.rms(), 4);
    m["width_mean_ratio"] = r(widthCount > 0 && widthSumGt > 0
                                  ? (widthSum / widthCount) / (widthSumGt / widthCountGt)
                                  : std::numeric_limits<double>::quiet_NaN());
    m["width_rise_px"] = r(riseV.mean(), 2);
    m["width_rise_truth_px"] = r(riseGt.mean(), 2);
    m["width_fall_px"] = r(fallV.mean(), 2);
    m["width_fall_truth_px"] = r(fallGt.mean(), 2);
    m["width_start_ratio"] = r(startRatioV.mean());
    m["width_start_ratio_truth"] = r(startRatioGt.mean());
    m["width_end_ratio"] = r(endRatioV.mean());
    m["width_end_ratio_truth"] = r(endRatioGt.mean());
    m["width_track_rms_px"] = r(trackAbs.mean(), 4);
    m["width_track_rms_rel"] = r(trackRel.mean(), 4);
    m["width_start_err_px"] = r(widthStartErr.mean(), 4);
    m["width_end_err_px"] = r(widthEndErr.mean(), 4);
    m["width_body_err_px"] = r(widthBodyErr.mean(), 4);
    m["width_step_max_px"] = r(widthStepMax, 4);
    m["width_step_max_truth_px"] = r(widthStepMaxGt, 4);
    m["width_body_cv"] = r(widthCv.mean(), 4);
    m["width_body_cv_truth"] = r(widthCvGt.mean(), 4);
    m["width_err_mean_px"] = r(widthErr.mean(), 4);
    m["width_err_rel_mean_pct"] = r(widthErrRel.mean() * 100.0, 2);
    m["width_touch_ratio"] = r(widthTouch.mean());
    m["width_lift_ratio"] = r(widthLift.mean());
    m["width_end_jump_max_pct"] = r(widthEndJump.max() * 100.0, 2);
    m["width_end_jump_truth_max_pct"] = r(widthEndJumpGt.max() * 100.0, 2);
    m["pressure_lag_ms"] = r(pressureLag.mean(), 2);
    m["pressure_err_mean"] = r(pressureErr.mean(), 4);
    m["wet_lag_px_mean"] = r(lagPx.mean());
    m["wet_lag_px_p95"] = r(lagPx.pct(0.95));
    m["wet_lag_px_max"] = r(lagPx.max());
    m["wet_lag_truth_px_mean"] = r(lagTruePx.mean());
    m["wet_lag_ms_mean"] = r(lagMs.mean(), 2);
    m["wet_lag_ms_p95"] = r(lagMs.pct(0.95), 2);
    m["wet_vs_final_mean_px"] = r(wetFinal.mean());
    m["wet_vs_final_max_px"] = r(wetFinal.max());
    for (int li = 0; li < kLatencyCount; ++li) {
        const QString suffix = QStringLiteral("_L%1").arg(int(std::lround(kFrameLatencies[li] * 1000)));
        m["frame_gap_px_mean" + suffix] = r(frames.gap[li].mean());
        m["frame_gap_px_med" + suffix] = r(median(frames.gap[li]));
        m["frame_gap_px_p95" + suffix] = r(frames.gap[li].pct(0.95));
        m["frame_gap_fast_px_med" + suffix] =
            frames.gapFast[li].v.empty() ? QJsonValue() : r(median(frames.gapFast[li]));
        m["frame_gap_ms_med" + suffix] = r(median(frames.gapMs[li]), 2);
        m["frame_ahead_px_p99" + suffix] = r(frames.ahead[li].pct(0.99));
        m["frame_ahead_px_max" + suffix] = r(frames.ahead[li].max());
    }
    m["frame_offpath_px_p99"] = r(frames.offpath.pct(0.99));
    m["frame_offpath_px_max"] = r(frames.offpath.max());
    m["frame_retract_px_p95"] = r(frames.retract.pct(0.95));
    m["frame_retract_px_p99"] = r(frames.retract.pct(0.99));
    m["frame_wag_deg_p95"] = r(frames.wag.pct(0.95));
    m["frame_heading_err_deg_p95"] = r(frames.heading.pct(0.95));
    m["frame_tail_vs_final_px_p95"] = r(frames.tailFinal.pct(0.95));
    m["frame_tail_vs_final_px_p99"] = r(frames.tailFinal.pct(0.99));
    m["frame_lift_ahead_px_max"] = r(frames.liftAhead.max());
    m["settled_mutations"] = mutations;
    m["cpu_us_per_event_mean"] = r(cpu.mean(), 2);
    m["cpu_us_per_event_p50"] = r(cpu.pct(0.5), 2);
    m["cpu_us_per_event_p99"] = r(cpu.pct(0.99), 2);
    m["cpu_us_per_event_max"] = r(cpu.max(), 2);
    m["cpu_us_finish"] = r(finishUs, 2);
    m["events"] = int(cpu.v.size());
    m["final_vertices"] = finalVertices;
    m["centerline_points"] = centerPoints;
    m["wet_dirty_vertices_mean"] = r(dirty.mean(), 1);
    return m;
}

} // namespace lab
