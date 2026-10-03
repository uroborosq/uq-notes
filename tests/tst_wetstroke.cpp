// Задача 5.4: инкрементальное наращивание «мокрого» штриха. Проверяется, что
// на добавление точки приходится постоянная работа независимо от длины штриха,
// и что итог совпадает с полной тесселяцией.

#include <QElapsedTimer>
#include <QLineF>
#include <QTest>

#include "ink/stroketessellator.h"
#include "ink/wetstroke.h"
#include "support/referencestrokes.h"

namespace {

// Вершин в одном торце: K + 1 пар (design.md, D4).
constexpr int kCapVertices = 2 * (StrokeTessellator::kCapPairs + 1);

QPointF toPoint(const WetStroke::Vertex &v)
{
    return QPointF(v.x, v.y);
}

// Центр дуги конечного торца. Торец - последние kCapVertices вершин полосы,
// его первая пара - боковая, c + r·n и c - r·n, и её середина - центр c.
// Кончик торца (последняя пара) лежит на полуширину дальше центра, поэтому
// «конец линии» - центр дуги, а не середина последней пары.
QPointF endCapCenter(const QVector<WetStroke::Vertex> &strip)
{
    const int i = int(strip.size()) - kCapVertices;
    return (toPoint(strip[i]) + toPoint(strip[i + 1])) / 2.0;
}

bool sameStrip(const QVector<WetStroke::Vertex> &a, const QVector<WetStroke::Vertex> &b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i) {
        if (qAbs(a[i].x - b[i].x) >= 1e-3 || qAbs(a[i].y - b[i].y) >= 1e-3)
            return false;
    }
    return true;
}

} // namespace

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

    // --- rnote-grade-stroke-fidelity, задача 5.2 --------------------------

    // Сценарий «Кончик линии у пера»: центр дуги торца после каждого отсчёта
    // стоит у последнего положения пера.
    void stripEndFollowsThePenOnFastArc()
    {
        WetStroke wet;
        wet.begin(0, prototype(3.0));
        const QList<InkPoint> input =
            reference::sampledAtSpeed(reference::circle(80.0, 1, QPointF(200, 200)), 1200.0, 240.0);

        for (int i = 0; i < input.size(); ++i) {
            wet.addPoint(input[i]);
            if (wet.strip().size() < 2 * kCapVertices)
                continue;
            const QPointF end = endCapCenter(wet.strip());
            const qreal lag = QLineF(end, input[i].position).length();
            QVERIFY2(lag <= 2.0,
                     qPrintable(QStringLiteral("конец линии отстал на %1 на отсчёте %2")
                                    .arg(lag)
                                    .arg(i)));
        }
    }

    // ink-beauty-pass, D4: торцы круглые и в окончательной части, и в
    // провизорной - на каждом отсчёте.
    void wetCapsAreRoundAtEveryStep()
    {
        WetStroke wet;
        wet.begin(0, prototype(3.0));
        const QList<InkPoint> input =
            reference::sampledAtSpeed(reference::loop(), 600.0, 481.0);

        int checked = 0;
        for (int i = 0; i < input.size(); ++i) {
            wet.addPoint(input[i]);
            if (wet.settledVertexCount() < kCapVertices)
                continue;
            const QVector<WetStroke::Vertex> &strip = wet.strip();
            // Начальный торец уже окончательный: полудиск вокруг первой точки.
            const InkPoint &first = wet.points().constFirst();
            const qreal r0 = wet.tessellator().halfWidthAt(first);
            for (int v = 0; v < kCapVertices; ++v)
                QVERIFY(qAbs(QLineF(toPoint(strip[v]), first.position).length() - r0) <= 0.05 * r0);
            // Конечный торец провизорный: полудиск вокруг конца хвоста.
            const InkPoint &tip = wet.provisionalPoints().isEmpty()
                ? wet.points().constLast()
                : wet.provisionalPoints().constLast();
            const qreal r1 = wet.tessellator().halfWidthAt(tip);
            for (int v = int(strip.size()) - kCapVertices; v < strip.size(); ++v)
                QVERIFY(qAbs(QLineF(toPoint(strip[v]), tip.position).length() - r1) <= 0.05 * r1);
            ++checked;
        }
        QVERIFY2(checked > 100, qPrintable(QStringLiteral("проверено отсчётов: %1").arg(checked)));
    }

    // На отрыве мокрый торец совпадает с сухим и смотрит по хорде, даже если
    // дотяжка до точки контакта дописала короткий отрезок вбок.
    void hookedEndMatchesDryAndFollowsChord()
    {
        WetStroke wet;
        wet.begin(0, prototype(3.0));
        QList<InkPoint> input =
            reference::sampledAtSpeed(reference::straightLine(100, 120.0), 300.0, 240.0);
        input.append({input.constLast().position + QPointF(0.3, 0.4), 0.6,
                      input.constLast().timeSeconds + 1.0 / 240.0});
        for (const InkPoint &point : input)
            wet.addPoint(point);
        const Stroke stroke = wet.finish();

        StrokeTessellator tessellator;
        tessellator.setWidth(stroke.width);
        tessellator.setStyle(stroke.style);
        QVERIFY(sameStrip(wet.strip(), tessellator.tessellate(stroke.points)));

        // Кончик торца - почти по оси x, а не под 53°, как крючок.
        const QPointF end = stroke.points.constLast().position;
        const QPointF tip = toPoint(wet.strip().constLast());
        const qreal angle = std::atan2(tip.y() - end.y(), tip.x() - end.x()) * 180.0 / M_PI;
        QVERIFY2(qAbs(angle) < 20.0, qPrintable(QStringLiteral("торец под %1°").arg(angle)));
    }

    // Сценарий «Резкая смена направления»: после разворота провизорная часть
    // полосы не торчит дальше, чем прошло само перо.
    void provisionalTailDoesNotOvershootReversal()
    {
        const qreal width = 3.0;
        WetStroke wet;
        wet.begin(0, prototype(width));
        const QList<InkPoint> input = reference::sampledAtSpeed(
            {QPointF(40, 100), QPointF(200, 100), QPointF(40, 100)}, 1000.0, 240.0);

        int reversal = -1;
        qreal reachX = 0.0; // докуда дошло перо и окончательная траектория
        for (int i = 0; i < input.size(); ++i) {
            wet.addPoint(input[i]);
            reachX = qMax(reachX, input[i].position.x());
            for (const InkPoint &point : wet.points())
                reachX = qMax(reachX, point.position.x());
            if (reversal < 0 && i > 0 && input[i].position.x() < input[i - 1].position.x())
                reversal = i;
            // Отсчёт разворота и следующий за ним.
            if (reversal < 0 || i < reversal + 1)
                continue;

            for (int v = wet.settledVertexCount(); v < wet.strip().size(); ++v) {
                QVERIFY2(wet.strip()[v].x <= reachX + width / 2.0 + 1.5,
                         qPrintable(QStringLiteral("хвост торчит до x=%1 при пути до %2, отсчёт %3")
                                        .arg(wet.strip()[v].x)
                                        .arg(reachX)
                                        .arg(i)));
            }
        }
        QVERIFY(reversal > 0);
    }

    // Сценарий «Нанесённый штрих без предварительного участка»: завершённый
    // штрих - это окончательные точки плюс дотяжка до отрыва, и ничего из
    // провизорного хвоста.
    void finishedStrokeHasNoPredictedPoints()
    {
        WetStroke wet;
        wet.begin(0, prototype());
        const QList<InkPoint> input =
            reference::sampledAtSpeed(reference::straightLine(200, 300.0), 1500.0, 240.0);
        for (const InkPoint &point : input)
            wet.addPoint(point);

        const QList<InkPoint> settled = wet.points();
        QVERIFY2(!wet.provisionalPoints().isEmpty(), "эталон должен давать провизорный хвост");

        const Stroke stroke = wet.finish();
        QVERIFY(stroke.points.size() >= settled.size());
        for (int i = 0; i < settled.size(); ++i)
            QCOMPARE(stroke.points[i].position, settled[i].position);
        QCOMPARE(stroke.points.constLast().position, input.constLast().position);
        QVERIFY(wet.provisionalPoints().isEmpty());
    }

    // ink-beauty-pass, D3: мокрый штрих рисуется законом толщины своего
    // прототипа, и завершённый штрих уносит этот закон с собой.
    void wetStrokeUsesPrototypeStyle()
    {
        Stroke proto = prototype(4.0);
        proto.style = StrokeStyle::Tapered;
        WetStroke wet;
        wet.begin(0, proto);
        // Прямая с нулевым нажимом: каждая пара вершин полосы - поперечник
        // штриха, и он обязан быть 0.08 толщины пера, а не 0.25.
        for (int i = 0; i < 40; ++i)
            wet.addPoint({QPointF(i * 3.0, 20.0), 0.0, i / 200.0});
        const Stroke stroke = wet.finish();
        QCOMPARE(stroke.style, StrokeStyle::Tapered);

        const QVector<WetStroke::Vertex> &strip = wet.strip();
        QVERIFY(strip.size() >= 2 * kCapVertices);
        // Пары от боковой пары начального торца до боковой пары конечного;
        // внутри торцов пары - хорды полудиска, а не поперечник (D4).
        for (int i = kCapVertices - 2; i <= int(strip.size()) - kCapVertices; i += 2) {
            const qreal width = std::hypot(strip[i].x - strip[i + 1].x, strip[i].y - strip[i + 1].y);
            QVERIFY2(qAbs(width - 4.0 * 0.08) < 1e-3,
                     qPrintable(QStringLiteral("толщина %1 в паре %2").arg(width).arg(i / 2)));
        }
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
