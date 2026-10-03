#include "rastermetrics.h"

#include <QTransform>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <unordered_map>

#include "aaraster.h"
#include "raster.h"

namespace lab {

namespace {

double len(const QPointF &v) { return std::hypot(v.x(), v.y()); }

Geometry mapGeometry(const Geometry &g, const QTransform &T)
{
    Geometry out = g;
    for (QPointF &p : out.vertices)
        p = T.map(p);
    for (QPolygonF &ring : out.polygons)
        ring = T.map(ring);
    return out;
}

template<class Buffer>
void addMapped(Buffer &buffer, const Geometry &g)
{
    switch (g.kind) {
    case Geometry::Kind::TriangleStrip:
        buffer.addTriangleStrip(g.vertices);
        break;
    case Geometry::Kind::Triangles:
        buffer.addTriangles(g.vertices);
        break;
    default:
        break;
    }
}

void addSweptBrush(CoverageBuffer &buffer, const std::vector<QPointF> &pts,
                   const std::vector<double> &radius)
{
    for (size_t i = 0; i < pts.size(); ++i) {
        buffer.addDisc(pts[i], radius[i]);
        if (i + 1 < pts.size()) {
            const QPointF d = pts[i + 1] - pts[i];
            const double l = len(d);
            if (l < 1e-9)
                continue;
            const QPointF n(-d.y() / l, d.x() / l);
            buffer.addConvex({pts[i] + n * radius[i], pts[i + 1] + n * radius[i + 1],
                              pts[i + 1] - n * radius[i + 1], pts[i] - n * radius[i]});
        }
    }
}

QSize imageSize(const Sample &sample, const RenderOptions &options)
{
    // +1 px запаса под субпиксельный сдвиг.
    return QSize(std::max(1, int(std::ceil(sample.bounds.width() * options.dpr)) + 1),
                 std::max(1, int(std::ceil(sample.bounds.height() * options.dpr)) + 1));
}

// Как renderApp, но с произвольным pageToPixel и цветом (возможно,
// полупрозрачным).
QImage renderAppAt(const std::vector<StrokeRun> &runs, const QTransform &T, const QSize &size,
                   const QColor &paper, const QColor &ink)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(paper);
    const bool translucent = ink.alpha() < 255;
    CoverageBuffer msaa(size.width(), size.height(), SamplePattern::msaa4());
    SampleCountBuffer counted(size.width(), size.height(), SamplePattern::msaa4());
    bool anyMsaa = false;
    std::vector<const StrokeRun *> own;
    for (const StrokeRun &run : runs) {
        QImage probe = image;
        RasterTarget target{&probe, T, ink};
        if (run.variant && run.variant->rasterize(run.geometry, target)) {
            own.push_back(&run);
            continue;
        }
        const Geometry g = mapGeometry(run.geometry, T);
        anyMsaa = true;
        if (g.kind == Geometry::Kind::Polygons) {
            msaa.addPolygons(g.polygons);
        } else if (translucent) {
            addMapped(counted, g);
        } else {
            addMapped(msaa, g);
        }
    }
    if (anyMsaa) {
        msaa.compositeOnto(image, ink);
        if (translucent)
            counted.compositeOnto(image, ink);
    }
    for (const StrokeRun *run : own) {
        RasterTarget target{&image, T, ink};
        run->variant->rasterize(run->geometry, target);
    }
    return image;
}

// Покрытие из цвета: проекция на отрезок бумага -> чернила (rgb без альфы).
std::vector<float> coverageOf(const QImage &image, const QColor &paper, const QColor &ink)
{
    const double dr = paper.red() - ink.red(), dg = paper.green() - ink.green(),
                 db = paper.blue() - ink.blue();
    const double n2 = dr * dr + dg * dg + db * db;
    std::vector<float> c(size_t(image.width()) * size_t(image.height()), 0.0f);
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const double v = ((paper.red() - qRed(line[x])) * dr
                              + (paper.green() - qGreen(line[x])) * dg
                              + (paper.blue() - qBlue(line[x])) * db)
                             / n2;
            c[size_t(y) * size_t(image.width()) + size_t(x)] = float(v);
        }
    }
    return c;
}

std::vector<float> coverageOf(const CoverageBuffer &buffer)
{
    std::vector<float> c(size_t(buffer.width()) * size_t(buffer.height()));
    for (int y = 0; y < buffer.height(); ++y)
        for (int x = 0; x < buffer.width(); ++x)
            c[size_t(y) * size_t(buffer.width()) + size_t(x)] = float(buffer.coverage(x, y));
    return c;
}

std::vector<float> shapeCoverage(const std::vector<StrokeRun> &runs, const QTransform &T,
                                 const QSize &size)
{
    CoverageBuffer buffer(size.width(), size.height(), SamplePattern::lattice64());
    for (const StrokeRun &run : runs) {
        const Geometry g = mapGeometry(run.geometry, T);
        if (g.kind == Geometry::Kind::Polygons)
            buffer.addPolygons(g.polygons);
        else
            addMapped(buffer, g);
    }
    return coverageOf(buffer);
}

std::vector<float> idealCoverage(const std::vector<StrokeRun> &runs, const QTransform &T,
                                 double toPixel, const QSize &size)
{
    CoverageBuffer buffer(size.width(), size.height(), SamplePattern::lattice64());
    for (const StrokeRun &run : runs) {
        std::vector<QPointF> pts;
        std::vector<double> radius;
        for (const CenterPoint &c : run.centerline) {
            pts.push_back(T.map(c.pos));
            radius.push_back(c.width * toPixel / 2.0);
        }
        addSweptBrush(buffer, pts, radius);
    }
    return coverageOf(buffer);
}

std::vector<float> truthCoverage(const Sample &sample, double scale, const RenderOptions &options,
                                 const QPointF &shift, const QSize &size)
{
    CoverageBuffer buffer(size.width(), size.height(), SamplePattern::lattice64());
    for (const SampleStroke &stroke : sample.strokes) {
        std::vector<QPointF> pts;
        std::vector<double> radius;
        for (size_t i = 0; i < stroke.truth.p.size(); ++i) {
            const QPointF p = stroke.truth.p[i] * options.dpr + shift;
            if (!pts.empty() && len(p - pts.back()) < 0.4 && i + 1 < stroke.truth.p.size())
                continue;
            pts.push_back(p);
            radius.push_back(truthWidthPx(stroke.truth.pressure[i], scale, options.metrics)
                             * options.dpr / 2.0);
        }
        addSweptBrush(buffer, pts, radius);
    }
    return coverageOf(buffer);
}

bool partial(float c) { return c > 0.002f && c < 0.998f; }

struct ErrStats
{
    double rmse = 0, p99 = 0, max = 0;
    int count = 0;
};

ErrStats edgeError(const std::vector<float> &a, const std::vector<float> &b)
{
    std::vector<double> errs;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!partial(a[i]) && !partial(b[i]))
            continue;
        errs.push_back(std::abs(double(a[i]) - double(b[i])) * 255.0);
    }
    ErrStats s;
    s.count = int(errs.size());
    if (errs.empty())
        return s;
    double sq = 0;
    for (double e : errs)
        sq += e * e;
    s.rmse = std::sqrt(sq / errs.size());
    std::sort(errs.begin(), errs.end());
    s.p99 = errs[std::min(errs.size() - 1, size_t(errs.size() * 0.99))];
    s.max = errs.back();
    return s;
}

// Масса по отрезкам осевой: «бусины».
double massCv(const std::vector<StrokeRun> &runs, const QTransform &T,
              const std::vector<float> &render, const std::vector<float> &ideal, const QSize &size)
{
    constexpr double kBin = 2.0;
    constexpr double kEnd = 3.0;
    struct P
    {
        QPointF p;
        double arc;
        double total;
        int run;
    };
    std::vector<P> pts;
    double maxR = 0;
    for (size_t r = 0; r < runs.size(); ++r) {
        double arc = 0;
        const size_t first = pts.size();
        for (size_t i = 0; i < runs[r].centerline.size(); ++i) {
            const QPointF p = T.map(runs[r].centerline[i].pos);
            if (i > 0)
                arc += len(p - pts.back().p);
            pts.push_back({p, arc, 0, int(r)});
            maxR = std::max(maxR, runs[r].centerline[i].width * std::sqrt(std::abs(T.determinant())) / 2);
        }
        for (size_t i = first; i < pts.size(); ++i)
            pts[i].total = arc;
    }
    if (pts.empty())
        return 0;
    // Сетка 4 px для поиска ближайшей точки.
    constexpr double kCell = 4.0;
    std::unordered_map<long long, std::vector<int>> grid;
    const auto key = [](int x, int y) { return (long long)(x) * 1000003LL + y; };
    for (size_t i = 0; i < pts.size(); ++i)
        grid[key(int(std::floor(pts[i].p.x() / kCell)), int(std::floor(pts[i].p.y() / kCell)))]
            .push_back(int(i));
    const double reach = maxR + 2.0;
    const int ring = int(std::ceil(reach / kCell)) + 1;
    std::map<std::pair<int, int>, std::pair<double, double>> bins; // (run, bin) -> (render, ideal)
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const size_t idx = size_t(y) * size_t(size.width()) + size_t(x);
            const double cr = std::max(0.0f, render[idx]);
            const double ci = ideal[idx];
            if (cr <= 0.002 && ci <= 0.002)
                continue;
            const QPointF c(x + 0.5, y + 0.5);
            const int gx = int(std::floor(c.x() / kCell)), gy = int(std::floor(c.y() / kCell));
            int best = -1;
            double bestD = reach;
            for (int dy = -ring; dy <= ring; ++dy) {
                for (int dx = -ring; dx <= ring; ++dx) {
                    const auto it = grid.find(key(gx + dx, gy + dy));
                    if (it == grid.end())
                        continue;
                    for (int i : it->second) {
                        const double d = len(pts[size_t(i)].p - c);
                        if (d < bestD) {
                            bestD = d;
                            best = i;
                        }
                    }
                }
            }
            if (best < 0)
                continue;
            const P &p = pts[size_t(best)];
            if (p.arc < kEnd || p.arc > p.total - kEnd)
                continue;
            auto &bin = bins[{p.run, int(p.arc / kBin)}];
            bin.first += cr;
            bin.second += ci;
        }
    }
    std::vector<double> ratios;
    double medianIdeal = 0;
    {
        std::vector<double> m;
        for (const auto &[k, v] : bins)
            m.push_back(v.second);
        if (m.empty())
            return 0;
        std::nth_element(m.begin(), m.begin() + m.size() / 2, m.end());
        medianIdeal = m[m.size() / 2];
    }
    for (const auto &[k, v] : bins)
        if (v.second > 0.3 * medianIdeal)
            ratios.push_back(v.first / v.second);
    if (ratios.size() < 2)
        return 0;
    const double mean = std::accumulate(ratios.begin(), ratios.end(), 0.0) / ratios.size();
    double var = 0;
    for (double r : ratios)
        var += (r - mean) * (r - mean);
    var /= ratios.size();
    return mean > 0 ? std::sqrt(var) / mean : 0;
}

} // namespace

RasterMetricsResult computeRasterMetrics(const Sample &sample, const std::vector<StrokeRun> &runs,
                                         double scale, const RenderOptions &options,
                                         const RasterMetricsOptions &rasterOptions)
{
    RasterMetricsResult result;
    QJsonObject &m = result.metrics;
    const QSize size = imageSize(sample, options);
    const double toPixel = scale * options.dpr;
    const int shifts = std::max(1, rasterOptions.shifts);
    const size_t npx = size_t(size.width()) * size_t(size.height());

    std::vector<std::vector<float>> errs; // вариант − форма, по сдвигам
    std::vector<double> masses;
    std::vector<float> render0, shape0;
    double shapeSq = 0;
    int shapeCount = 0;
    double shapeP99 = 0, shapeMax = 0;
    for (int k = 0; k < shifts; ++k) {
        const QPointF shift(double(k) / shifts, double((k * 3) % shifts) / shifts);
        QTransform T = QTransform::fromScale(toPixel, toPixel);
        T *= QTransform::fromTranslate(shift.x(), shift.y());
        const QImage image = renderAppAt(runs, T, size, options.paper, options.ink);
        std::vector<float> cov = coverageOf(image, options.paper, options.ink);
        std::vector<float> shape = shapeCoverage(runs, T, size);
        const ErrStats e = edgeError(cov, shape);
        shapeSq += e.rmse * e.rmse * e.count;
        shapeCount += e.count;
        shapeP99 += e.p99 / shifts;
        shapeMax = std::max(shapeMax, e.max);
        std::vector<float> err(npx);
        double mass = 0;
        for (size_t i = 0; i < npx; ++i) {
            err[i] = cov[i] - shape[i];
            mass += cov[i];
        }
        errs.push_back(std::move(err));
        masses.push_back(mass);
        if (k == 0) {
            QImage map(size, QImage::Format_RGB32);
            for (int y = 0; y < size.height(); ++y) {
                QRgb *line = reinterpret_cast<QRgb *>(map.scanLine(y));
                for (int x = 0; x < size.width(); ++x) {
                    const size_t i = size_t(y) * size_t(size.width()) + size_t(x);
                    const double e = std::clamp(double(cov[i] - shape[i]) * 4.0, -1.0, 1.0);
                    const int base = 255 - int(std::lround(std::clamp(double(shape[i]), 0.0, 1.0) * 40));
                    if (e >= 0)
                        line[x] = qRgb(base, int(base * (1 - e)), int(base * (1 - e)));
                    else
                        line[x] = qRgb(int(base * (1 + e)), int(base * (1 + e)), base);
                }
            }
            result.errorMap = map;
            render0 = std::move(cov);
            shape0 = std::move(shape);
        }
    }
    m["aa_rmse_shape"] = shapeCount ? std::sqrt(shapeSq / shapeCount) : 0.0;
    m["aa_p99_shape"] = shapeP99;
    m["aa_max_shape"] = shapeMax;

    if (shifts > 1) {
        double acc = 0;
        int n = 0;
        for (size_t i = 0; i < npx; ++i) {
            double mean = 0;
            bool any = false;
            for (const auto &e : errs) {
                mean += e[i];
                any = any || std::abs(e[i]) > 0.002f;
            }
            if (!any)
                continue;
            mean /= shifts;
            double var = 0;
            for (const auto &e : errs)
                var += (e[i] - mean) * (e[i] - mean);
            acc += std::sqrt(var / shifts) * 255.0;
            ++n;
        }
        m["aa_flicker"] = n ? acc / n : 0.0;
        const double meanMass = std::accumulate(masses.begin(), masses.end(), 0.0) / shifts;
        double var = 0;
        for (double x : masses)
            var += (x - meanMass) * (x - meanMass);
        m["aa_mass_breath_pct"] = meanMass > 0 ? 100.0 * std::sqrt(var / shifts) / meanMass : 0.0;
    }

    const QTransform T0 = QTransform::fromScale(toPixel, toPixel);
    const std::vector<float> ideal = idealCoverage(runs, T0, toPixel, size);
    const std::vector<float> truth = truthCoverage(sample, scale, options, QPointF(), size);
    m["aa_rmse_ideal"] = edgeError(render0, ideal).rmse;
    m["aa_rmse_truth"] = edgeError(render0, truth).rmse;
    double massR = 0, massI = 0;
    for (size_t i = 0; i < npx; ++i) {
        massR += render0[i];
        massI += ideal[i];
    }
    m["aa_mass_ratio"] = massI > 0 ? massR / massI : 0.0;
    m["aa_mass_cv"] = massCv(runs, T0, render0, ideal, size);

    if (rasterOptions.marker) {
        const QColor marker = rasterOptions.markerColor;
        QImage image = renderAppAt(runs, T0, size, options.paper, marker);
        QColor opaque = marker;
        opaque.setAlpha(255);
        const std::vector<float> a = coverageOf(image, options.paper, opaque);
        const double alpha = marker.alphaF();
        int inked = 0, dark = 0;
        double amax = 0;
        for (float v : a) {
            if (v > 0.01f)
                ++inked;
            if (v > alpha + 0.01)
                ++dark;
            amax = std::max(amax, double(v));
        }
        m["marker_dark_frac"] = inked ? double(dark) / inked : 0.0;
        m["marker_alpha_max"] = amax;
        result.marker = image;
    }
    return result;
}

} // namespace lab
