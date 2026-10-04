// Задача 5.3: тесселяция штриха переменной ширины в треугольную полосу со
// скруглением стыков. На эталонных траекториях с резкими изгибами и петлями
// проверяется отсутствие артефактов - разрывов на стыках и выбросов геометрии.

#include <QImage>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QTest>

#include "ink/stroketessellator.h"
#include "ink/wetstroke.h"
#include "support/referencestrokes.h"

namespace {

using Vertex = StrokeTessellator::Vertex;

// Вершина полосы «в покое» - без выноса бахромы, на полуширине. Это та же
// фигура, что раньше давала плоская полоса; у центра веера - сам центр.
QPointF toPoint(const Vertex &v)
{
    return v.at(v.hw);
}

bool nearPoint(const Vertex &v, const QPointF &p, qreal tolerance = 1e-3)
{
    return std::hypot(v.cx - p.x(), v.cy - p.y()) < tolerance;
}

bool insideTriangle(const QPointF &p, const QPointF &a, const QPointF &b, const QPointF &c)
{
    const auto sign = [](const QPointF &p1, const QPointF &p2, const QPointF &p3) {
        return (p1.x() - p3.x()) * (p2.y() - p3.y()) - (p2.x() - p3.x()) * (p1.y() - p3.y());
    };
    const qreal d1 = sign(p, a, b);
    const qreal d2 = sign(p, b, c);
    const qreal d3 = sign(p, c, a);
    const bool negative = (d1 < -1e-9) || (d2 < -1e-9) || (d3 < -1e-9);
    const bool positive = (d1 > 1e-9) || (d2 > 1e-9) || (d3 > 1e-9);
    return !(negative && positive);
}

// Покрыта ли точка невырожденным треугольником полосы. Вырожденные
// треугольники - (C, Ok, C) между вершинами веера, (C, L, R) у торца - это
// отрезки из центра: точка на таком отрезке оказалась бы «покрытой» и без
// веера.
bool stripCovers(const QVector<Vertex> &strip, const QPointF &point)
{
    for (int i = 0; i + 2 < strip.size(); ++i) {
        const QPointF a = toPoint(strip[i]);
        const QPointF b = toPoint(strip[i + 1]);
        const QPointF c = toPoint(strip[i + 2]);
        const QPointF ab = b - a;
        const QPointF ac = c - a;
        if (std::abs(ab.x() * ac.y() - ab.y() * ac.x()) < 1e-9)
            continue;
        if (insideTriangle(point, a, b, c))
            return true;
    }
    return false;
}

QPointF unit(const QPointF &v)
{
    const qreal length = std::hypot(v.x(), v.y());
    return length < 1e-9 ? QPointF() : v / length;
}

QPointF normalOf(const QPointF &direction)
{
    return QPointF(-direction.y(), direction.x());
}

// Точки, которые полоса обязана покрыть: ось, смещённая на ±0.5·hw - в
// стыках по биссектрисе нормалей, в серединах сегментов по нормали, у концов
// ещё и назад за начало и вперёд за конец, по направлению торца. Сама ось
// для этого не годится: в каждой её точке у резкого стыка и торца стоит
// центр веера, и она «покрыта» при любом веере.
QList<QPointF> coverageProbes(const QList<InkPoint> &points, const StrokeTessellator &tessellator)
{
    QList<QPointF> directions;
    QPointF previous(1.0, 0.0);
    for (int i = 0; i + 1 < points.size(); ++i) {
        QPointF direction = unit(points[i + 1].position - points[i].position);
        if (direction.isNull())
            direction = previous;
        directions.append(direction);
        previous = direction;
    }

    QList<QPointF> probes;
    const int last = int(points.size()) - 1;
    for (int i = 0; i <= last; ++i) {
        const QPointF c = points[i].position;
        const qreal offset = 0.5 * tessellator.halfWidthAt(points[i]);
        QPointF across;
        if (i == 0) {
            across = normalOf(directions.constFirst());
            probes.append(c - StrokeTessellator::capDirection(points, StrokeTessellator::CapEnd::Start,
                                                              2.0 * offset)
                                  * offset);
        } else if (i == last) {
            across = normalOf(directions.constLast());
            probes.append(c + StrokeTessellator::capDirection(points, StrokeTessellator::CapEnd::End,
                                                              2.0 * offset)
                                  * offset);
        } else {
            across = unit(normalOf(directions[i - 1]) + normalOf(directions[i]));
            if (across.isNull())
                across = normalOf(directions[i]);
        }
        probes.append(c + across * offset);
        probes.append(c - across * offset);
        if (i < last) {
            const QPointF middle = (c + points[i + 1].position) / 2.0;
            const qreal halfWidth = std::min(tessellator.halfWidthAt(points[i]),
                                             tessellator.halfWidthAt(points[i + 1]));
            probes.append(middle + normalOf(directions[i]) * (0.5 * halfWidth));
            probes.append(middle - normalOf(directions[i]) * (0.5 * halfWidth));
        }
    }
    return probes;
}

qreal distanceToPolyline(const QPointF &p, const QList<InkPoint> &points)
{
    qreal best = std::numeric_limits<qreal>::max();
    for (int i = 0; i + 1 < points.size(); ++i) {
        const QPointF a = points[i].position;
        const QPointF b = points[i + 1].position;
        const QPointF ab = b - a;
        const qreal lengthSquared = QPointF::dotProduct(ab, ab);
        qreal t = lengthSquared < 1e-12 ? 0.0 : QPointF::dotProduct(p - a, ab) / lengthSquared;
        t = qBound(0.0, t, 1.0);
        const QPointF projection = a + ab * t;
        best = qMin(best, std::hypot(p.x() - projection.x(), p.y() - projection.y()));
    }
    return best;
}

// --- ink-beauty-pass, D4 и D5 ------------------------------------------------

// Вершин в торцах: полувеер из K + 1 центров и K + 1 вершин дуги, у начального
// торца ещё правая вершина боковой пары, у конечного - вся боковая пара.
constexpr int kStartCapVertices = StrokeTessellator::kStartCapVertices;
constexpr int kEndCapVertices = StrokeTessellator::kEndCapVertices;

// Центр торца - центр любой его вершины: первая вершина полосы - центр
// начального полувеера, последняя - вершина дуги конечного.
QPointF capCenter(const QVector<Vertex> &strip, StrokeTessellator::CapEnd end)
{
    return end == StrokeTessellator::CapEnd::Start ? strip.constFirst().center()
                                                   : strip.constLast().center();
}

// Кончик торца - вершина дуги на оси штриха (φ = π/2), в покое.
QPointF capTip(const QVector<Vertex> &strip, StrokeTessellator::CapEnd end)
{
    const int half = StrokeTessellator::kCapFanSteps / 2;
    return end == StrokeTessellator::CapEnd::Start
        ? toPoint(strip[2 * half + 1])
        : toPoint(strip[int(strip.size()) - 1 - 2 * (StrokeTessellator::kCapFanSteps - half)]);
}

// Полные толщины поперечных пар: боковая пара начального торца, пары на
// биссектрисах, боковая пара конечного. У прямого штриха это ровно по одной
// паре на точку.
QList<qreal> sideWidths(const QVector<Vertex> &strip)
{
    QList<qreal> widths;
    for (int i = 0; i + 1 < strip.size(); ++i) {
        const Vertex &a = strip[i];
        const Vertex &b = strip[i + 1];
        if (a.isCenter() || b.isCenter() || a.s <= 0.0f || b.s != -a.s || a.cx != b.cx
            || a.cy != b.cy)
            continue;
        widths.append(QLineF(toPoint(a), toPoint(b)).length());
        ++i;
    }
    return widths;
}

// Площадь замкнутого многоугольника по формуле шнурования.
qreal polygonArea(const QPolygonF &polygon)
{
    qreal twice = 0.0;
    for (int i = 0; i + 1 < polygon.size(); ++i)
        twice += polygon[i].x() * polygon[i + 1].y() - polygon[i + 1].x() * polygon[i].y();
    return std::abs(twice) / 2.0;
}

QRectF boundsOf(const QVector<Vertex> &strip)
{
    QPolygonF polygon;
    for (const Vertex &vertex : strip)
        polygon.append(toPoint(vertex));
    return polygon.boundingRect();
}

qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    return distanceToPolyline(p, {{a, 0.5, 0.0}, {b, 0.5, 0.0}});
}

} // namespace

class TestStrokeTessellator : public QObject
{
    Q_OBJECT

private:
    // Растр контура экспорта при 4 px на точку, заливка по ненулевому обходу,
    // как в NoteExporter. Залит каждый пиксель, центр которого ближе hw - 0.5 px
    // к сегменту осевой (hw - меньшая из полуширин его концов), и ни один
    // дальше hw + 0.5 px (большая из полуширин). Штрих - то, что даёт
    // WetStroke на этих отсчётах.
    void checkOutlineFill(const QList<InkPoint> &input, qreal width, qreal zoom)
    {
        Stroke prototype;
        prototype.width = width;
        prototype.style = StrokeStyle::Tapered;
        WetStroke wet;
        wet.begin(0, prototype, zoom);
        for (const InkPoint &point : input)
            wet.addPoint(point);
        const Stroke stroke = wet.finish();
        QVERIFY(stroke.points.size() > 10);

        StrokeTessellator tessellator;
        tessellator.setWidth(stroke.width);
        tessellator.setStyle(stroke.style);
        QPainterPath outline;
        outline.addPolygon(tessellator.outline(stroke.points));
        outline.setFillRule(Qt::WindingFill);

        constexpr qreal pxPerUnit = 4.0;
        const QRectF bounds = outline.boundingRect().adjusted(-2.0, -2.0, 2.0, 2.0);
        QImage image(int(std::ceil(bounds.width() * pxPerUnit)),
                     int(std::ceil(bounds.height() * pxPerUnit)), QImage::Format_Grayscale8);
        image.fill(255);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.scale(pxPerUnit, pxPerUnit);
            painter.translate(-bounds.topLeft());
            painter.fillPath(outline, Qt::black);
        }

        const QList<InkPoint> &points = stroke.points;
        const qreal margin = 0.5 / pxPerUnit;
        const int size = image.width() * image.height();
        QList<bool> required(size, false);
        QList<bool> allowed(size, false);
        QList<qreal> depth(size, 0.0);
        for (int i = 0; i + 1 < points.size(); ++i) {
            const QPointF a = points[i].position;
            const QPointF b = points[i + 1].position;
            const qreal hwA = tessellator.halfWidthAt(points[i]);
            const qreal hwB = tessellator.halfWidthAt(points[i + 1]);
            const qreal inner = std::min(hwA, hwB) - margin;
            const qreal outer = std::max(hwA, hwB) + margin;
            const QRectF box = QRectF(a, b).normalized().adjusted(-outer, -outer, outer, outer);
            const int x0 = std::max(0, int((box.left() - bounds.left()) * pxPerUnit));
            const int x1 = std::min(image.width() - 1, int((box.right() - bounds.left()) * pxPerUnit) + 1);
            const int y0 = std::max(0, int((box.top() - bounds.top()) * pxPerUnit));
            const int y1 = std::min(image.height() - 1, int((box.bottom() - bounds.top()) * pxPerUnit) + 1);
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const QPointF p = bounds.topLeft() + QPointF(x + 0.5, y + 0.5) / pxPerUnit;
                    const qreal distance = distanceToSegment(p, a, b);
                    const int k = y * image.width() + x;
                    if (distance <= outer)
                        allowed[k] = true;
                    if (distance <= inner) {
                        required[k] = true;
                        depth[k] = std::max(depth[k], inner - distance);
                    }
                }
            }
        }

        int holes = 0;
        int spills = 0;
        qreal deepest = 0.0;
        for (int y = 0; y < image.height(); ++y) {
            const uchar *row = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                const int k = y * image.width() + x;
                const bool filled = row[x] == 0;
                if (required[k] && !filled) {
                    ++holes;
                    deepest = std::max(deepest, depth[k] * pxPerUnit);
                }
                if (filled && !allowed[k])
                    ++spills;
            }
        }
        QVERIFY2(holes == 0,
                 qPrintable(QStringLiteral("незалитых пикселей внутри штриха: %1, до %2 px "
                                           "глубже допуска")
                                .arg(holes)
                                .arg(deepest)));
        QVERIFY2(spills == 0,
                 qPrintable(QStringLiteral("залитых пикселей за кромкой: %1").arg(spills)));
    }

    // Общая проверка «без артефактов» для любой траектории.
    void checkNoArtifacts(const QList<InkPoint> &points, const StrokeTessellator &tessellator,
                          const char *label)
    {
        const QVector<Vertex> strip = tessellator.tessellate(points);

        QVERIFY2(!StrokeTessellator::isDegenerate(strip), label);

        qreal maxHalfWidth = 0.0;
        for (const InkPoint &point : points)
            maxHalfWidth = qMax(maxHalfWidth, tessellator.halfWidthAt(point));

        // Ни одна вершина не улетает от средней линии дальше полуширины: так
        // ловится выброс на остром стыке, который дал бы шип.
        for (const Vertex &vertex : strip) {
            const qreal distance = distanceToPolyline(toPoint(vertex), points);
            QVERIFY2(distance <= maxHalfWidth + 0.01,
                     qPrintable(QStringLiteral("%1: вершина в %2 от средней линии, полуширина %3")
                                    .arg(QString::fromLatin1(label))
                                    .arg(distance)
                                    .arg(maxHalfWidth)));
        }

        // И штрих вокруг всей средней линии покрыт полосой - по обе стороны
        // оси, у стыков и за концами: значит, на стыках нет дыр, а веера на
        // месте и не вывернуты.
        const QList<QPointF> probes = coverageProbes(points, tessellator);
        for (int i = 0; i < probes.size(); ++i) {
            QVERIFY2(stripCovers(strip, probes[i]),
                     qPrintable(QStringLiteral("%1: точка (%2, %3) у средней линии не покрыта")
                                    .arg(QString::fromLatin1(label))
                                    .arg(probes[i].x())
                                    .arg(probes[i].y())));
        }
    }

private slots:
    void straightStrokeHasRequestedWidth()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        tessellator.setMinWidthFactor(0.0);

        QList<InkPoint> points;
        for (int i = 0; i < 20; ++i)
            points.append({QPointF(i * 5.0, 50.0), 1.0, i / 200.0});

        const QVector<Vertex> strip = tessellator.tessellate(points);
        // По паре на каждую внутреннюю точку и по полудиску на каждом конце.
        QCOMPARE(strip.size(), (points.size() - 2) * 2 + kStartCapVertices + kEndCapVertices);

        const QList<qreal> widths = sideWidths(strip);
        QCOMPARE(widths.size(), points.size());
        for (int i = 0; i < widths.size(); ++i) {
            QVERIFY2(qAbs(widths[i] - 4.0) < 1e-4,
                     qPrintable(QStringLiteral("width %1 at %2").arg(widths[i]).arg(i)));
        }
    }

    // Сценарий «Штрих с нарастающим нажатием»: линия утолщается плавно, без
    // ступенек и разрывов.
    void widthFollowsPressureSmoothly()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(6.0);
        tessellator.setMinWidthFactor(0.2);

        QList<InkPoint> points;
        const int count = 60;
        for (int i = 0; i < count; ++i)
            points.append({QPointF(i * 3.0, 40.0), qreal(i) / (count - 1), i / 200.0});

        const QVector<Vertex> strip = tessellator.tessellate(points);
        const QList<qreal> widths = sideWidths(strip);

        QCOMPARE(widths.size(), count);
        // Монотонный рост.
        for (int i = 1; i < widths.size(); ++i)
            QVERIFY2(widths[i] >= widths[i - 1] - 1e-6, "толщина не растёт монотонно");
        // Без ступенек: шаг между соседями мал по сравнению с полным размахом.
        const qreal span = widths.constLast() - widths.constFirst();
        QVERIFY(span > 4.0);
        for (int i = 1; i < widths.size(); ++i)
            QVERIFY2(widths[i] - widths[i - 1] < span * 0.1, "ступенька в толщине");
        // Границы: минимум - от minWidthFactor, максимум - полная толщина.
        QVERIFY(qAbs(widths.constFirst() - 6.0 * 0.2) < 1e-4);
        QVERIFY(qAbs(widths.constLast() - 6.0) < 1e-4);
    }

    // Сценарий «Перо без данных о давлении»: постоянная толщина, без ошибок.
    void missingPressureGivesConstantWidth()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(3.0);
        tessellator.setConstantPressure(1.0);
        tessellator.setMinWidthFactor(0.0);

        QList<InkPoint> points;
        for (int i = 0; i < 25; ++i)
            points.append({QPointF(i * 4.0, 10.0), -1.0, i / 200.0});

        const QVector<Vertex> strip = tessellator.tessellate(points);
        QVERIFY(!StrokeTessellator::isDegenerate(strip));
        for (const qreal width : sideWidths(strip))
            QVERIFY(qAbs(width - 3.0) < 1e-4);
    }

    // ink-beauty-pass, D3, сценарии «Заострение новых штрихов» и «Перо без
    // данных о давлении»: Legacy - W·(0.25 + 0.75p), Tapered - W·(0.08 +
    // 0.92p), без давления оба - W·0.7375 при давлении по умолчанию 0.65.
    void styleSelectsThicknessLaw()
    {
        const auto widthAt = [](const StrokeTessellator &tessellator, qreal pressure) {
            return 2.0 * tessellator.halfWidthAt({QPointF(0, 0), pressure, 0.0});
        };
        const auto near = [](qreal a, qreal b) { return qAbs(a - b) < 1e-9; };

        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        // По умолчанию - прежний закон: им рисуются штрихи старых заметок.
        QVERIFY(near(widthAt(tessellator, 0.0), 4.0 * 0.25));

        tessellator.setStyle(StrokeStyle::Tapered);
        QVERIFY(near(widthAt(tessellator, 0.0), 4.0 * 0.08));
        QVERIFY(near(widthAt(tessellator, 0.5), 4.0 * (0.08 + 0.92 * 0.5)));
        QVERIFY(near(widthAt(tessellator, 1.0), 4.0));
        QVERIFY2(near(widthAt(tessellator, -1.0), 4.0 * 0.7375),
                 qPrintable(QStringLiteral("без давления %1").arg(widthAt(tessellator, -1.0))));

        tessellator.setStyle(StrokeStyle::Legacy);
        QVERIFY(near(widthAt(tessellator, 0.0), 4.0 * 0.25));
        QVERIFY(near(widthAt(tessellator, 0.5), 4.0 * (0.25 + 0.75 * 0.5)));
        QVERIFY(near(widthAt(tessellator, 1.0), 4.0));
        QVERIFY(near(widthAt(tessellator, -1.0), 4.0 * 0.7375));

        // И полоса идёт по тому же закону: прямая с нулевым нажимом.
        tessellator.setStyle(StrokeStyle::Tapered);
        QList<InkPoint> points;
        for (int i = 0; i < 10; ++i)
            points.append({QPointF(i * 5.0, 50.0), 0.0, i / 200.0});
        const QVector<Vertex> strip = tessellator.tessellate(points);
        QCOMPARE(strip.size(), (points.size() - 2) * 2 + kStartCapVertices + kEndCapVertices);
        // Полуширина у каждой вершины - та же, что в поперечных парах.
        for (const Vertex &v : strip)
            QVERIFY2(qAbs(v.hw - 4.0f * 0.08f / 2.0f) < 1e-5f,
                     qPrintable(QStringLiteral("hw %1").arg(v.hw)));
        const QList<qreal> widths = sideWidths(strip);
        QCOMPARE(widths.size(), points.size());
        for (int i = 0; i < widths.size(); ++i) {
            QVERIFY2(qAbs(widths[i] - 4.0 * 0.08) < 1e-4,
                     qPrintable(QStringLiteral("толщина %1 в паре %2").arg(widths[i]).arg(i)));
        }
    }

    void loopHasNoArtifacts()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(5.0);
        checkNoArtifacts(reference::fromPositions(reference::loop()), tessellator, "loop");
    }

    void sharpZigzagHasNoArtifacts()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(5.0);
        checkNoArtifacts(reference::fromPositions(reference::sharpZigzag()), tessellator,
                         "sharp zigzag");
    }

    void tightSpiralHasNoArtifacts()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        checkNoArtifacts(reference::fromPositions(reference::tightSpiral()), tessellator,
                         "tight spiral");
    }

    void letterHasNoArtifacts()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(3.0);
        checkNoArtifacts(reference::fromPositions(reference::letterE()), tessellator, "letter e");
    }

    // Разворот почти на 180 градусов - худший стык, который вообще бывает.
    void hairpinTurnHasNoArtifacts()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(6.0);

        QList<InkPoint> points;
        for (int i = 0; i < 20; ++i)
            points.append({QPointF(i * 3.0, 0.0), 0.8, i / 200.0});
        for (int i = 1; i <= 20; ++i)
            points.append({QPointF(57.0 - i * 3.0, 0.6), 0.8, (19 + i) / 200.0});

        checkNoArtifacts(points, tessellator, "hairpin");
    }

    // Стык скругляется: на резком повороте появляются промежуточные вершины,
    // которых не было бы при простом усечении угла.
    void sharpTurnProducesRoundJoinVertices()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(6.0);

        const QList<InkPoint> gentle = {{QPointF(0, 0), 1.0, 0.0},
                                        {QPointF(10, 0), 1.0, 0.01},
                                        {QPointF(20, 1), 1.0, 0.02}};
        const QList<InkPoint> sharp = {{QPointF(0, 0), 1.0, 0.0},
                                       {QPointF(10, 0), 1.0, 0.01},
                                       {QPointF(10, 10), 1.0, 0.02}};

        // Два торца и одна пара на стыке, без дуги.
        QCOMPARE(tessellator.tessellate(gentle).size(), kStartCapVertices + kEndCapVertices + 2);
        QVERIFY2(tessellator.tessellate(sharp).size() > kStartCapVertices + kEndCapVertices + 2,
                 "стык не скруглён");
    }

    // --- ink-beauty-pass, D4: круглые торцы ---------------------------------

    // Сценарий «Отрыв пера»: осевая кончается в точке отрыва, а торец -
    // полудиск радиуса полуширины с центром в этой точке, выступающий за неё
    // на полуширину. То же у точки касания.
    void capsAreHalfDiscsAroundStrokeEnds()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        tessellator.setMinWidthFactor(0.0);

        // Нажим растёт: у начала полуширина 1, у конца 2.
        QList<InkPoint> points;
        for (int i = 0; i < 20; ++i)
            points.append({QPointF(i * 5.0, 50.0), 0.5 + 0.5 * i / 19.0, i / 200.0});
        const QPointF first = points.constFirst().position;
        const QPointF last = points.constLast().position;
        const qreal r0 = tessellator.halfWidthAt(points.constFirst());
        const qreal r1 = tessellator.halfWidthAt(points.constLast());

        const QVector<Vertex> strip = tessellator.tessellate(points);
        QVERIFY(strip.size() >= kStartCapVertices + kEndCapVertices);

        // Центры вееров стоят в самой точке, остальные вершины торца - на дуге.
        for (int i = 0; i < kStartCapVertices; ++i) {
            if (strip[i].isCenter())
                continue;
            const qreal d = QLineF(toPoint(strip[i]), first).length();
            QVERIFY2(qAbs(d - r0) <= 0.05 * r0,
                     qPrintable(QStringLiteral("начальный торец: вершина %1 в %2 от центра, "
                                               "радиус %3").arg(i).arg(d).arg(r0)));
        }
        for (int i = int(strip.size()) - kEndCapVertices; i < strip.size(); ++i) {
            if (strip[i].isCenter())
                continue;
            const qreal d = QLineF(toPoint(strip[i]), last).length();
            QVERIFY2(qAbs(d - r1) <= 0.05 * r1,
                     qPrintable(QStringLiteral("конечный торец: вершина %1 в %2 от центра, "
                                               "радиус %3").arg(i).arg(d).arg(r1)));
        }

        // Кончики торцов - на оси, на полуширину за концами осевой.
        QVERIFY(QLineF(capTip(strip, StrokeTessellator::CapEnd::Start), first - QPointF(r0, 0.0)).length() < 1e-4);
        QVERIFY(QLineF(capTip(strip, StrokeTessellator::CapEnd::End), last + QPointF(r1, 0.0)).length() < 1e-4);
        const QRectF bounds = boundsOf(strip);
        QVERIFY(qAbs(bounds.left() - (first.x() - r0)) < 1e-4);
        QVERIFY(qAbs(bounds.right() - (last.x() + r1)) < 1e-4);

        // Центры дуг - концы осевой.
        QVERIFY(QLineF(capCenter(strip, StrokeTessellator::CapEnd::Start), first).length() < 1e-4);
        QVERIFY(QLineF(capCenter(strip, StrokeTessellator::CapEnd::End), last).length() < 1e-4);
    }

    // Направление торца - по хорде длиной не меньше радиуса, а не по
    // последнему отрезку: дотяжка до точки отрыва дописывает отрезок короче
    // шага в случайную сторону.
    void capDirectionFollowsChordNotLastSegment()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(2.4);
        tessellator.setMinWidthFactor(0.0);

        QList<InkPoint> points;
        for (int i = 0; i < 20; ++i)
            points.append({QPointF(i * 1.2, 50.0), 1.0, i / 200.0});
        // Крючок 0.39 под 50° - короче радиуса 1.2.
        points.append({points.constLast().position + QPointF(0.25, 0.3), 1.0, 20 / 200.0});

        const qreal r = tessellator.halfWidthAt(points.constLast());
        const QPointF end = points.constLast().position;
        // Первая точка назад на расстоянии не меньше r - третья с конца.
        const QPointF far = points[points.size() - 3].position;
        const QPointF chord = (end - far) / QLineF(far, end).length();

        const QPointF direction =
            StrokeTessellator::capDirection(points, StrokeTessellator::CapEnd::End, r);
        QVERIFY2(QLineF(direction, chord).length() < 1e-9,
                 qPrintable(QStringLiteral("направление (%1, %2), хорда (%3, %4)")
                                .arg(direction.x()).arg(direction.y())
                                .arg(chord.x()).arg(chord.y())));

        const QVector<Vertex> strip = tessellator.tessellate(points);
        const QPointF tip = capTip(strip, StrokeTessellator::CapEnd::End);
        QVERIFY2(QLineF(tip, end + chord * r).length() < 1e-4,
                 qPrintable(QStringLiteral("кончик торца в (%1, %2)").arg(tip.x()).arg(tip.y())));

        // В начале - то же, только направление от касания вперёд.
        QList<InkPoint> reversed(points.crbegin(), points.crend());
        QVERIFY(QLineF(StrokeTessellator::capDirection(reversed, StrokeTessellator::CapEnd::Start, r),
                       -chord).length() < 1e-9);
    }

    // Хорда ищется не дальше kCapLookback точек: работа мокрого штриха на
    // отсчёт не должна зависеть от того, как долго перо стояло у конца.
    void capDirectionLooksBackBoundedDistance()
    {
        QList<InkPoint> points;
        for (int i = 0; i < 60; ++i)
            points.append({QPointF(0.0, i * 0.01), 1.0, i / 200.0});
        points.append({QPointF(50.0, 0.6), 1.0, 60 / 200.0});

        // Среди первых kCapLookback точек ни одна не отошла на радиус: хорда -
        // до последней из них, а не до далёкой точки за ними.
        const QPointF direction =
            StrokeTessellator::capDirection(points, StrokeTessellator::CapEnd::Start, 1.0);
        QVERIFY2(QLineF(direction, QPointF(0.0, 1.0)).length() < 1e-9,
                 qPrintable(QStringLiteral("(%1, %2)").arg(direction.x()).arg(direction.y())));

        // Все точки в одном месте - направление по умолчанию.
        const QList<InkPoint> stacked(5, InkPoint{QPointF(3, 3), 0.5, 0.0});
        QCOMPARE(StrokeTessellator::capDirection(stacked, StrokeTessellator::CapEnd::End, 1.0),
                 QPointF(1.0, 0.0));
    }

    // Экспорт заливает outline(): с полудисками он остаётся замкнутым
    // контуром, а его площадь - прямоугольник плюс круг.
    void outlineIsClosedContourWithRoundEnds()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        tessellator.setMinWidthFactor(0.0);

        QList<InkPoint> points;
        for (int i = 0; i <= 20; ++i)
            points.append({QPointF(10.0 + i * 2.0, 30.0), 1.0, i / 200.0});

        const QPolygonF outline = tessellator.outline(points);
        QVERIFY(outline.size() > 2 * (StrokeTessellator::kCapFanSteps + 1));
        QCOMPARE(outline.constFirst(), outline.constLast());

        const qreal r = 2.0;
        const qreal expected = 40.0 * 2.0 * r + M_PI * r * r;
        const qreal area = polygonArea(outline);
        QVERIFY2(qAbs(area - expected) <= 0.005 * expected,
                 qPrintable(QStringLiteral("площадь %1, ждали %2").arg(area).arg(expected)));
        // Ни одна вершина контура не дальше полуширины от осевой.
        for (const QPointF &p : outline)
            QVERIFY(distanceToPolyline(p, points) <= r + 1e-4);
    }

    // Экспорт заливает outline() по ненулевому обходу (NoteExporter). На
    // острых поворотах настоящего штриха - выход StrokeSampler, отсчёты
    // гуще полуширины - внутренняя сторона контура заходит сама за себя, и
    // петля не должна выйти обратного обхода: иначе в PDF внутри острия
    // остаётся незалитая щель. Штрих снят на зуме 1 и 4: на большом зуме
    // отсчёты гуще.
    void outlineFillsSharpTurnsOfSampledStrokes_data()
    {
        QTest::addColumn<QList<QPointF>>("path");
        QTest::addColumn<qreal>("dwell");
        QTest::addColumn<qreal>("width");
        QTest::addColumn<qreal>("zoom");
        const QList<std::pair<const char *, QList<QPointF>>> paths = {
            {"зигзаг", reference::sharpZigzag()}, {"N", reference::cornerN()}};
        for (const auto &[name, path] : paths) {
            for (const qreal dwell : {0.0, 0.03}) {
                for (const qreal width : {2.4, 4.5}) {
                    for (const qreal zoom : {1.0, 4.0}) {
                        QTest::addRow("%s, стоянка %g с, перо %g, зум %g", name, dwell, width, zoom)
                            << path << dwell << width << zoom;
                    }
                }
            }
        }
    }

    void outlineFillsSharpTurnsOfSampledStrokes()
    {
        QFETCH(QList<QPointF>, path);
        QFETCH(qreal, dwell);
        QFETCH(qreal, width);
        QFETCH(qreal, zoom);
        checkOutlineFill(reference::sampledAtSpeed(path, 600.0, 240.0, dwell), width, zoom);
    }

    // На гладких штрихах контур - тот же штрих, что и полоса: залито всё
    // ближе hw - 0.5 px к осевой и ничего дальше hw + 0.5 px.
    void outlineMatchesSmoothSampledStrokes_data()
    {
        QTest::addColumn<QList<QPointF>>("path");
        QTest::addColumn<qreal>("width");
        QTest::addColumn<qreal>("zoom");
        const QList<std::pair<const char *, QList<QPointF>>> paths = {
            {"петля", reference::loop()},
            {"буква e", reference::letterE()},
            {"окружность", reference::circle(20.0, 1, QPointF(60, 60), 360)}};
        for (const auto &[name, path] : paths) {
            for (const qreal width : {2.4, 4.5}) {
                for (const qreal zoom : {1.0, 4.0})
                    QTest::addRow("%s, перо %g, зум %g", name, width, zoom) << path << width << zoom;
            }
        }
    }

    void outlineMatchesSmoothSampledStrokes()
    {
        QFETCH(QList<QPointF>, path);
        QFETCH(qreal, width);
        QFETCH(qreal, zoom);
        checkOutlineFill(reference::sampledAtSpeed(path, 300.0, 240.0), width, zoom);
    }

    // Сценарий «Точка над буквой» (D5) в его геометрической части: касание
    // без сдвига - две точки в одном месте - с полудисками на концах уже
    // диск диаметром в толщину при этом нажиме, и в экспорте это круг.
    void tapInOnePlaceIsDisc()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(3.0);
        const QPointF center(10.0, 10.0);
        const QList<InkPoint> points = {{center, 0.6, 0.0}, {center, 0.6, 0.01}};
        const qreal r = tessellator.halfWidthAt(points.constFirst());

        const QVector<Vertex> strip = tessellator.tessellate(points);
        QVERIFY(!StrokeTessellator::isDegenerate(strip));
        QCOMPARE(strip.size(), kStartCapVertices + kEndCapVertices);
        for (const Vertex &vertex : strip) {
            if (!vertex.isCenter())
                QVERIFY(qAbs(QLineF(toPoint(vertex), center).length() - r) < 1e-4);
        }

        const QRectF bounds = boundsOf(strip);
        QVERIFY(qAbs(bounds.width() - 2.0 * r) < 1e-4);
        QVERIFY(qAbs(bounds.height() - 2.0 * r) < 1e-4);

        const qreal area = polygonArea(tessellator.outline(points));
        QVERIFY2(qAbs(area - M_PI * r * r) <= 0.02 * M_PI * r * r,
                 qPrintable(QStringLiteral("площадь %1, круг %2").arg(area).arg(M_PI * r * r)));
    }

    // --- ink-beauty-pass, D5: точка от касания --------------------------------

    // Сценарий «Касание с едва заметным сдвигом»: осевая короче толщины пера -
    // капсула с наибольшей полушириной, а не чёрточка. Штрих длиннее толщины -
    // обычный, торцы по своим полуширинам.
    void strokeShorterThanPenWidthIsCapsule()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(3.0);

        const QList<InkPoint> tap = {{QPointF(10.0, 10.0), 0.2, 0.0},
                                     {QPointF(10.5, 10.0), 0.9, 0.01},
                                     {QPointF(11.0, 10.0), 0.3, 0.02}};
        const qreal r = tessellator.halfWidthAt(tap[1]);
        const QVector<Vertex> strip = tessellator.tessellate(tap);
        QVERIFY(!StrokeTessellator::isDegenerate(strip));
        for (const Vertex &vertex : strip) {
            if (vertex.isCenter())
                continue;
            const qreal d = distanceToSegment(toPoint(vertex), tap.constFirst().position,
                                              tap.constLast().position);
            QVERIFY2(qAbs(d - r) < 1e-4,
                     qPrintable(QStringLiteral("вершина в %1 от оси капсулы, радиус %2")
                                    .arg(d).arg(r)));
        }
        const QRectF bounds = boundsOf(strip);
        QVERIFY(qAbs(bounds.width() - (1.0 + 2.0 * r)) < 1e-4);
        QVERIFY(qAbs(bounds.height() - 2.0 * r) < 1e-4);

        // Осевая 4.5 при толщине 3 - уже не точка: начальный торец по своей
        // полуширине, а не по наибольшей.
        const QList<InkPoint> line = {{QPointF(10.0, 10.0), 0.2, 0.0},
                                      {QPointF(12.25, 10.0), 0.9, 0.01},
                                      {QPointF(14.5, 10.0), 0.3, 0.02}};
        const QVector<Vertex> longer = tessellator.tessellate(line);
        const qreal r0 = tessellator.halfWidthAt(line.constFirst());
        QVERIFY(QLineF(capTip(longer, StrokeTessellator::CapEnd::Start), QPointF(10.0 - r0, 10.0)).length() < 1e-4);
    }

    // ink-beauty-pass, D7: центр каждой вершины - точка осевой; направление
    // выноса нулевое (центр веера) или единичное; |s| не больше единицы.
    void axisVerticesAreWellFormed()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(5.0);
        const QList<InkPoint> points = reference::fromPositions(reference::letterE());
        const QVector<Vertex> strip = tessellator.tessellate(points);
        QVERIFY(!StrokeTessellator::isDegenerate(strip));

        for (int i = 0; i < strip.size(); ++i) {
            const Vertex &v = strip[i];
            bool onAxis = false;
            for (const InkPoint &point : points) {
                if (nearPoint(v, point.position)) {
                    onAxis = true;
                    break;
                }
            }
            QVERIFY2(onAxis, qPrintable(QStringLiteral("центр вершины %1 не на осевой").arg(i)));
            QVERIFY(v.hw > 0.0f);
            QVERIFY(std::abs(v.s) <= 1.0f + 1e-6f);
            if (v.isCenter()) {
                QCOMPARE(v.s, 0.0f);
                continue;
            }
            QVERIFY2(std::abs(v.directionLength() - 1.0) < 1e-5,
                     qPrintable(QStringLiteral("|n| = %1 у вершины %2")
                                    .arg(v.directionLength())
                                    .arg(i)));
        }
    }

    // D7, «Стыки точные»: у внутренней вершины d = ±(hw + e)·cos(θ/2) - её
    // настоящее расстояние до прямых обоих сегментов при любом выносе.
    void innerJointVertexCarriesExactDistance()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        // Вправо по y = 0, затем вниз по x = 20: поворот на +90°.
        const QList<InkPoint> points = {{QPointF(0, 0), 1.0, 0.0},
                                        {QPointF(20, 0), 1.0, 0.01},
                                        {QPointF(20, 20), 1.0, 0.02}};
        const QVector<Vertex> strip = tessellator.tessellate(points);
        const qreal cosHalf = std::cos(M_PI / 4.0);

        int inner = 0;
        for (const Vertex &v : strip) {
            if (v.isCenter() || std::abs(std::abs(v.s) - 1.0f) < 1e-6f)
                continue;
            ++inner;
            QVERIFY(nearPoint(v, QPointF(20, 0)));
            QVERIFY(std::abs(std::abs(v.s) - cosHalf) < 1e-5);
            for (const qreal extension : {2.0, 3.0, 5.0}) {
                const QPointF p = v.at(extension);
                const qreal d = std::abs(v.s) * extension;
                QVERIFY(std::abs(std::abs(p.y()) - d) < 1e-4);        // до прямой y = 0
                QVERIFY(std::abs(std::abs(p.x() - 20.0) - d) < 1e-4); // до прямой x = 20
            }
        }
        QVERIFY2(inner >= 1, "у стыка нет внутренней вершины");
    }

    // D7: внешняя дуга стыка - веер вокруг центра стыка (d = 0), а не из
    // внутренней точки, как было.
    void roundJoinFansAroundJointCenter()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        const QList<InkPoint> points = {{QPointF(0, 0), 1.0, 0.0},
                                        {QPointF(20, 0), 1.0, 0.01},
                                        {QPointF(20, 20), 1.0, 0.02}};
        const QVector<Vertex> strip = tessellator.tessellate(points);

        int centers = 0;
        int arc = 0;
        for (const Vertex &v : strip) {
            if (!nearPoint(v, QPointF(20, 0)))
                continue;
            if (v.isCenter()) {
                ++centers;
                QCOMPARE(v.s, 0.0f);
            } else if (std::abs(std::abs(v.s) - 1.0f) < 1e-6f) {
                ++arc;
                // Снаружи поворота: от нормали входящего (0, -1) к нормали
                // исходящего (1, 0).
                const QPointF n = v.direction();
                QVERIFY(n.x() >= -1e-6 && n.y() <= 1e-6);
                QVERIFY(std::abs(v.directionLength() - 1.0) < 1e-5);
            }
        }
        QVERIFY2(centers >= 5, qPrintable(QStringLiteral("центров веера %1").arg(centers)));
        QVERIFY2(arc >= 6, qPrintable(QStringLiteral("вершин дуги %1").arg(arc)));
    }

    // D7, «Торцы»: полувеер с центральной вершиной; дуга - позади начала и
    // впереди конца штриха, полукругом от одной стороны до другой.
    void capsAreHalfFansAroundEndPoints()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        tessellator.setMinWidthFactor(0.0);
        QList<InkPoint> points;
        for (int i = 0; i < 10; ++i)
            points.append({QPointF(10 + i * 5.0, 50.0), 1.0, i / 200.0});
        const QVector<Vertex> strip = tessellator.tessellate(points);

        const QPointF first(10, 50);
        const QPointF last(55, 50);
        int startCenters = 0;
        int endCenters = 0;
        bool startBack = false;
        bool endFront = false;
        for (const Vertex &v : strip) {
            if (nearPoint(v, first)) {
                QVERIFY(std::abs(v.hw - 2.0f) < 1e-5f);
                if (v.isCenter()) {
                    ++startCenters;
                    continue;
                }
                QVERIFY2(v.direction().x() <= 1e-6, "дуга начального торца впереди начала");
                startBack = startBack || v.direction().x() < -0.999;
            } else if (nearPoint(v, last)) {
                QVERIFY(std::abs(v.hw - 2.0f) < 1e-5f);
                if (v.isCenter()) {
                    ++endCenters;
                    continue;
                }
                QVERIFY2(v.direction().x() >= -1e-6, "дуга конечного торца позади конца");
                endFront = endFront || v.direction().x() > 0.999;
            }
        }
        QCOMPARE(startCenters, StrokeTessellator::kCapFanSteps + 1);
        QCOMPARE(endCenters, StrokeTessellator::kCapFanSteps + 1);
        QVERIFY2(startBack && endFront, "дуга торца не доходит до оси");
    }

    // Пологий стык - пара на биссектрисе с s = ±cos(θ/2).
    void gentleJointPairCarriesHalfAngleCosine()
    {
        StrokeTessellator tessellator;
        tessellator.setWidth(6.0);
        const QList<InkPoint> points = {{QPointF(0, 0), 1.0, 0.0},
                                        {QPointF(10, 0), 1.0, 0.01},
                                        {QPointF(20, 1), 1.0, 0.02}};
        const QVector<Vertex> strip = tessellator.tessellate(points);
        const int startCap = 2 * (StrokeTessellator::kCapFanSteps + 1) + 1;
        QCOMPARE(strip.size(), startCap + 2 + 2 + 2 * (StrokeTessellator::kCapFanSteps + 1));

        const qreal cosHalf = std::cos(std::atan2(1.0, 10.0) / 2.0);
        const Vertex &left = strip[startCap];
        const Vertex &right = strip[startCap + 1];
        QVERIFY(nearPoint(left, QPointF(10, 0)) && nearPoint(right, QPointF(10, 0)));
        QVERIFY(std::abs(left.s - cosHalf) < 1e-5);
        QVERIFY(std::abs(right.s + cosHalf) < 1e-5);
        const QPointF l = left.direction();
        const QPointF r = right.direction();
        QVERIFY(std::abs(l.x() + r.x()) < 1e-5 && std::abs(l.y() + r.y()) < 1e-5);
    }

    void degenerateInputIsHandled()
    {
        StrokeTessellator tessellator;
        QVERIFY(tessellator.tessellate({}).isEmpty());
        QVERIFY(tessellator.tessellate({{QPointF(1, 1), 0.5, 0.0}}).isEmpty());

        // Все точки в одном месте: геометрии нет, но и падения тоже.
        QList<InkPoint> stacked;
        for (int i = 0; i < 5; ++i)
            stacked.append({QPointF(3, 3), 0.5, i / 200.0});
        const QVector<Vertex> strip = tessellator.tessellate(stacked);
        for (const Vertex &vertex : strip) {
            QVERIFY(std::isfinite(vertex.cx) && std::isfinite(vertex.cy));
            QVERIFY(std::isfinite(vertex.nx) && std::isfinite(vertex.ny));
            QVERIFY(std::isfinite(vertex.hw) && std::isfinite(vertex.s));
        }
    }
};

QTEST_APPLESS_MAIN(TestStrokeTessellator)
#include "tst_stroketessellator.moc"
