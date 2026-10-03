#include "raster.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace lab {

SamplePattern SamplePattern::msaa4()
{
    return {{QPointF(0.375, 0.125), QPointF(0.875, 0.375), QPointF(0.125, 0.625),
             QPointF(0.625, 0.875)}};
}

SamplePattern SamplePattern::grid(int n)
{
    n = std::clamp(n, 1, 8);
    SamplePattern p;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            p.offsets.emplace_back((i + 0.5) / n, (j + 0.5) / n);
    return p;
}

SamplePattern SamplePattern::lattice64()
{
    SamplePattern p;
    for (int i = 0; i < 64; ++i)
        p.offsets.emplace_back((i + 0.5) / 64.0, ((19 * i) % 64 + 0.5) / 64.0);
    return p;
}

CoverageBuffer::CoverageBuffer(int width, int height, SamplePattern pattern)
    : m_width(std::max(1, width))
    , m_height(std::max(1, height))
    , m_pattern(std::move(pattern))
    , m_count(int(m_pattern.offsets.size()))
    , m_mask(size_t(m_width) * size_t(m_height), 0)
{
}

void CoverageBuffer::addTriangle(const QPointF &a, const QPointF &b, const QPointF &c)
{
    const double area = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
    if (!std::isfinite(area) || std::abs(area) < 1e-12)
        return; // GPU вырожденный треугольник не рисует
    const int x0 = std::max(0, int(std::floor(std::min({a.x(), b.x(), c.x()}))));
    const int x1 = std::min(m_width - 1, int(std::floor(std::max({a.x(), b.x(), c.x()}))));
    const int y0 = std::max(0, int(std::floor(std::min({a.y(), b.y(), c.y()}))));
    const int y1 = std::min(m_height - 1, int(std::floor(std::max({a.y(), b.y(), c.y()}))));
    if (x0 > x1 || y0 > y1)
        return;
    const double sign = area > 0 ? 1.0 : -1.0;
    const auto edge = [](const QPointF &p, const QPointF &q, double x, double y) {
        return (q.x() - p.x()) * (y - p.y()) - (q.y() - p.y()) * (x - p.x());
    };
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            uint64_t bits = 0;
            for (int s = 0; s < m_count; ++s) {
                const double sx = x + m_pattern.offsets[size_t(s)].x();
                const double sy = y + m_pattern.offsets[size_t(s)].y();
                const double e0 = edge(a, b, sx, sy) * sign;
                const double e1 = edge(b, c, sx, sy) * sign;
                const double e2 = edge(c, a, sx, sy) * sign;
                if (e0 >= 0 && e1 >= 0 && e2 >= 0)
                    bits |= uint64_t(1) << s;
            }
            m_mask[size_t(y) * size_t(m_width) + size_t(x)] |= bits;
        }
    }
}

void CoverageBuffer::addTriangleStrip(const std::vector<QPointF> &strip)
{
    for (size_t i = 0; i + 2 < strip.size(); ++i)
        addTriangle(strip[i], strip[i + 1], strip[i + 2]);
}

void CoverageBuffer::addTriangles(const std::vector<QPointF> &triangles)
{
    for (size_t i = 0; i + 2 < triangles.size(); i += 3)
        addTriangle(triangles[i], triangles[i + 1], triangles[i + 2]);
}

void CoverageBuffer::addConvex(const std::vector<QPointF> &poly)
{
    for (size_t i = 1; i + 1 < poly.size(); ++i)
        addTriangle(poly[0], poly[i], poly[i + 1]);
}

void CoverageBuffer::addDisc(const QPointF &center, double radius)
{
    if (radius <= 0)
        return;
    const int x0 = std::max(0, int(std::floor(center.x() - radius)));
    const int x1 = std::min(m_width - 1, int(std::floor(center.x() + radius)));
    const int y0 = std::max(0, int(std::floor(center.y() - radius)));
    const int y1 = std::min(m_height - 1, int(std::floor(center.y() + radius)));
    const double r2 = radius * radius;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            uint64_t bits = 0;
            for (int s = 0; s < m_count; ++s) {
                const double dx = x + m_pattern.offsets[size_t(s)].x() - center.x();
                const double dy = y + m_pattern.offsets[size_t(s)].y() - center.y();
                if (dx * dx + dy * dy <= r2)
                    bits |= uint64_t(1) << s;
            }
            m_mask[size_t(y) * size_t(m_width) + size_t(x)] |= bits;
        }
    }
}

void CoverageBuffer::addPolygons(const std::vector<QPolygonF> &rings)
{
    struct Edge
    {
        double x0, y0, x1, y1;
        int dir;
    };
    std::vector<Edge> edges;
    for (const QPolygonF &ring : rings) {
        const qsizetype n = ring.size();
        for (qsizetype i = 0; i < n; ++i) {
            const QPointF a = ring[i];
            const QPointF b = ring[(i + 1) % n];
            if (a.y() == b.y())
                continue;
            if (a.y() < b.y())
                edges.push_back({a.x(), a.y(), b.x(), b.y(), 1});
            else
                edges.push_back({b.x(), b.y(), a.x(), a.y(), -1});
        }
    }
    // Рёбра по строкам пикселей.
    std::vector<std::vector<int>> rows(static_cast<size_t>(m_height));
    for (size_t i = 0; i < edges.size(); ++i) {
        const int r0 = std::max(0, int(std::floor(edges[i].y0)));
        const int r1 = std::min(m_height - 1, int(std::floor(edges[i].y1)));
        for (int r = r0; r <= r1; ++r)
            rows[size_t(r)].push_back(int(i));
    }
    std::vector<std::pair<double, int>> crossings;
    for (int y = 0; y < m_height; ++y) {
        if (rows[size_t(y)].empty())
            continue;
        for (int s = 0; s < m_count; ++s) {
            const double sy = y + m_pattern.offsets[size_t(s)].y();
            const double ox = m_pattern.offsets[size_t(s)].x();
            crossings.clear();
            for (int ei : rows[size_t(y)]) {
                const Edge &e = edges[size_t(ei)];
                // Полуоткрытый интервал [y0, y1) - вершина не считается дважды.
                if (sy < e.y0 || sy >= e.y1)
                    continue;
                const double x = e.x0 + (e.x1 - e.x0) * (sy - e.y0) / (e.y1 - e.y0);
                crossings.emplace_back(x, e.dir);
            }
            if (crossings.empty())
                continue;
            std::sort(crossings.begin(), crossings.end());
            int winding = 0;
            for (size_t k = 0; k + 1 < crossings.size(); ++k) {
                winding += crossings[k].second;
                if (winding == 0)
                    continue;
                const double xa = crossings[k].first;
                const double xb = crossings[k + 1].first;
                // Пиксели, у которых точка выборки x+ox в [xa, xb).
                const int pa = std::max(0, int(std::ceil(xa - ox)));
                const int pb = std::min(m_width - 1, int(std::ceil(xb - ox)) - 1);
                for (int x = pa; x <= pb; ++x)
                    m_mask[size_t(y) * size_t(m_width) + size_t(x)] |= uint64_t(1) << s;
            }
        }
    }
}

double CoverageBuffer::coverage(int x, int y) const
{
    return double(std::popcount(m_mask[size_t(y) * size_t(m_width) + size_t(x)])) / m_count;
}

void CoverageBuffer::compositeOnto(QImage &image, const QColor &ink) const
{
    const int w = std::min(m_width, image.width());
    const int h = std::min(m_height, image.height());
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const uint64_t m = m_mask[size_t(y) * size_t(m_width) + size_t(x)];
            if (!m)
                continue;
            const double c = double(std::popcount(m)) / m_count;
            const QRgb bg = line[x];
            const auto mix = [c](int b, int i) { return int(std::lround(b + (i - b) * c)); };
            line[x] = qRgb(mix(qRed(bg), ink.red()), mix(qGreen(bg), ink.green()),
                           mix(qBlue(bg), ink.blue()));
        }
    }
}

void CoverageBuffer::compositeOntoLinear(QImage &image, const QColor &ink) const
{
    const auto toLinear = [](int v) {
        const double c = v / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const auto toSrgb = [](double c) {
        c = std::clamp(c, 0.0, 1.0);
        const double v = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
        return int(std::lround(v * 255.0));
    };
    const int w = std::min(m_width, image.width());
    const int h = std::min(m_height, image.height());
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const uint64_t m = m_mask[size_t(y) * size_t(m_width) + size_t(x)];
            if (!m)
                continue;
            const double c = double(std::popcount(m)) / m_count;
            const QRgb bg = line[x];
            const auto mix = [&](int b, int i) {
                return toSrgb(toLinear(b) + (toLinear(i) - toLinear(b)) * c);
            };
            line[x] = qRgb(mix(qRed(bg), ink.red()), mix(qGreen(bg), ink.green()),
                           mix(qBlue(bg), ink.blue()));
        }
    }
}

} // namespace lab
