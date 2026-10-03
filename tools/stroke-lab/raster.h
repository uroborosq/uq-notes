#pragma once

// Программная растеризация по точкам выборки - эмуляция того, что делает GPU.
//
// Приложение рисует чернила QSGFlatColorMaterial (непрозрачный цвет, без
// смешивания) в окно с MSAA 4x (main.cpp: format.setSamples(4)). GPU проверяет
// для каждого пикселя 4 точки выборки: точка покрыта, если лежит внутри
// какого-нибудь треугольника; при resolve цвет пикселя - среднее по точкам.
// Буфер окна не sRGB, поэтому среднее берётся по значениям sRGB, без гаммы:
//   pixel = bg + (ink - bg) * (covered / 4).
// Шаблон точек - стандартный 4x (D3D10+/GL, его же даёт Mesa на Intel):
// (0.375,0.125) (0.875,0.375) (0.125,0.625) (0.625,0.875) в долях пикселя.
// Пересечения треугольников полосы на непрозрачном цвете ничего не меняют -
// покрытие считается объединением.
//
// Тот же движок с решёткой 8x8 (64 точки) - эталон «идеального»
// аналитического сглаживания: ошибка покрытия не больше 1/64.

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QPolygonF>

#include <cstdint>
#include <vector>

namespace lab {

struct SamplePattern
{
    std::vector<QPointF> offsets; // внутри пикселя, [0,1)

    static SamplePattern msaa4();
    static SamplePattern grid(int n); // n*n, n <= 8
};

class CoverageBuffer
{
public:
    CoverageBuffer(int width, int height, SamplePattern pattern);

    int width() const { return m_width; }
    int height() const { return m_height; }

    // Координаты - пиксели изображения (центр пикселя (x,y) в (x+0.5,y+0.5)).
    void addTriangle(const QPointF &a, const QPointF &b, const QPointF &c);
    void addTriangleStrip(const std::vector<QPointF> &strip);
    void addTriangles(const std::vector<QPointF> &triangles);
    // Заливка по правилу nonzero (все кольца - одно множество).
    void addPolygons(const std::vector<QPolygonF> &rings);
    // Круг (для эталонной «круглой» кисти).
    void addDisc(const QPointF &center, double radius);
    // Выпуклый четырёхугольник/многоугольник.
    void addConvex(const std::vector<QPointF> &poly);

    double coverage(int x, int y) const;
    // Без гаммы, как MSAA resolve в не-sRGB буфере.
    void compositeOnto(QImage &image, const QColor &ink) const;
    // С гаммой (линейное смешивание) - для вариантов, которым интересно.
    void compositeOntoLinear(QImage &image, const QColor &ink) const;

private:
    int m_width = 0;
    int m_height = 0;
    SamplePattern m_pattern;
    int m_count = 0;
    std::vector<uint64_t> m_mask;
};

} // namespace lab
