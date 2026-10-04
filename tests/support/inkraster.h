#pragma once

// Растеризация полосы AxisVertex на CPU по формулам материала чернил
// (src/ink/inkcoverage.h - зеркало ink.vert и ink.frag): одна точка выборки в
// центре пикселя, правило top-left на общих рёбрах, атрибуты интерполируются
// аффинно, смешивание over на каждый фрагмент - как GPU без MSAA. Тот же
// приём, что FragmentRaster стенда (tools/stroke-lab/aaraster.h).

#include <QTransform>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <vector>

#include "ink/inkcoverage.h"
#include "ink/stroketessellator.h"

namespace inkraster {

struct Coverage
{
    int width = 0;
    int height = 0;
    std::vector<float> alpha;

    float at(int x, int y) const { return alpha[size_t(y) * size_t(width) + size_t(x)]; }
};

// pageToDevice - подобие без отражения: масштаб и сдвиг.
inline Coverage rasterize(const QVector<AxisVertex> &strip, const QTransform &pageToDevice,
                          int width, int height,
                          qreal minHalfWidthPx = ink::kDefaultMinHalfWidthPx)
{
    Coverage out{width, height, std::vector<float>(size_t(width) * size_t(height), 0.0f)};
    const qreal pxPerUnit = std::sqrt(std::abs(pageToDevice.determinant()));

    struct DeviceVertex
    {
        QPointF p;
        qreal d;
        qreal hw;
    };
    std::vector<DeviceVertex> v;
    v.reserve(size_t(strip.size()));
    for (const AxisVertex &a : strip) {
        const ink::DeviceVertex dv = ink::toDevice(a, pxPerUnit, minHalfWidthPx);
        v.push_back({pageToDevice.map(dv.position), dv.distance, dv.halfWidth});
    }

    const auto edge = [](const QPointF &p, const QPointF &q, double x, double y) {
        return (q.x() - p.x()) * (y - p.y()) - (q.y() - p.y()) * (x - p.x());
    };
    // Общее ребро двух согласованно ориентированных треугольников проходится
    // в противоположные стороны и достаётся одному из них.
    const auto owns = [](const QPointF &p, const QPointF &q) {
        const double dy = q.y() - p.y();
        const double dx = q.x() - p.x();
        return dy < 0 || (dy == 0 && dx > 0);
    };

    for (size_t i = 0; i + 2 < v.size(); ++i) {
        const DeviceVertex *a = &v[i];
        const DeviceVertex *b = &v[i + 1];
        const DeviceVertex *c = &v[i + 2];
        double area = edge(a->p, b->p, c->p.x(), c->p.y());
        if (!std::isfinite(area) || std::abs(area) < 1e-12)
            continue; // вырожденный треугольник GPU не рисует
        if (area < 0) {
            std::swap(b, c);
            area = -area;
        }
        const int x0 = std::max(0, int(std::floor(std::min({a->p.x(), b->p.x(), c->p.x()}) - 0.5)));
        const int x1 =
            std::min(width - 1, int(std::ceil(std::max({a->p.x(), b->p.x(), c->p.x()}) - 0.5)));
        const int y0 = std::max(0, int(std::floor(std::min({a->p.y(), b->p.y(), c->p.y()}) - 0.5)));
        const int y1 =
            std::min(height - 1, int(std::ceil(std::max({a->p.y(), b->p.y(), c->p.y()}) - 0.5)));
        const bool o0 = owns(b->p, c->p);
        const bool o1 = owns(c->p, a->p);
        const bool o2 = owns(a->p, b->p);
        for (int y = y0; y <= y1; ++y) {
            const double sy = y + 0.5;
            for (int x = x0; x <= x1; ++x) {
                const double sx = x + 0.5;
                const double w0 = edge(b->p, c->p, sx, sy);
                const double w1 = edge(c->p, a->p, sx, sy);
                const double w2 = edge(a->p, b->p, sx, sy);
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                if ((w0 == 0 && !o0) || (w1 == 0 && !o1) || (w2 == 0 && !o2))
                    continue;
                const double d = (w0 * a->d + w1 * b->d + w2 * c->d) / area;
                const double hw = (w0 * a->hw + w1 * b->hw + w2 * c->hw) / area;
                const float cov = float(ink::coverage(d, hw));
                float &dst = out.alpha[size_t(y) * size_t(width) + size_t(x)];
                dst = dst + cov * (1.0f - dst);
            }
        }
    }
    return out;
}

} // namespace inkraster
