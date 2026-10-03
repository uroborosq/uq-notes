#include "metrics.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
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

struct Stats
{
    std::vector<double> v;
    void add(double x)
    {
        if (std::isfinite(x))
            v.push_back(x);
    }
    double mean() const { return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / v.size(); }
    double max() const { return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()); }
    double rms() const
    {
        if (v.empty())
            return 0.0;
        double s = 0;
        for (double x : v)
            s += x * x;
        return std::sqrt(s / v.size());
    }
    double pct(double q) const
    {
        if (v.empty())
            return 0.0;
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
        widthHf;
    int facetCount = 0;
    int mutations = 0;
    int finalVertices = 0;
    int centerPoints = 0;
    double curvTv = 0, curvLen = 0, curvTvGt = 0, curvLenGt = 0;
    double widthTv = 0, widthLen = 0, widthTvGt = 0, widthLenGt = 0;
    double widthSum = 0, widthSumGt = 0;
    int widthCount = 0, widthCountGt = 0;
    double finishUs = 0;
    double areaRatio = std::numeric_limits<double>::quiet_NaN();

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
        if (gt.size() < 2)
            gt.push_back(gt.empty() ? stroke.truth.p.front() : gt.back() + QPointF(1e-3, 0));
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
    }

    for (double d : cornerBest)
        corner.add(d);

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
    m["curvature_tv_per100px"] = r(curvLen > 0 ? curvTv / curvLen * 100.0 : 0.0);
    m["curvature_tv_truth_per100px"] = r(curvLenGt > 0 ? curvTvGt / curvLenGt * 100.0 : 0.0);
    {
        // Сумма |dk| в радианах на 1 px шага, на 100 px длины. Отношение - с
        // добавкой 1, иначе на прямой (у эталона почти 0) оно бесконечно.
        const double tv = curvLen > 0 ? curvTv / curvLen * 100.0 : 0.0;
        const double tvGt = curvLenGt > 0 ? curvTvGt / curvLenGt * 100.0 : 0.0;
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
    m["width_tv_per100px"] = r(widthLen > 0 ? widthTv / widthLen * 100.0 : 0.0);
    m["width_tv_truth_per100px"] = r(widthLenGt > 0 ? widthTvGt / widthLenGt * 100.0 : 0.0);
    m["width_hf_rms_px"] = r(widthHf.rms(), 4);
    m["width_mean_ratio"] = r(widthCount > 0 && widthSumGt > 0
                                  ? (widthSum / widthCount) / (widthSumGt / widthCountGt)
                                  : std::numeric_limits<double>::quiet_NaN());
    m["wet_lag_px_mean"] = r(lagPx.mean());
    m["wet_lag_px_p95"] = r(lagPx.pct(0.95));
    m["wet_lag_px_max"] = r(lagPx.max());
    m["wet_lag_truth_px_mean"] = r(lagTruePx.mean());
    m["wet_lag_ms_mean"] = r(lagMs.mean(), 2);
    m["wet_lag_ms_p95"] = r(lagMs.pct(0.95), 2);
    m["wet_vs_final_mean_px"] = r(wetFinal.mean());
    m["wet_vs_final_max_px"] = r(wetFinal.max());
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
