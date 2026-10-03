#include "aaraster.h"

namespace lab {

SampleCountBuffer::SampleCountBuffer(int width, int height, SamplePattern pattern)
    : m_width(std::max(1, width))
    , m_height(std::max(1, height))
    , m_pattern(std::move(pattern))
    , m_count(int(m_pattern.offsets.size()))
    , m_counts(size_t(m_width) * size_t(m_height) * size_t(m_count), 0)
{
}

void SampleCountBuffer::addTriangle(const QPointF &a, const QPointF &b, const QPointF &c)
{
    const double area = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
    if (!std::isfinite(area) || std::abs(area) < 1e-12)
        return;
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
            for (int s = 0; s < m_count; ++s) {
                const double sx = x + m_pattern.offsets[size_t(s)].x();
                const double sy = y + m_pattern.offsets[size_t(s)].y();
                // Ничьи - как у CoverageBuffer (>= 0): для маркера это может
                // слегка завысить двойное наложение на общих рёбрах, но точка
                // выборки ровно на ребре почти не встречается.
                if (edge(a, b, sx, sy) * sign >= 0 && edge(b, c, sx, sy) * sign >= 0
                    && edge(c, a, sx, sy) * sign >= 0) {
                    uint8_t &n = m_counts[(size_t(y) * size_t(m_width) + size_t(x)) * size_t(m_count)
                                          + size_t(s)];
                    if (n < 255)
                        ++n;
                }
            }
        }
    }
}

void SampleCountBuffer::addTriangleStrip(const std::vector<QPointF> &strip)
{
    for (size_t i = 0; i + 2 < strip.size(); ++i)
        addTriangle(strip[i], strip[i + 1], strip[i + 2]);
}

void SampleCountBuffer::addTriangles(const std::vector<QPointF> &triangles)
{
    for (size_t i = 0; i + 2 < triangles.size(); i += 3)
        addTriangle(triangles[i], triangles[i + 1], triangles[i + 2]);
}

void SampleCountBuffer::compositeOnto(QImage &image, const QColor &ink) const
{
    const double alpha = ink.alphaF();
    const int w = std::min(m_width, image.width());
    const int h = std::min(m_height, image.height());
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const uint8_t *c = &m_counts[(size_t(y) * size_t(m_width) + size_t(x)) * size_t(m_count)];
            double acc = 0;
            for (int s = 0; s < m_count; ++s)
                if (c[s])
                    acc += 1.0 - std::pow(1.0 - alpha, c[s]);
            if (acc <= 0)
                continue;
            const double k = acc / m_count;
            const QRgb bg = line[x];
            const auto mix = [k](int b, int i) { return int(std::lround(b + (i - b) * k)); };
            line[x] = qRgb(mix(qRed(bg), ink.red()), mix(qGreen(bg), ink.green()),
                           mix(qBlue(bg), ink.blue()));
        }
    }
}

} // namespace lab
