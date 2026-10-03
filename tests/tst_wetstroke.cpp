// Задача 5.4: инкрементальное наращивание «мокрого» штриха. Проверяется, что
// на добавление точки приходится постоянная работа независимо от длины штриха,
// и что итог совпадает с полной тесселяцией.

#include <QElapsedTimer>
#include <QLineF>
#include <QPolygonF>
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

qreal distanceToSegment(const QPointF &p, const QLineF &segment)
{
    const QPointF ab = segment.p2() - segment.p1();
    const qreal lengthSquared = QPointF::dotProduct(ab, ab);
    const qreal t = lengthSquared < 1e-12
        ? 0.0
        : qBound(0.0, QPointF::dotProduct(p - segment.p1(), ab) / lengthSquared, 1.0);
    return QLineF(p, segment.p1() + ab * t).length();
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

    // --- ink-beauty-pass, D5: точка от касания --------------------------------

    // Сценарий «Точка над буквой»: касание без движения оставляет точку, и
    // видна она уже во время касания, а не только после отрыва.
    void tapWithoutMovementGivesDot()
    {
        WetStroke wet;
        wet.begin(0, prototype());
        const QPointF at(10, 10);
        wet.addPoint({at, 0.5, 0.0});

        // Мокрый диск под пером.
        QCOMPARE(wet.strip().size(), 2 * kCapVertices);
        const qreal r = wet.tessellator().halfWidthAt({at, 0.5, 0.0});
        for (const WetStroke::Vertex &vertex : wet.strip())
            QVERIFY(qAbs(QLineF(toPoint(vertex), at).length() - r) < 1e-4);

        wet.addPoint({at, 0.7, 0.04});
        const Stroke stroke = wet.finish();
        QVERIFY(stroke.isValid());
        QCOMPARE(stroke.points.size(), 2);
        QCOMPARE(stroke.points[0].position, at);
        QCOMPARE(stroke.points[1].position, at);
        // Толщина точки - по самому сильному отфильтрованному нажиму касания.
        QVERIFY(stroke.points[1].pressure > 0.5);

        // Мокрая полоса на отрыве - та же, что сухая.
        StrokeTessellator tessellator;
        tessellator.setWidth(stroke.width);
        tessellator.setStyle(stroke.style);
        QVERIFY(sameStrip(wet.strip(), tessellator.tessellate(stroke.points)));
    }

    // Сценарий «Случайный задев»: короче 30 мс и легче 0.05 - следа нет.
    // Достаточно одного из двух - долгого касания или нажима, - и точка
    // остаётся. У мыши давления нет, её касание не отбрасывается.
    void onlyShortLightTouchIsDiscarded_data()
    {
        QTest::addColumn<qreal>("pressure");
        QTest::addColumn<qreal>("duration");
        QTest::addColumn<bool>("kept");
        QTest::newRow("задев") << 0.03 << 0.01 << false;
        QTest::newRow("короткое с нажимом") << 0.5 << 0.01 << true;
        QTest::newRow("долгое лёгкое") << 0.03 << 0.05 << true;
        QTest::newRow("мышь") << -1.0 << 0.01 << true;
    }

    void onlyShortLightTouchIsDiscarded()
    {
        QFETCH(qreal, pressure);
        QFETCH(qreal, duration);
        QFETCH(bool, kept);

        WetStroke wet;
        wet.begin(0, prototype());
        for (qreal t = 0.0; t <= duration + 1e-9; t += 0.005)
            wet.addPoint({QPointF(10, 10), pressure, t});
        const Stroke stroke = wet.finish();
        QCOMPARE(stroke.isValid(), kept);
        if (!kept)
            QVERIFY(wet.strip().isEmpty());
    }

    // Сценарий «Случайный задев» мерит давление касания, а не сглаженное:
    // фильтр давления отстаёт на нарастании, и у быстрого намеренного касания
    // отфильтрованный пик ниже порога 0.05, хотя перо нажало до 0.08. Такая
    // точка остаётся. Давления подобраны под нынешний фильтр: у 0.01/0.08/
    // 0.15/0.10 отфильтрованный пик 0.08 и различия не видно, а у этого
    // касания (16 мс, отсчёт в 4 мс) он около 0.042. Это проверяется первым,
    // иначе тест ничего бы не доказывал.
    void quickFirmTapIsKeptDespiteFilterLag()
    {
        const QPointF at(10, 10);
        const QList<InkPoint> input = {{at, 0.01, 0.000},
                                       {at, 0.02, 0.004},
                                       {at, 0.04, 0.008},
                                       {at, 0.08, 0.012},
                                       {at, 0.06, 0.016}};
        StrokeSampler reference;
        for (const InkPoint &point : input)
            reference.append(point);
        QVERIFY2(reference.peakPressure() < 0.05,
                 qPrintable(QStringLiteral("отфильтрованный пик %1 - подобрать давления заново")
                                .arg(reference.peakPressure())));
        QCOMPARE(reference.peakRawPressure(), 0.08);
        QVERIFY(reference.contactDuration() < 0.030);

        WetStroke wet;
        wet.begin(0, prototype());
        for (const InkPoint &point : input)
            wet.addPoint(point);
        const Stroke stroke = wet.finish();
        QVERIFY(stroke.isValid());
        // Толщину точке по-прежнему даёт отфильтрованный пик (D5).
        QCOMPARE(stroke.points.constLast().pressure, reference.peakPressure());
    }

    // Сценарий «Касание с едва заметным сдвигом»: перо сдвинулось на 0.5 при
    // толщине 3 - капсула одной толщины, а не чёрточка с тонким заходом.
    void tinyMovementGivesCapsule()
    {
        WetStroke wet;
        wet.begin(0, prototype(3.0));
        const qreal pressures[] = {0.2, 0.5, 0.8, 0.8, 0.7, 0.6, 0.5};
        for (int i = 0; i < 7; ++i)
            wet.addPoint({QPointF(10.0 + 0.5 * i / 6.0, 10.0), pressures[i], i / 200.0});
        const Stroke stroke = wet.finish();
        QVERIFY(stroke.isValid());

        // Радиус капсулы - наибольшая полуширина среди точек штриха.
        qreal r = 0.0;
        for (const InkPoint &point : stroke.points)
            r = qMax(r, wet.tessellator().halfWidthAt(point));
        const QLineF axis(stroke.points.constFirst().position, stroke.points.constLast().position);
        for (const WetStroke::Vertex &vertex : wet.strip()) {
            const qreal d = distanceToSegment(toPoint(vertex), axis);
            QVERIFY2(qAbs(d - r) < 1e-4,
                     qPrintable(QStringLiteral("вершина в %1 от оси, радиус капсулы %2").arg(d).arg(r)));
        }

        StrokeTessellator tessellator;
        tessellator.setWidth(stroke.width);
        tessellator.setStyle(stroke.style);
        QVERIFY(sameStrip(wet.strip(), tessellator.tessellate(stroke.points)));
    }

    // Сценарий «Касание с едва заметным сдвигом» при живом нажиме: касание
    // начинается и кончается почти без давления, а точка обязана быть толщиной
    // в нажим касания - по его пику, и во время касания, и после отрыва.
    void shiftedTapHasPeakWidth_data()
    {
        QTest::addColumn<qreal>("shift");
        QTest::newRow("0.3") << 0.3;
        QTest::newRow("1.5") << 1.5;
        QTest::newRow("3.0") << 3.0;
    }

    void shiftedTapHasPeakWidth()
    {
        QFETCH(qreal, shift);
        // Толщина 4 - все сдвиги короче её; касание 40 мс на частоте пера.
        const qreal width = 4.0;
        QList<InkPoint> input;
        for (int k = 0; k <= 19; ++k) {
            const qreal u = k / 19.0;
            input.append({QPointF(10.0 + shift * u, 10.0), 0.05 + 0.55 * std::sin(M_PI * u),
                          k / 481.0});
        }
        StrokeSampler reference;
        for (const InkPoint &point : input)
            reference.append(point);
        const qreal peak = reference.peakPressure();

        WetStroke wet;
        wet.begin(0, prototype(width));
        for (const InkPoint &point : input)
            wet.addPoint(point);
        const qreal r = wet.tessellator().halfWidthAt({QPointF(), peak, 0.0});
        // Во время касания: мокрая капсула уже толщиной по пику. Мокрая полоса -
        // точка, только пока окончательная осевая вместе с провизорным хвостом
        // короче толщины пера; при сдвиге 3.0 предсказание модели могло бы
        // дотянуть хвост до толщины, и тогда во время касания это обычный
        // штрих, а проверка точки - после отрыва, ниже.
        QList<InkPoint> wetAxis = wet.points();
        wetAxis.append(wet.provisionalPoints());
        if (wet.tessellator().isDot(wetAxis)) {
            QPolygonF during;
            for (const WetStroke::Vertex &vertex : wet.strip())
                during.append(toPoint(vertex));
            QVERIFY2(qAbs(during.boundingRect().height() - 2.0 * r) <= 0.05 * 2.0 * r,
                     qPrintable(QStringLiteral("мокрая точка высотой %1, ждали %2")
                                    .arg(during.boundingRect().height()).arg(2.0 * r)));
        } else {
            QVERIFY2(shift >= 3.0,
                     qPrintable(QStringLiteral("при сдвиге %1 мокрая полоса не точка").arg(shift)));
        }

        const Stroke stroke = wet.finish();
        QVERIFY(stroke.isValid());
        // После отрыва: последняя точка - с пиком нажима, капсула - его толщины.
        QCOMPARE(stroke.points.constLast().pressure, peak);
        const QLineF axis(stroke.points.constFirst().position, stroke.points.constLast().position);
        for (const WetStroke::Vertex &vertex : wet.strip()) {
            const qreal d = distanceToSegment(toPoint(vertex), axis);
            QVERIFY2(qAbs(d - r) <= 0.05 * r,
                     qPrintable(QStringLiteral("вершина в %1 от оси, радиус по пику %2").arg(d).arg(r)));
        }
        StrokeTessellator tessellator;
        tessellator.setWidth(stroke.width);
        tessellator.setStyle(stroke.style);
        QVERIFY(sameStrip(wet.strip(), tessellator.tessellate(stroke.points)));
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

    // ink-beauty-pass, D4: пока хорда для начального торца не известна, вся
    // полоса провизорная; как только она известна, начальный торец ложится в
    // окончательную часть и больше не переписывается. На этом переходе держится
    // загрузчик кусков холста: окончательная часть только растёт. С D5 торец
    // окончателен, когда известна хорда и окончательная осевая не короче
    // толщины пера: до того штрих ещё может оказаться точкой.
    void startCapSettlesOnceAndKeepsPrefix()
    {
        const qreal width = 6.0;
        WetStroke wet;
        wet.begin(0, prototype(width));
        const qreal radius = width / 2.0; // полный нажим

        // Медленное начало: перо ползёт по 0.05 на отсчёт, и несколько
        // окончательных точек ложатся ближе радиуса от касания. Дальше - быстро.
        QList<InkPoint> input;
        qreal x = 20.0;
        for (int i = 0; i < 160; ++i) {
            x += i < 60 ? 0.05 : 2.0;
            input.append({QPointF(x, 50.0), 1.0, i / 240.0});
        }

        int transition = -1;
        int settledPointsBefore = 0; // окончательных точек, пока торец провизорный
        int previousSettled = 0;
        QVector<WetStroke::Vertex> prefix;
        for (int i = 0; i < input.size(); ++i) {
            const int added = wet.addPoint(input[i]);
            const int settled = wet.settledVertexCount();
            QVERIFY2(settled >= previousSettled,
                     qPrintable(QStringLiteral("окончательная часть сжалась с %1 до %2 на отсчёте %3")
                                    .arg(previousSettled).arg(settled).arg(i)));
            QCOMPARE(added, settled - previousSettled);

            if (transition < 0) {
                if (added == 0) {
                    QCOMPARE(settled, 0);
                    // Пока торец провизорный, либо ни одна окончательная точка
                    // не отошла на радиус, либо окончательная осевая ещё короче
                    // толщины пера и штрих может оказаться точкой (D5).
                    qreal arc = 0.0;
                    bool withinRadius = true;
                    const QList<InkPoint> &settledPoints = wet.points();
                    for (int k = 0; k < settledPoints.size(); ++k) {
                        if (k > 0)
                            arc += QLineF(settledPoints[k - 1].position,
                                          settledPoints[k].position).length();
                        withinRadius = withinRadius
                            && QLineF(settledPoints[k].position,
                                      settledPoints.constFirst().position).length()
                                < radius;
                    }
                    QVERIFY(withinRadius || arc < width);
                    settledPointsBefore = int(wet.points().size());
                    previousSettled = settled;
                    continue;
                }
                transition = i;
                // Начальный торец лёг целиком.
                QVERIFY(settled >= kCapVertices);
            } else {
                // Префикс, объявленный окончательным раньше, не изменился.
                QVERIFY2(sameStrip(wet.strip().mid(0, prefix.size()), prefix),
                         qPrintable(QStringLiteral("окончательный префикс переписан на отсчёте %1")
                                        .arg(i)));
            }
            prefix = wet.strip().mid(0, settled);
            previousSettled = settled;
        }

        QVERIFY2(transition > 0, "начальный торец так и не стал окончательным");
        // Хотя бы один окончательный сегмент лёг внутри радиуса: прежнее
        // правило «торец окончательный с первым сегментом» здесь бы уже сработало.
        QVERIFY2(settledPointsBefore >= 2,
                 qPrintable(QStringLiteral("до перехода окончательных точек: %1")
                                .arg(settledPointsBefore)));
        QVERIFY(prefix.size() > 4 * kCapVertices);

        // И итог - тот же, что у сухой полосы.
        const Stroke stroke = wet.finish();
        StrokeTessellator tessellator;
        tessellator.setWidth(stroke.width);
        tessellator.setStyle(stroke.style);
        QVERIFY(sameStrip(wet.strip(), tessellator.tessellate(stroke.points)));
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
