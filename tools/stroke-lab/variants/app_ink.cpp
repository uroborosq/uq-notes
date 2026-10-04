// app-ink - приложение после шага 3 ink-beauty-pass: полоса AxisVertex из
// StrokeTessellator и покрытие по формулам материала чернил
// (src/ink/inkcoverage.h - зеркало шейдеров), over на каждый фрагмент, без
// MSAA. Траектория - как у baseline (AppPipeline).
//
// Сверяется с aa-fringe, прототипом D7 (предела выноса по радиусу кривизны
// нет): расхождение значит, что приложение ушло от прототипа. baseline после
// шага 3 - та же полоса в покое под эмуляцией MSAA 4x, то есть прежняя
// отрисовка.

#include "app_pipeline.h"

#include "aaraster.h"
#include "ink/inkcoverage.h"

#include <cmath>

namespace {

using namespace lab;

class AppInk : public AppPipeline
{
public:
    AppInk()
        : AppPipeline(StrokeStyle::Tapered)
    {
    }

    bool rasterize(const Geometry &, const RasterTarget &target) const override
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(m_final.width);
        tessellator.setStyle(m_final.style);
        const QVector<StrokeTessellator::Vertex> strip = tessellator.tessellate(m_final.points);
        if (StrokeTessellator::isDegenerate(strip) || !target.image)
            return false;

        const QTransform &T = target.pageToPixel;
        const qreal pxPerUnit = std::sqrt(std::abs(T.determinant()));
        std::vector<AttrVertex<2>> vertices;
        vertices.reserve(size_t(strip.size()));
        for (const AxisVertex &a : strip) {
            const ink::DeviceVertex d = ink::toDevice(a, pxPerUnit);
            AttrVertex<2> out;
            out.p = T.map(d.position);
            out.a = {float(d.distance), float(d.halfWidth)};
            vertices.push_back(out);
        }

        FragmentRaster raster(target.image->width(), target.image->height());
        AlphaBuffer buffer(target.image->width(), target.image->height());
        const float inkAlpha = float(target.color.alphaF());
        raster.strip(vertices, [&](int x, int y, const std::array<float, 2> &a) {
            const float cov = float(ink::coverage(a[0], a[1]));
            if (cov > 0.0f)
                buffer.over(x, y, cov * inkAlpha);
        });
        QColor opaque = target.color;
        opaque.setAlpha(255);
        buffer.compositeOnto(*target.image, opaque);
        return true;
    }
};

} // namespace

STROKE_LAB_VARIANT(AppInk, "app-ink",
                   "приложение после шага 3: полоса AxisVertex из StrokeTessellator, покрытие "
                   "по формулам InkMaterial (src/ink/inkcoverage.h), over, без MSAA");
