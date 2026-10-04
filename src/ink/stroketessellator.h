#pragma once

#include <QList>
#include <QPointF>
#include <QPolygonF>
#include <QVector>

#include <cmath>

#include "ink/inktypes.h"

// Вершина оболочки штриха с осью (design.md ink-beauty-pass, D7). Полоса не
// хранит готовых позиций: вершинный шейдер выносит вершину на 1 device px за
// кромку - pos = c + n·вынос - и отдаёт d = s·вынос, знаковое расстояние
// поперёк штриха. Поэтому зум не пересобирает геометрию, а кромка
// сглаживается точной долей пикселя.
//
// Вершины двух видов:
// - центр веера (стыка или торца): n = 0, s = 0;
// - вершина кромки: |n| = 1; s = ±1 на нормали и дуге, ±cos(θ/2) у пары на
//   биссектрисе пологого стыка и у внутренней вершины резкого. Вынос
//   внутренней вершины ничем не ограничен (D7): предел по радиусу кривизны
//   вырезал краску внутри чернил на тугих арках.
//
// Тот же разбор вершины - в src/canvas/shaders/ink.vert и src/ink/inkcoverage.h.
struct AxisVertex
{
    float cx = 0.0f; // центр стыка или торца, точки страницы
    float cy = 0.0f;
    float nx = 0.0f; // единичное направление выноса, у центра веера - нулевое
    float ny = 0.0f;
    float hw = 0.0f; // полуширина, точки страницы
    float s = 0.0f;  // d = s·вынос

    QPointF center() const { return QPointF(cx, cy); }
    bool isCenter() const { return nx == 0.0f && ny == 0.0f; }
    qreal directionLength() const { return std::hypot(qreal(nx), qreal(ny)); }
    QPointF direction() const
    {
        const qreal length = directionLength();
        return length > 0.0 ? QPointF(nx, ny) / length : QPointF();
    }
    // Положение вершины при выносе extension: at(hw) - кромка без бахромы.
    QPointF at(qreal extension) const { return center() + direction() * extension; }
};
static_assert(sizeof(AxisVertex) == 6 * sizeof(float), "VBO и шейдер ждут 24 байта на вершину");

// Тесселяция штриха переменной ширины в треугольную полосу.
//
// Полоса, а не путь: чернила должны жить в графе сцены как обычная геометрия,
// иначе на каждом кадре пришлось бы растеризовать путь на CPU - ровно та
// проблема, из-за которой отвергнут cairo-подход (design.md, D4).
//
// Полоса несёт ось штриха (AxisVertex), а не готовые позиции: кромку выносит и
// сглаживает материал чернил (design.md ink-beauty-pass, D7). Веера вокруг
// центров стыков и торцов лежат в той же полосе повтором центральной вершины:
// C, O0, C, O1, ... - треугольники (C, Ok, C) вырождены, (Ok, C, Ok+1) - веер.
class StrokeTessellator
{
public:
    using Vertex = AxisVertex;

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
    enum class CapEnd { Start, End };
    // Шаг полувеера торца: π / 0.28 с округлением вверх - тот же шаг, что у
    // дуги стыка. Вершин в начальном торце 2·(K + 1) + 1, в конечном
    // 2 + 2·(K + 1).
    static constexpr int kCapFanSteps = 12;
    static constexpr int kStartCapVertices = 2 * (kCapFanSteps + 1) + 1;
    static constexpr int kEndCapVertices = 2 + 2 * (kCapFanSteps + 1);
    // Дальше стольких точек от конца хорда для направления торца не ищется:
    // столько же точек мокрый штрих держит под рукой, чтобы его торец совпал с
    // сухим, а работа на отсчёт не росла с длиной штриха.
    static constexpr int kCapLookback = 48;

    // Торец - полувеер вокруг точки (design.md, D4, D7): центр (d = 0)
    // чередуется с вершинами дуги радиуса полуширины; осевая кончается в точке
    // касания и в точке отрыва, а торец выступает за неё на полуширину.
    // Начальный торец кончается боковой парой c ± r·n, конечный с неё
    // начинается: так торец стыкуется с полосой. direction - направление
    // движения пера у этого конца.
    void emitCap(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                 const QPointF &direction, CapEnd end) const;
    // Направление торца - хорда до первой точки на расстоянии не меньше radius
    // от конца, но не дальше kCapLookback точек, а не последний отрезок: на
    // отрыве дотяжка до точки контакта дописывает отрезок короче шага со
    // случайным направлением. Вырожденный штрих - (1, 0).
    static QPointF capDirection(const QList<InkPoint> &points, CapEnd end, qreal radius);
    // Индекс точки, до которой идёт хорда торца: первая на расстоянии не
    // меньше radius от конца среди kCapLookback точек, а если таких нет, но
    // точек уже kCapLookback - последняя из них. -1, пока хорда не определена:
    // точек меньше kCapLookback и ни одна не отошла на radius. Одно правило и
    // для сухого торца, и для решения мокрого штриха, когда начальный торец
    // становится окончательным - только так мокрое совпадает с сухим.
    static int capChordIndex(const QList<InkPoint> &points, CapEnd end, qreal radius);
    // Стык: пара на биссектрисе на пологом повороте; на резком - внутренняя
    // вершина с точным d = ±(hw + e)·cos(θ/2) и веер внешней дуги вокруг центра
    // стыка.
    void emitJoint(QVector<Vertex> &strip, const QPointF &position, qreal halfWidth,
                   const QPointF &incoming, const QPointF &outgoing) const;

    // Замкнутый контур штриха для экспорта: дуга начального торца, левая
    // сторона, дуга конечного, правая сторона назад. Экспорт вписывает чернила
    // в PDF заливкой контура, а не линией постоянной толщины: только так
    // переменная ширина от давления переживает экспорт. Точки - там, где у
    // полосы на экране проходит кромка |d| = hw, из тех же примитивов:
    // стороны, дуги торцов, внешние дуги стыков (design.md, D7, «Экспорт»).
    // Заливать по ненулевому обходу: на петлях контур перекрывает сам себя.
    QPolygonF outline(const QList<InkPoint> &points) const;

    // Точка (design.md, D5): осевая короче полной толщины пера. Такой штрих
    // tessellate() рисует капсулой, а WetStroke дописывает к нему точку с
    // наибольшим нажимом касания - решение у обоих одно.
    bool isDot(const QList<InkPoint> &points) const;

    // Полуширина штриха в конкретной точке - по ней тесты проверяют, что
    // толщина следует за давлением.
    qreal halfWidthAt(const InkPoint &point) const;

    static bool isDegenerate(const QVector<Vertex> &strip);

private:
    // Осевая в том виде, в каком её обходят и полоса, и контур. Касание (D5)
    // уже свёрнуто в капсулу из двух точек.
    struct Axis
    {
        QList<QPointF> positions;
        QList<qreal> halfWidths;
        QList<QPointF> directions; // направления сегментов, на один меньше точек
        QPointF startDirection;    // направление движения пера у торцов
        QPointF endDirection;
    };
    Axis axisOf(const QList<InkPoint> &points) const;

    qreal m_width = 2.4;
    qreal m_minWidthFactor = 0.25;
    qreal m_constantPressure = 0.65;
};
