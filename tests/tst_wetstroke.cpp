// Задача 5.4: инкрементальное наращивание «мокрого» штриха. Проверяется, что
// на добавление точки приходится постоянная работа независимо от длины штриха,
// и что итог совпадает с полной тесселяцией.

#include <QElapsedTimer>
#include <QTest>

#include "ink/stroketessellator.h"
#include "ink/wetstroke.h"
#include "support/referencestrokes.h"

class TestWetStroke : public QObject
{
    Q_OBJECT

private:
    static Stroke prototype(qreal width = 3.0)
    {
        Stroke stroke;
        stroke.width = width;
        return stroke;
    }

private slots:
    // Главное свойство: уже уложенная геометрия не переписывается. На кадр
    // добавляется только новый сегмент.
    void settledGeometryIsNeverRewritten()
    {
        WetStroke wet;
        wet.begin(0, prototype());

        QVector<WetStroke::Vertex> snapshot;
        const QList<QPointF> path = reference::letterE(300);

        for (int i = 0; i < path.size(); ++i) {
            wet.addPoint({path[i], 0.5 + 0.4 * std::sin(i * 0.1), i / 200.0});

            // Всё, что уже объявлено окончательным, совпадает с тем, что было.
            const int common = qMin(snapshot.size(), wet.settledVertexCount());
            for (int v = 0; v < common; ++v) {
                QVERIFY2(qFuzzyCompare(snapshot[v].x + 1.0f, wet.strip()[v].x + 1.0f)
                             && qFuzzyCompare(snapshot[v].y + 1.0f, wet.strip()[v].y + 1.0f),
                         qPrintable(QStringLiteral("вершина %1 переписана на шаге %2")
                                        .arg(v)
                                        .arg(i)));
            }
            snapshot = wet.strip().mid(0, wet.settledVertexCount());
        }
    }

    // Работа на точку не растёт с длиной штриха.
    void perPointWorkDoesNotGrowWithStrokeLength()
    {
        WetStroke wet;
        wet.begin(0, prototype());

        int firstHundredVertices = 0;
        int lastHundredVertices = 0;

        for (int i = 0; i < 4000; ++i) {
            const qreal t = i * 0.01;
            const int added = wet.addPoint({QPointF(t * 12.0, 40.0 * std::sin(t)), 0.6, i / 200.0});
            if (i < 100)
                firstHundredVertices += added;
            if (i >= 3900)
                lastHundredVertices += added;
        }

        QVERIFY(firstHundredVertices > 0);
        QVERIFY(lastHundredVertices > 0);
        // Разброс объясняется кривизной, а не длиной: порядок тот же.
        const qreal ratio = qreal(lastHundredVertices) / firstHundredVertices;
        QVERIFY2(ratio < 3.0 && ratio > 0.33,
                 qPrintable(QStringLiteral("first %1, last %2")
                                .arg(firstHundredVertices)
                                .arg(lastHundredVertices)));
    }

    // Инкрементальный результат совпадает с полной тесселяцией той же
    // траектории: наращивание не срезает углы.
    void incrementalResultMatchesFullTessellation()
    {
        WetStroke wet;
        wet.begin(2, prototype(4.0));

        const QList<QPointF> path = reference::loop(160);
        for (int i = 0; i < path.size(); ++i)
            wet.addPoint({path[i], 0.7, i / 200.0});
        const Stroke finished = wet.finish();

        StrokeTessellator tessellator;
        tessellator.setWidth(4.0);
        const QVector<StrokeTessellator::Vertex> full = tessellator.tessellate(finished.points);

        QCOMPARE(wet.strip().size(), full.size());
        for (int i = 0; i < full.size(); ++i) {
            QVERIFY2(qAbs(wet.strip()[i].x - full[i].x) < 1e-3
                         && qAbs(wet.strip()[i].y - full[i].y) < 1e-3,
                     qPrintable(QStringLiteral("расхождение в вершине %1").arg(i)));
        }
    }

    void finishProducesStrokeOnItsPage()
    {
        WetStroke wet;
        Stroke proto = prototype(2.5);
        proto.color = QColor(10, 20, 30);
        wet.begin(5, proto);
        QVERIFY(wet.isActive());
        QCOMPARE(wet.page(), 5);

        for (int i = 0; i < 30; ++i)
            wet.addPoint({QPointF(i * 3.0, i * 1.5), 0.8, i / 200.0});

        const Stroke stroke = wet.finish();
        QVERIFY(!wet.isActive());
        QCOMPARE(stroke.page, 5);
        QCOMPARE(stroke.width, 2.5);
        QCOMPARE(stroke.color, QColor(10, 20, 30));
        QVERIFY(stroke.isValid());
    }

    void singleTapProducesNoStroke()
    {
        WetStroke wet;
        wet.begin(0, prototype());
        wet.addPoint({QPointF(10, 10), 0.5, 0.0});
        const Stroke stroke = wet.finish();
        QVERIFY(!stroke.isValid());
    }

    void geometryStaysFiniteThroughSharpTurns()
    {
        WetStroke wet;
        wet.begin(0, prototype(6.0));
        for (const QPointF &point : reference::sharpZigzag())
            wet.addPoint({point, 0.9, 0.0});
        wet.addPoint({QPointF(0, 0), 0.9, 1.0});
        wet.finish();

        QVERIFY(!StrokeTessellator::isDegenerate(wet.strip()));
    }
};

QTEST_APPLESS_MAIN(TestWetStroke)
#include "tst_wetstroke.moc"
