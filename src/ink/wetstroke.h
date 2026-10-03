#pragma once

#include <QList>
#include <QVector>

#include "ink/inktypes.h"
#include "ink/strokesampler.h"
#include "ink/stroketessellator.h"

// «Мокрый» штрих - тот, что рисуется прямо сейчас. Наращивается инкрементально:
// на кадр в полосу дописывается только геометрия новых окончательных точек, а
// всё, что уже лежит, не пересчитывается. Именно на этом держится бюджет
// латентности из ink-input: работа на кадр не зависит от того, насколько штрих
// уже длинный.
//
// Полоса состоит из двух частей:
// - окончательная - стыки точек, у которых уже есть окончательная следующая
//   точка; она никогда не переписывается;
// - провизорная - стык в последней окончательной точке, провизорный хвост из
//   сэмплера (сплайн без следующего соседа и предсказание модели до пера) и
//   торец. Она снимается и строится заново на каждом отсчёте, её размер
//   ограничен длиной хвоста.
//
// В завершённый штрих провизорная часть не попадает никогда.
class WetStroke
{
public:
    using Vertex = StrokeTessellator::Vertex;

    // scale - сколько экранных единиц в одной точке страницы на момент начала
    // штриха. От него зависят и шаг нарезки, и сила сглаживания.
    void begin(int page, const Stroke &prototype, qreal scale = 1.0);
    void reset();

    bool isActive() const { return m_active; }
    int page() const { return m_page; }

    // Добавляет сырую точку пера. Возвращает число вершин, добавившихся к
    // окончательной части полосы (провизорная в счёт не идёт).
    int addPoint(const InkPoint &raw);
    // Завершает штрих и отдаёт его для перевода в постоянную геометрию.
    Stroke finish();

    const QVector<Vertex> &strip() const { return m_strip; }
    // Вершины, которые уже никогда не изменятся.
    int settledVertexCount() const { return m_settledVertices; }
    // Окончательные точки штриха.
    const QList<InkPoint> &points() const { return m_sampler.points(); }
    const QList<InkPoint> &provisionalPoints() const { return m_sampler.provisionalPoints(); }

    StrokeTessellator &tessellator() { return m_tessellator; }

private:
    void rebuildTail();
    // Начальный торец можно класть в окончательную часть: штрих уже не станет
    // точкой, и хорда для направления торца больше не изменится.
    bool startCapIsSettled(const QList<InkPoint> &points) const;
    // Точка (design.md, D5): если осевая короче толщины пера, к ней дописывается
    // ещё одна точка в положении последней с наибольшим нажимом касания.
    // Окончательные точки не меняются - только добавляется одна.
    QList<InkPoint> withDotTwin(QList<InkPoint> points) const;

    bool m_active = false;
    int m_page = -1;
    Stroke m_prototype;

    StrokeSampler m_sampler;
    StrokeTessellator m_tessellator;

    QVector<Vertex> m_strip;
    QList<QPointF> m_directions; // направления окончательных сегментов
    int m_settledVertices = 0;   // длина окончательной части полосы
    int m_jointedPoints = 0;     // сколько точек уже получили окончательный стык
    qreal m_settledArc = 0.0;    // длина окончательной осевой
};
