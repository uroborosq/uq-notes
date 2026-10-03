// Задача 5.3: тесселяция штриха переменной ширины в треугольную полосу со
// скруглением стыков. На эталонных траекториях с резкими изгибами и петлями
// проверяется отсутствие артефактов - разрывов на стыках и выбросов геометрии.

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
        QCOMPARE(strip.size(), points.size() * 2);

        for (int i = 0; i + 1 < strip.size(); i += 2) {
            const qreal width = std::hypot(strip[i].x - strip[i + 1].x,
                                           strip[i].y - strip[i + 1].y);
            QVERIFY2(qAbs(width - 4.0) < 1e-4,
                     qPrintable(QStringLiteral("width %1 at %2").arg(width).arg(i)));
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

        QList<qreal> widths;
        for (int i = 0; i + 1 < strip.size(); i += 2)
            widths.append(std::hypot(strip[i].x - strip[i + 1].x, strip[i].y - strip[i + 1].y));

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
        for (int i = 0; i + 1 < strip.size(); i += 2) {
            const qreal width = std::hypot(strip[i].x - strip[i + 1].x,
                                           strip[i].y - strip[i + 1].y);
            QVERIFY(qAbs(width - 3.0) < 1e-4);
        }
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
        QCOMPARE(strip.size(), points.size() * 2);
        for (int i = 0; i + 1 < strip.size(); i += 2) {
            const qreal width = std::hypot(strip[i].x - strip[i + 1].x, strip[i].y - strip[i + 1].y);
            QVERIFY2(qAbs(width - 4.0 * 0.08) < 1e-4,
                     qPrintable(QStringLiteral("толщина %1 в паре %2").arg(width).arg(i / 2)));
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

        QCOMPARE(tessellator.tessellate(gentle).size(), 6); // три пары, без дуги
        QVERIFY2(tessellator.tessellate(sharp).size() > 6, "стык не скруглён");
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
