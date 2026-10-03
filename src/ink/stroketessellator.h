#pragma once

#include <QList>
#include <QPointF>
#include <QPolygonF>
#include <QVector>

#include "ink/inktypes.h"

// Тесселяция штриха переменной ширины в треугольную полосу.
//
// Полоса, а не путь: чернила должны жить в графе сцены как обычная геометрия,
// иначе на каждом кадре пришлось бы растеризовать путь на CPU - ровно та
// проблема, из-за которой отвергнут cairo-подход (design.md, D4).
//
// Стыки скругляются: на резком изгибе усечённый угол дал бы либо разрыв
// снаружи, либо самопересечение внутри.
class StrokeTessellator
{
public:
    struct Vertex
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    // Толщина при полном нажиме, в точках страницы.
    void setWidth(qreal width);
    qreal width() const { return m_width; }

    // Доля толщины, остающаяся при нулевом нажиме: линия не должна
    // схлопываться в ничто на слабом касании. Обычно задаётся через setStyle.
    // Действует только на точки с давлением: точки без давления всегда
    // W·(0.25 + 0.75·constantPressure).
    void setMinWidthFactor(qreal factor);

    // Закон толщины штриха: доля при нулевом нажиме 0.25 у Legacy и 0.08 у
    // Tapered (ink-beauty-pass, design.md, D3). Задаётся перед каждым штрихом -
    // и мокрым, и сухим, и для экспорта: один тесселятор рисует штрихи разных
    // законов.
    void setStyle(StrokeStyle style);

    // Давление, которым рисуется штрих, если устройство давления не сообщает.
    // Такой штрих идёт по закону Legacy при любом стиле: толщина пера без
    // давления задаётся настройками и от заострения не меняется (D3).
    void setConstantPressure(qreal pressure);

    // Полная тесселяция траектории в треугольную полосу.
    QVector<Vertex> tessellate(const QList<InkPoint> &points) const;

    // Поэлементная выдача - на ней стоит инкрементальное наращивание «мокрого»
    // штриха: одна и та же математика стыка используется и при полной
    // тесселяции, и при дописывании хвоста.
    //
    // Торец штриха: пара вершин по нормали к направлению.
    void emitCap(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                 const QPointF &direction) const;
    // Стык: биссектриса на пологом повороте, дуга скругления на резком.
    void emitJoint(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                   const QPointF &incoming, const QPointF &outgoing) const;

    // Замкнутый контур штриха: левая сторона полосы вперёд, правая - назад.
    // Экспорт вписывает чернила в PDF именно заливкой контура, а не линией
    // постоянной толщины: только так переменная ширина от давления переживает
    // экспорт (design.md, D6). Контур строится из той же полосы, что рисуется
    // на экране, поэтому экспорт не может разойтись с тем, что видно.
    QPolygonF outline(const QList<InkPoint> &points) const;

    // Полуширина штриха в конкретной точке - по ней тесты проверяют, что
    // толщина следует за давлением.
    qreal halfWidthAt(const InkPoint &point) const;

    // Число вершин, которое даст полоса для траектории такой длины: нужно
    // инкрементальному наращиванию, чтобы понять, что именно добавилось.
    static bool isDegenerate(const QVector<Vertex> &strip);

private:
    qreal m_width = 2.4;
    qreal m_minWidthFactor = 0.25;
    qreal m_constantPressure = 0.65;
};
