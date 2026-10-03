#pragma once

// Эмуляция растеризации с аналитическим сглаживанием - то, что делал бы
// фрагментный шейдер без MSAA.
//
// FragmentRaster: одна точка выборки в центре пикселя, правило top-left на
// общих рёбрах (общая кромка двух треугольников одной полосы закрашивается
// ровно один раз), вершинные атрибуты интерполируются аффинно, как varying в
// 2D. Колбэк получает пиксель и интерполированные атрибуты - это и есть
// «фрагментный шейдер».
//
// AlphaBuffer: накопление альфы одного цвета. over() - обычное смешивание
// GPU (src_alpha, one_minus_src_alpha), применённое к каждому фрагменту:
// перекрывающиеся фрагменты одного штриха темнеют дважды. max() - объединение
// по максимуму покрытия (BlendOp::Max/Min для чернил темнее фона или слой
// штриха со «своим» max и последующим over).
//
// SampleCountBuffer: MSAA с подсчётом, сколько треугольников покрыло каждую
// точку выборки, - нужен для полупрозрачного цвета (маркер): при смешивании
// каждый треугольник, накрывший точку, кладёт альфу ещё раз.

#include <QColor>
#include <QImage>
#include <QPointF>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "raster.h"

namespace lab {

template<int N>
struct AttrVertex
{
    QPointF p;               // пиксели изображения
    std::array<float, N> a{}; // атрибуты
};

class FragmentRaster
{
public:
    FragmentRaster(int width, int height) : m_width(width), m_height(height) {}

    // frag(x, y, const std::array<float, N> &attrs)
    template<int N, class F>
    void triangle(const AttrVertex<N> &v0, const AttrVertex<N> &v1, const AttrVertex<N> &v2,
                  F &&frag) const
    {
        QPointF a = v0.p, b = v1.p, c = v2.p;
        const AttrVertex<N> *va = &v0, *vb = &v1, *vc = &v2;
        double area = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
        if (!std::isfinite(area) || std::abs(area) < 1e-12)
            return; // вырожденный треугольник GPU не рисует
        if (area < 0) {
            std::swap(b, c);
            std::swap(vb, vc);
            area = -area;
        }
        const int x0 = std::max(0, int(std::floor(std::min({a.x(), b.x(), c.x()}) - 0.5)));
        const int x1 = std::min(m_width - 1, int(std::ceil(std::max({a.x(), b.x(), c.x()}) - 0.5)));
        const int y0 = std::max(0, int(std::floor(std::min({a.y(), b.y(), c.y()}) - 0.5)));
        const int y1 = std::min(m_height - 1, int(std::ceil(std::max({a.y(), b.y(), c.y()}) - 0.5)));
        if (x0 > x1 || y0 > y1)
            return;
        // e(p,q) > 0 внутри (area > 0). На ничьей (точка ровно на ребре)
        // ребро «своё», только если его направление в выбранной полуплоскости:
        // общее ребро двух согласованно ориентированных треугольников
        // проходится в противоположные стороны, поэтому достаётся одному.
        const auto edge = [](const QPointF &p, const QPointF &q, double x, double y) {
            return (q.x() - p.x()) * (y - p.y()) - (q.y() - p.y()) * (x - p.x());
        };
        const auto owns = [](const QPointF &p, const QPointF &q) {
            const double dy = q.y() - p.y();
            const double dx = q.x() - p.x();
            return dy < 0 || (dy == 0 && dx > 0);
        };
        const bool o0 = owns(b, c), o1 = owns(c, a), o2 = owns(a, b);
        std::array<float, N> attrs;
        for (int y = y0; y <= y1; ++y) {
            const double sy = y + 0.5;
            for (int x = x0; x <= x1; ++x) {
                const double sx = x + 0.5;
                const double w0 = edge(b, c, sx, sy); // вес вершины a
                const double w1 = edge(c, a, sx, sy); // вес b
                const double w2 = edge(a, b, sx, sy); // вес c
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                if ((w0 == 0 && !o0) || (w1 == 0 && !o1) || (w2 == 0 && !o2))
                    continue;
                const double l0 = w0 / area, l1 = w1 / area, l2 = w2 / area;
                for (int i = 0; i < N; ++i)
                    attrs[size_t(i)] = float(l0 * va->a[size_t(i)] + l1 * vb->a[size_t(i)]
                                             + l2 * vc->a[size_t(i)]);
                frag(x, y, attrs);
            }
        }
    }

    template<int N, class F>
    void strip(const std::vector<AttrVertex<N>> &v, F &&frag) const
    {
        for (size_t i = 0; i + 2 < v.size(); ++i)
            triangle(v[i], v[i + 1], v[i + 2], frag);
    }

    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    int m_width;
    int m_height;
};

class AlphaBuffer
{
public:
    AlphaBuffer(int width, int height)
        : m_width(width), m_height(height), m_a(size_t(width) * size_t(height), 0.0f)
    {
    }

    void over(int x, int y, float alpha)
    {
        float &d = m_a[size_t(y) * size_t(m_width) + size_t(x)];
        d = d + alpha * (1.0f - d);
    }
    void max(int x, int y, float alpha)
    {
        float &d = m_a[size_t(y) * size_t(m_width) + size_t(x)];
        d = std::max(d, alpha);
    }
    float at(int x, int y) const { return m_a[size_t(y) * size_t(m_width) + size_t(x)]; }

    // pixel = pixel + (ink - pixel) * A * ink.alpha - без гаммы, как GPU в
    // не-sRGB буфере окна.
    void compositeOnto(QImage &image, const QColor &ink) const
    {
        const double inkAlpha = ink.alphaF();
        const int w = std::min(m_width, image.width());
        const int h = std::min(m_height, image.height());
        for (int y = 0; y < h; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const double c = double(m_a[size_t(y) * size_t(m_width) + size_t(x)]) * inkAlpha;
                if (c <= 0)
                    continue;
                const QRgb bg = line[x];
                const auto mix = [c](int b, int i) { return int(std::lround(b + (i - b) * c)); };
                line[x] = qRgb(mix(qRed(bg), ink.red()), mix(qGreen(bg), ink.green()),
                               mix(qBlue(bg), ink.blue()));
            }
        }
    }

private:
    int m_width;
    int m_height;
    std::vector<float> m_a;
};

// MSAA с подсчётом покрытий в каждой точке выборки (до 255).
class SampleCountBuffer
{
public:
    SampleCountBuffer(int width, int height, SamplePattern pattern);

    void addTriangle(const QPointF &a, const QPointF &b, const QPointF &c);
    void addTriangleStrip(const std::vector<QPointF> &strip);
    void addTriangles(const std::vector<QPointF> &triangles);

    // Resolve: среднее по точкам выборки от 1 - (1 - alpha)^count, без гаммы.
    void compositeOnto(QImage &image, const QColor &ink) const;

private:
    int m_width;
    int m_height;
    SamplePattern m_pattern;
    int m_count;
    std::vector<uint8_t> m_counts;
};

} // namespace lab
