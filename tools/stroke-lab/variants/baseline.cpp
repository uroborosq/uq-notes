// baseline - конвейер приложения ровно как в DocumentCanvas:
// WetStroke::begin(page, prototype, m_zoom) на касании, addPoint на каждый
// отсчёт, finish() на отрыве; сухая геометрия - новый StrokeTessellator с
// setWidth(stroke.width) и tessellate(stroke.points)
// (DocumentCanvas::tessellate). Растеризация - лабораторная эмуляция MSAA 4x.

#include "lab.h"

#include "ink/stroketessellator.h"
#include "ink/wetstroke.h"

namespace {

class Baseline : public lab::Variant
{
public:
    void begin(const lab::StrokeSetup &setup) override
    {
        Stroke prototype;
        prototype.tool = InkTool::Pen;
        prototype.color = setup.color;
        prototype.width = setup.width;
        m_wet.begin(0, prototype, setup.scale);
        m_final = Stroke();
    }

    void addEvent(const InkPoint &raw) override { m_wet.addPoint(raw); }

    void wet(int from, lab::WetState &out) const override
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

    std::vector<lab::CenterPoint> centerline() const override
    {
        std::vector<lab::CenterPoint> out;
        out.reserve(m_final.points.size());
        for (const InkPoint &p : m_final.points)
            out.push_back(toCenter(p));
        return out;
    }

    lab::Geometry geometry() const override
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(m_final.width);
        const QVector<StrokeTessellator::Vertex> strip = tessellator.tessellate(m_final.points);
        lab::Geometry g;
        if (StrokeTessellator::isDegenerate(strip))
            return g;
        g.kind = lab::Geometry::Kind::TriangleStrip;
        g.vertices.reserve(strip.size());
        for (const auto &v : strip)
            g.vertices.emplace_back(v.x, v.y);
        return g;
    }

    lab::Geometry wetGeometry() const override
    {
        lab::Geometry g;
        g.kind = lab::Geometry::Kind::TriangleStrip;
        for (const auto &v : m_wet.strip())
            g.vertices.emplace_back(v.x, v.y);
        return g;
    }

private:
    lab::CenterPoint toCenter(const InkPoint &p) const
    {
        // halfWidthAt не зависит от состояния, кроме толщины: одна формула и
        // для мокрого, и для сухого штриха.
        auto &tessellator = const_cast<WetStroke &>(m_wet).tessellator();
        return {p.position, 2.0 * tessellator.halfWidthAt(p), p.pressure, p.timeSeconds};
    }

    WetStroke m_wet;
    Stroke m_final;
};

} // namespace

STROKE_LAB_VARIANT(Baseline, "baseline",
                   "Конвейер приложения: StrokeSampler (ink-stroke-modeler + follow + "
                   "Catmull-Rom 1.2 px) -> WetStroke -> StrokeTessellator, MSAA 4x");
