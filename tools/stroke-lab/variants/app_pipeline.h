#pragma once

// Конвейер приложения ровно как в DocumentCanvas: WetStroke::begin(page,
// prototype, m_zoom) на касании, addPoint на каждый отсчёт, finish() на отрыве;
// сухая геометрия - новый StrokeTessellator с setWidth(stroke.width) и
// setStyle(stroke.style), затем tessellate(stroke.points)
// (DocumentCanvas::tessellate). Растеризация - лабораторная эмуляция MSAA 4x.
//
// Закон толщины - параметр: приложение даёт новым штрихам Tapered (baseline),
// а Legacy нужен, чтобы мерить давление на прежнем законе W·(0.25 + 0.75p),
// на котором выбраны числа D2 (app-legacy, ink-beauty-pass, design.md).

#include "lab.h"

#include "ink/stroketessellator.h"
#include "ink/wetstroke.h"

namespace lab {

class AppPipeline : public Variant
{
public:
    explicit AppPipeline(StrokeStyle style)
        : m_style(style)
    {
    }

    void begin(const StrokeSetup &setup) override
    {
        Stroke prototype;
        prototype.tool = InkTool::Pen;
        prototype.color = setup.color;
        prototype.width = setup.width;
        prototype.style = m_style;
        m_wet.begin(0, prototype, setup.scale);
        m_final = Stroke();
    }

    void addEvent(const InkPoint &raw) override { m_wet.addPoint(raw); }

    void wet(int from, WetState &out) const override
    {
        const QList<InkPoint> &points = m_wet.points();
        out.settledCount = int(points.size());
        for (int i = qMax(0, from); i < points.size(); ++i)
            out.settledFrom.push_back(toCenter(points[i]));
        for (const InkPoint &p : m_wet.provisionalPoints())
            out.provisional.push_back(toCenter(p));
        out.vertexCount = int(m_wet.strip().size());
        out.settledVertexCount = m_wet.settledVertexCount();
    }

    void finish() override { m_final = m_wet.finish(); }

    std::vector<CenterPoint> centerline() const override
    {
        std::vector<CenterPoint> out;
        out.reserve(m_final.points.size());
        for (const InkPoint &p : m_final.points)
            out.push_back(toCenter(p));
        return out;
    }

    Geometry geometry() const override
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(m_final.width);
        tessellator.setStyle(m_final.style);
        const QVector<StrokeTessellator::Vertex> strip = tessellator.tessellate(m_final.points);
        Geometry g;
        if (StrokeTessellator::isDegenerate(strip))
            return g;
        g.kind = Geometry::Kind::TriangleStrip;
        g.vertices.reserve(strip.size());
        for (const auto &v : strip)
            g.vertices.emplace_back(v.x, v.y);
        return g;
    }

    Geometry wetGeometry() const override
    {
        Geometry g;
        g.kind = Geometry::Kind::TriangleStrip;
        for (const auto &v : m_wet.strip())
            g.vertices.emplace_back(v.x, v.y);
        return g;
    }

protected:
    CenterPoint toCenter(const InkPoint &p) const
    {
        // halfWidthAt зависит только от толщины и закона, а их задал begin():
        // одна формула и для мокрого, и для сухого штриха.
        auto &tessellator = const_cast<WetStroke &>(m_wet).tessellator();
        return {p.position, 2.0 * tessellator.halfWidthAt(p), p.pressure, p.timeSeconds};
    }

    StrokeStyle m_style;
    WetStroke m_wet;
    Stroke m_final;
};

} // namespace lab
