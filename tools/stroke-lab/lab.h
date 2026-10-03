#pragma once

// Stroke lab: общий интерфейс варианта конвейера чернил.
//
// Вариант - потоковый объект, который делает с отсчётами пера то же, что
// приложение делает между beginInkAt / continueInkAt / endInk: принимает сырые
// точки (координаты страницы, время из события) и отдаёт
//  - «мокрое» состояние после каждого отсчёта: окончательные точки осевой линии
//    и провизорный хвост;
//  - после finish() - окончательную осевую линию с толщиной и геометрию для
//    растеризации (треугольники или контур).
//
// Один вариант = один .cpp в tools/stroke-lab/variants/ с макросом
// STROKE_LAB_VARIANT в конце. CMake подхватывает файлы сам.

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QPolygonF>
#include <QTransform>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ink/inktypes.h"

namespace lab {

// Параметры штриха - то, что приложение знает на касании.
struct StrokeSetup
{
    // Экранных логических пикселей в одной точке страницы (m_zoom приложения).
    qreal scale = 1.0;
    // Толщина при полном нажиме, в точках страницы (Stroke::width).
    qreal width = 1.0;
    QColor color = QColor(0x1c, 0x2a, 0x5a);
};

// Точка осевой линии в координатах страницы. width - полная толщина линии в
// этой точке, в точках страницы.
struct CenterPoint
{
    QPointF pos;
    qreal width = 0.0;
    qreal pressure = -1.0;
    qreal time = 0.0;
};

// Геометрия для растеризации, в координатах страницы.
struct Geometry
{
    enum class Kind {
        None,
        TriangleStrip, // vertices - полоса, как GL_TRIANGLE_STRIP
        Triangles,     // vertices - тройки
        Polygons,      // polygons - заливка по правилу nonzero
    };
    Kind kind = Kind::None;
    std::vector<QPointF> vertices;
    std::vector<QPolygonF> polygons;

    int vertexCount() const
    {
        if (kind == Kind::Polygons) {
            int n = 0;
            for (const QPolygonF &p : polygons)
                n += int(p.size());
            return n;
        }
        return int(vertices.size());
    }
    bool empty() const { return vertexCount() == 0; }
};

// Мокрое состояние после очередного отсчёта.
struct WetState
{
    // Сколько точек осевой линии уже окончательны (префикс итоговой линии).
    int settledCount = 0;
    // Окончательные точки с индекса from (аргумент wet()) до settledCount.
    std::vector<CenterPoint> settledFrom;
    // Провизорный хвост после последней окончательной точки.
    std::vector<CenterPoint> provisional;
    // Размер мокрой геометрии и её окончательной части - сколько вершин
    // перезаливается на кадр (vertexCount - settledVertexCount).
    int vertexCount = 0;
    int settledVertexCount = 0;
};

// Куда вариант рисует себя сам, если у него своя растеризация.
struct RasterTarget
{
    QImage *image = nullptr;      // Format_RGB32, фон уже залит
    QTransform pageToPixel;       // точки страницы -> пиксели изображения
    QColor color;                 // цвет чернил
};

class Variant
{
public:
    virtual ~Variant() = default;

    virtual void begin(const StrokeSetup &setup) = 0;
    // Отсчёт пера: координаты страницы, давление 0..1 (или -1), время события
    // с разрешением 1 мс. Вся работа на отсчёт - здесь: лаборатория меряет
    // время именно этого вызова (в приложении это WetStroke::addPoint вместе с
    // тесселяцией мокрой полосы).
    virtual void addEvent(const InkPoint &raw) = 0;
    // Мокрое состояние; вызывается после каждого addEvent и не меряется.
    // В out.settledFrom нужно положить окончательные точки с индекса from.
    virtual void wet(int from, WetState &out) const = 0;
    // Отрыв пера.
    virtual void finish() = 0;

    // Итог после finish(): окончательная осевая линия (то, что сохранится в
    // .uqn) и сухая геометрия.
    virtual std::vector<CenterPoint> centerline() const = 0;
    virtual Geometry geometry() const = 0;
    // Текущая мокрая геометрия - для снимка кадра посреди штриха. Необязательна.
    virtual Geometry wetGeometry() const { return {}; }

    // Своя растеризация сухой геометрии (например, аналитическое сглаживание
    // или вершинная альфа). false - лаборатория рисует сама эмуляцией MSAA 4x,
    // как приложение.
    virtual bool rasterize(const Geometry &, const RasterTarget &) const { return false; }
};

using Factory = std::function<std::unique_ptr<Variant>()>;

struct VariantInfo
{
    std::string name;
    std::string description;
    Factory factory;
};

std::vector<VariantInfo> &registry();

struct Registrar
{
    Registrar(std::string name, std::string description, Factory factory)
    {
        registry().push_back({std::move(name), std::move(description), std::move(factory)});
    }
};

} // namespace lab

#define STROKE_LAB_CONCAT_INNER(a, b) a##b
#define STROKE_LAB_CONCAT(a, b) STROKE_LAB_CONCAT_INNER(a, b)

// Регистрация варианта: STROKE_LAB_VARIANT(MyVariant, "my-variant", "описание");
#define STROKE_LAB_VARIANT(Class, name, description)                                      \
    static ::lab::Registrar STROKE_LAB_CONCAT(stroke_lab_registrar_, __LINE__)(           \
        name, description, [] { return std::unique_ptr<::lab::Variant>(new Class()); })
