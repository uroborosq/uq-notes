// Задача 5.3: тесселяция штриха переменной ширины в треугольную полосу со
// скруглением стыков. На эталонных траекториях с резкими изгибами и петлями
// проверяется отсутствие артефактов - разрывов на стыках и выбросов геометрии.

#include <QLineF>
#include <QTest>

#include "ink/stroketessellator.h"
#include "support/referencestrokes.h"

namespace {

using Vertex = StrokeTessellator::Vertex;

QPointF toPoint(const Vertex &v)
{
    return QPointF(v.x, v.y);
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

// Покрыта ли точка полосой. Разрыв на стыке - это как раз точка средней линии,
// которая ничем не покрыта.
bool stripCovers(const QVector<Vertex> &strip, const QPointF &point)
{
    for (int i = 0; i + 2 < strip.size(); ++i) {
        if (insideTriangle(point, toPoint(strip[i]), toPoint(strip[i + 1]), toPoint(strip[i + 2])))
            return true;
    }
    return false;
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

// Вершин в одном торце: K + 1 пар.
constexpr int kCapVertices = 2 * (StrokeTessellator::kCapPairs + 1);

// Середина боковой пары торца - центр его дуги. У начального торца боковая
// пара последняя, у конечного - первая.
QPointF capCenter(const QVector<Vertex> &strip, StrokeTessellator::CapEnd end)
{
    const int i = end == StrokeTessellator::CapEnd::Start ? kCapVertices - 2
                                                          : int(strip.size()) - kCapVertices;
    return (toPoint(strip[i]) + toPoint(strip[i + 1])) / 2.0;
}

// Полные толщины пар от боковой пары начального торца до боковой пары
// конечного. У прямого штриха это ровно по одной паре на точку.
QList<qreal> sideWidths(const QVector<Vertex> &strip)
{
    QList<qreal> widths;
    for (int i = kCapVertices - 2; i <= int(strip.size()) - kCapVertices; i += 2)
        widths.append(QLineF(toPoint(strip[i]), toPoint(strip[i + 1])).length());
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
    // Общая проверка «без артефактов» для любой траектории.
    void checkNoArtifacts(const QList<InkPoint> &points, const StrokeTessellator &tessellator,
                          const char *label)
    {
        const QVector<Vertex> strip = tessellator.tessellate(points);

        QVERIFY2(!StrokeTessellator::isDegenerate(strip), label);
        QVERIFY2(strip.size() % 2 == 0, label);

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

        // И вся средняя линия покрыта полосой: значит, на стыках нет дыр.
        for (int i = 0; i < points.size(); ++i) {
            QVERIFY2(stripCovers(strip, points[i].position),
                     qPrintable(QStringLiteral("%1: точка %2 средней линии не покрыта")
                                    .arg(QString::fromLatin1(label))
                                    .arg(i)));
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
        QCOMPARE(strip.size(), (points.size() - 2) * 2 + 2 * kCapVertices);

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
        QCOMPARE(strip.size(), (points.size() - 2) * 2 + 2 * kCapVertices);
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
        QCOMPARE(tessellator.tessellate(gentle).size(), 2 * kCapVertices + 2);
        QVERIFY2(tessellator.tessellate(sharp).size() > 2 * kCapVertices + 2, "стык не скруглён");
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
        QVERIFY(strip.size() >= 2 * kCapVertices);
        QCOMPARE(strip.size() % 2, 0);

        for (int i = 0; i < kCapVertices; ++i) {
            const qreal d = QLineF(toPoint(strip[i]), first).length();
            QVERIFY2(qAbs(d - r0) <= 0.05 * r0,
                     qPrintable(QStringLiteral("начальный торец: вершина %1 в %2 от центра, "
                                               "радиус %3").arg(i).arg(d).arg(r0)));
        }
        for (int i = int(strip.size()) - kCapVertices; i < strip.size(); ++i) {
            const qreal d = QLineF(toPoint(strip[i]), last).length();
            QVERIFY2(qAbs(d - r1) <= 0.05 * r1,
                     qPrintable(QStringLiteral("конечный торец: вершина %1 в %2 от центра, "
                                               "радиус %3").arg(i).arg(d).arg(r1)));
        }

        // Кончики торцов - на оси, на полуширину за концами осевой.
        QVERIFY(QLineF(toPoint(strip.constFirst()), first - QPointF(r0, 0.0)).length() < 1e-4);
        QVERIFY(QLineF(toPoint(strip.constLast()), last + QPointF(r1, 0.0)).length() < 1e-4);
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
        const QPointF tip = toPoint(strip.constLast());
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
        QVERIFY(outline.size() > 2 * kCapVertices);
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
        QCOMPARE(strip.size(), 2 * kCapVertices);
        for (const Vertex &vertex : strip)
            QVERIFY(qAbs(QLineF(toPoint(vertex), center).length() - r) < 1e-4);

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
        QVERIFY(QLineF(toPoint(longer.constFirst()), QPointF(10.0 - r0, 10.0)).length() < 1e-4);
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
            QVERIFY(std::isfinite(vertex.x));
            QVERIFY(std::isfinite(vertex.y));
        }
    }
};

QTEST_APPLESS_MAIN(TestStrokeTessellator)
#include "tst_stroketessellator.moc"
