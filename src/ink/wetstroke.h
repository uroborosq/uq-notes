#pragma once

#include <QList>
#include <QVector>

#include "ink/inktypes.h"
#include "ink/strokesampler.h"
#include "ink/stroketessellator.h"

// «Мокрый» штрих - тот, что рисуется прямо сейчас. Наращивается инкрементально:
// на кадр в полосу дописывается только геометрия новых точек, а всё, что уже
// лежит, не пересчитывается. Именно на этом держится бюджет латентности из
// ink-input: работа на кадр не зависит от того, насколько штрих уже длинный.
//
// Провизорным остаётся ровно один торец: стык в предпоследней точке зависит от
// точки, которая приходит следующей, поэтому при добавлении хвост полосы
// длиной в одну пару вершин переписывается, а всё до него уже окончательно.
class WetStroke
{
public:
    using Vertex = StrokeTessellator::Vertex;

    // scale - сколько экранных единиц в одной точке страницы на момент начала
    // штриха. От него зависят и шаг ресемплинга, и сила сглаживания.
    void begin(int page, const Stroke &prototype, qreal scale = 1.0);
    void reset();

    bool isActive() const { return m_active; }
    int page() const { return m_page; }

    // Добавляет сырую точку пера. Возвращает число вершин, дописанных к полосе
    // (переписанный торец в счёт не идёт).
    int addPoint(const InkPoint &raw);
    // Завершает штрих и отдаёт его для перевода в постоянную геометрию.
    Stroke finish();

    const QVector<Vertex> &strip() const { return m_strip; }
    // Вершины, которые уже никогда не изменятся.
    int settledVertexCount() const { return m_settledVertices; }
    const QList<InkPoint> &points() const { return m_sampler.points(); }

    StrokeTessellator &tessellator() { return m_tessellator; }

private:
    void rebuildTail();

    bool m_active = false;
    int m_page = -1;
    Stroke m_prototype;

    StrokeSampler m_sampler;
    StrokeTessellator m_tessellator;

    QVector<Vertex> m_strip;
    QList<QPointF> m_directions;
    int m_settledVertices = 0; // длина полосы без провизорного торца
    int m_jointedPoints = 0;   // сколько точек уже получили окончательный стык
};
