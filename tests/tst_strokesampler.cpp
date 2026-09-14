// Задача 5.2: фильтр One Euro и ресемплинг по дуге. Проверяется ровно то, что
// требует задача: дрожание сглажено, а форма буквы не искажена.

#include <QTest>

#include "ink/strokesampler.h"
#include "support/referencestrokes.h"

class TestStrokeSampler : public QObject
{
    Q_OBJECT

private:
    // Порог граней. Кривая радиусом 30 px, нарезанная с шагом 1.2 px, даёт
    // поворот около 2.3° на шаг; линейная нарезка между отсчётами, отстоящими
    // на 12.5 px, - около 24° в каждом отсчёте. Порог между ними с запасом в обе
    // стороны.
    static constexpr qreal kMaxFacetDegrees = 6.0;
    // Первые и последние точки пути в метрике граней не участвуют: там штрих
    // законно начинается с места и кончается в точке отрыва.
    static constexpr int kEndSkipPoints = 10;
    // Допуск на угол: половина толщины линии в 3 px.
    static constexpr qreal kCornerTolerance = 1.5;
    // Частота пера целевого устройства по замеру пробника
    // (rnote-grade-stroke-fidelity, задача 1.2).
    static constexpr qreal kDeviceHz = 481.0;

    static QList<QPointF> run(StrokeSampler &sampler, const QList<InkPoint> &input)
    {
        for (const InkPoint &point : input)
            sampler.append(point);
        sampler.finish();
        QList<QPointF> path;
        for (const InkPoint &point : sampler.points())
            path.append(point.position);
        return path;
    }

private slots:
    // Медленное ведение - там, где дрожание руки и заметно.
    void smoothsJitterOnSlowStroke()
    {
        const QList<QPointF> clean = reference::straightLine(200, 60.0);
        const QList<QPointF> noisy = reference::withJitter(clean, 1.2, 42);

        const qreal rawJitter = reference::rmsDeviation(noisy, clean);
        QVERIFY2(rawJitter > 0.4, "эталон шума слишком слабый, тест ничего не проверит");

        StrokeSampler sampler;
        const QList<QPointF> filtered = run(sampler, reference::fromPositions(noisy));
        const qreal filteredJitter = reference::rmsDeviation(filtered, clean);

        // Порог намеренно не жёстче: сглаживание, которое давило дрожание
        // сильнее, срезало и размах настоящего движения - на нём и ломался
        // почерк. Требование «Точность передачи движения» прямо это запрещает,
        // поэтому здесь достаточно уверенного подавления, а не максимального.
        QVERIFY2(filteredJitter < rawJitter * 0.45,
                 qPrintable(QStringLiteral("raw rms %1 -> filtered rms %2")
                                .arg(rawJitter)
                                .arg(filteredJitter)));
    }

    // Быстрое ведение фильтр почти не трогает: иначе на разгоне копилась бы
    // задержка, а её бюджет из ink-input всего 30 мс на всё.
    void doesNotLagOnFastStroke()
    {
        const QList<QPointF> clean = reference::straightLine(200, 900.0);

        StrokeSampler sampler;
        const QList<QPointF> filtered = run(sampler, reference::fromPositions(clean));

        QVERIFY2(reference::maxDeviation(filtered, clean) < 0.5,
                 "фильтр отстаёт от быстрого движения");
    }

    // Форма буквы остаётся своей: сглаженная траектория не отходит от чистой
    // дальше чем на долю толщины штриха.
    void preservesLetterShape()
    {
        const QList<QPointF> clean = reference::letterE();
        const QList<QPointF> noisy = reference::withJitter(clean, 0.7, 7);

        StrokeSampler sampler;
        const QList<QPointF> filtered = run(sampler, reference::fromPositions(noisy));

        const qreal deviation = reference::maxDeviation(filtered, clean);
        QVERIFY2(deviation < 1.5, qPrintable(QStringLiteral("deviation %1").arg(deviation)));

        // И длина пути не съедена сглаживанием.
        const auto length = [](const QList<QPointF> &path) {
            qreal total = 0.0;
            for (int i = 0; i + 1 < path.size(); ++i)
                total += std::hypot(path[i + 1].x() - path[i].x(), path[i + 1].y() - path[i].y());
            return total;
        };
        const qreal ratio = length(filtered) / length(clean);
        QVERIFY2(ratio > 0.9 && ratio < 1.1,
                 qPrintable(QStringLiteral("length ratio %1").arg(ratio)));
    }

    void resamplesToUniformSpacing()
    {
        StrokeSampler sampler;
        sampler.setSpacing(2.0);
        // Скорость по траектории меняется вдвое, шаг остаться должен прежним.
        QList<QPointF> positions;
        for (int i = 0; i < 60; ++i)
            positions.append(QPointF(i * 1.0, 0.0));
        for (int i = 0; i < 60; ++i)
            positions.append(QPointF(60.0 + i * 5.0, 0.0));

        run(sampler, reference::fromPositions(positions));

        const QList<InkPoint> &points = sampler.points();
        QVERIFY(points.size() > 100);
        for (int i = 1; i + 1 < points.size(); ++i) {
            const qreal step = std::hypot(points[i].position.x() - points[i - 1].position.x(),
                                          points[i].position.y() - points[i - 1].position.y());
            QVERIFY2(qAbs(step - 2.0) < 1e-6,
                     qPrintable(QStringLiteral("step %1 at %2").arg(step).arg(i)));
        }
    }

    void interpolatesPressureAlongResampledPoints()
    {
        StrokeSampler sampler;
        sampler.setSpacing(1.0);

        QList<InkPoint> input;
        for (int i = 0; i < 40; ++i)
            input.append({QPointF(i * 4.0, 0.0), qreal(i) / 39.0, i / 200.0});

        run(sampler, input);

        const QList<InkPoint> &points = sampler.points();
        QVERIFY(points.size() > 40);
        for (const InkPoint &point : points) {
            QVERIFY(point.hasPressure());
            QVERIFY(point.pressure >= -0.01 && point.pressure <= 1.01);
        }
        // Давление растёт вдоль штриха.
        QVERIFY(points.constLast().pressure > points.constFirst().pressure + 0.5);
    }

    void keepsMissingPressureAsMissing()
    {
        StrokeSampler sampler;
        QList<InkPoint> input;
        for (int i = 0; i < 30; ++i)
            input.append({QPointF(i * 3.0, 0.0), -1.0, i / 200.0});

        run(sampler, input);
        for (const InkPoint &point : sampler.points())
            QVERIFY(!point.hasPressure());
    }

    // Сценарий «Отрыв пера»: штрих кончается ровно в последней точке контакта.
    void finishEndsExactlyAtLastContactPoint()
    {
        StrokeSampler sampler;
        sampler.setSpacing(5.0);

        QList<InkPoint> input;
        for (int i = 0; i < 10; ++i)
            input.append({QPointF(i * 4.0, 0.0), 0.5, i / 200.0});
        const QPointF lastContact = input.constLast().position;

        for (const InkPoint &point : input)
            sampler.append(point);
        // До finish хвост короче шага ещё не выдан.
        QVERIFY(sampler.points().constLast().position != lastContact);

        sampler.finish();
        const QPointF end = sampler.points().constLast().position;
        QVERIFY2(std::hypot(end.x() - lastContact.x(), end.y() - lastContact.y()) < 1e-6,
                 "штрих не дотянут до точки отрыва");

        // И дальше он не растёт.
        QCOMPARE(sampler.append({QPointF(999, 999), 0.9, 1.0}).size(), 0);
        QCOMPARE(sampler.finish().size(), 0);
    }

    void appendReturnsOnlyNewPoints()
    {
        StrokeSampler sampler;
        sampler.setSpacing(2.0);

        int total = 0;
        for (int i = 0; i < 50; ++i)
            total += sampler.append({QPointF(i * 6.0, 0.0), 0.5, i / 200.0}).size();
        total += sampler.finish().size();

        // Ровно те точки, что лежат в траектории: ничего не выдано дважды.
        QCOMPARE(total, sampler.points().size());
    }

    // Задача 10.2 и требование «Точность передачи движения».
    // Сглаживание не имеет права срезать углы быстрого знакопеременного
    // движения: размах должен дойти до линии почти целиком.
    void fastZigzagKeepsItsAmplitude()
    {
        const auto wave = reference::cardiogram();

        StrokeSampler sampler;
        const QList<QPointF> filtered =
            run(sampler, reference::atScale(wave.screenPath, 1.0, wave.hz));

        const qreal expected = reference::peakToPeakY(wave.screenPath);
        const qreal got = reference::peakToPeakY(filtered);
        QVERIFY2(got > expected * 0.85,
                 qPrintable(QStringLiteral("размах срезан: %1 из %2").arg(got).arg(expected)));
    }

    // Тот же жест на увеличенном масштабе должен дать ту же линию: и размах, и
    // плотность точек на экране. Именно здесь ломался почерк на 250%.
    void behaviourIsTheSameAtAnyScale_data()
    {
        QTest::addColumn<qreal>("scale");
        QTest::newRow("100%") << 1.0;
        QTest::newRow("250%") << 2.5;
        QTest::newRow("400%") << 4.0;
        QTest::newRow("50%") << 0.5;
    }

    void behaviourIsTheSameAtAnyScale()
    {
        QFETCH(qreal, scale);
        const auto wave = reference::cardiogram();

        StrokeSampler sampler;
        sampler.setScale(scale);
        const QList<QPointF> pagePath =
            run(sampler, reference::atScale(wave.screenPath, scale, wave.hz));

        // Возвращаем в экранные единицы - там и сравниваем.
        QList<QPointF> onScreen;
        for (const QPointF &p : pagePath)
            onScreen.append(p * scale);

        const qreal expected = reference::peakToPeakY(wave.screenPath);
        const qreal got = reference::peakToPeakY(onScreen);
        QVERIFY2(got > expected * 0.85,
                 qPrintable(QStringLiteral("масштаб %1: размах %2 из %3")
                                .arg(scale).arg(got).arg(expected)));

        // Плотность точек на экране не зависит от масштаба.
        for (int i = 1; i + 1 < onScreen.size(); ++i) {
            const qreal step = std::hypot(onScreen[i].x() - onScreen[i - 1].x(),
                                          onScreen[i].y() - onScreen[i - 1].y());
            QVERIFY2(qAbs(step - sampler.screenSpacing()) < 1e-6,
                     qPrintable(QStringLiteral("масштаб %1: шаг на экране %2, ждали %3")
                                    .arg(scale).arg(step).arg(sampler.screenSpacing())));
        }

        // И форма совпадает с той, что получается на 100%.
        StrokeSampler reference100;
        const QList<QPointF> baseline =
            run(reference100, reference::atScale(wave.screenPath, 1.0, wave.hz));
        const qreal drift = reference::maxDeviation(onScreen, baseline);
        QVERIFY2(drift < 1.5,
                 qPrintable(QStringLiteral("масштаб %1: форма разошлась на %2")
                                .arg(scale).arg(drift)));
    }

    // Мелкий почерк на увеличенном масштабе: буквы не должны заплывать.
    void smallHandwritingSurvivesHighZoom()
    {
        const QList<QPointF> letter = reference::letterE();

        StrokeSampler sampler;
        sampler.setScale(2.5);
        const QList<QPointF> pagePath = run(sampler, reference::atScale(letter, 2.5));

        QList<QPointF> onScreen;
        for (const QPointF &p : pagePath)
            onScreen.append(p * 2.5);

        QVERIFY2(reference::maxDeviation(onScreen, letter) < 1.5,
                 "форма буквы на большом масштабе искажена");
    }

    // --- rnote-grade-stroke-fidelity (design.md, D6) -------------------------

    void fastCircleHasNoFacets_data()
    {
        QTest::addColumn<qreal>("hz");
        QTest::addColumn<qreal>("ramp");
        // 120 Гц - редкие отсчёты: между ними по 12 с лишним пикселей, и граням
        // есть откуда взяться.
        QTest::newRow("120 Гц") << 120.0 << 0.0;
        // 481 Гц - частота пера целевого устройства по замеру пробника
        // (rnote-grade-stroke-fidelity, задача 1.2). Перо разгоняется из покоя
        // за 15 мс, как рука: мгновенный старт сразу на 1500 px/с даёт на этой
        // частоте разгонный излом 6.6° в первых 24 px, а при любом разгоне от 15
        // до 60 мс поворот не больше 4°.
        QTest::newRow("481 Гц, разгон 15 мс") << 481.0 << 0.015;
    }

    // Сценарий «Быстрый росчерк»: окружность на 1500 px/с, а линия всё равно без
    // граней и остаётся той окружностью, которую вели.
    void fastCircleHasNoFacets()
    {
        QFETCH(qreal, hz);
        QFETCH(qreal, ramp);
        const qreal radius = 30.0;
        const QPointF center(150.0, 150.0);
        const QList<InkPoint> input = reference::sampledAtSpeed(
            reference::circle(radius, 2, center), 1500.0, hz, 0.0, 0.6, ramp);
        if (hz < 200.0) {
            QVERIFY2(std::hypot(input[1].position.x() - input[0].position.x(),
                                input[1].position.y() - input[0].position.y())
                         > 10.0,
                     "эталон должен давать редкие отсчёты, иначе граням неоткуда взяться");
        }

        StrokeSampler sampler;
        const QList<QPointF> path = run(sampler, input);

        const qreal facet = reference::maxTurnDegrees(path, kEndSkipPoints);
        QVERIFY2(facet < kMaxFacetDegrees,
                 qPrintable(QStringLiteral("грань %1°").arg(facet)));

        qreal worst = 0.0;
        for (int i = kEndSkipPoints; i < path.size() - kEndSkipPoints; ++i) {
            const QPointF d = path[i] - center;
            worst = qMax(worst, std::abs(std::hypot(d.x(), d.y()) - radius));
        }
        QVERIFY2(worst < 2.0, qPrintable(QStringLiteral("радиус ушёл на %1").arg(worst)));
    }

    // Тот же сценарий на букве: крупная «e» быстрым движением.
    void fastLetterHasNoFacets()
    {
        const QList<QPointF> letter = reference::scaled(reference::letterE(), 3.0);
        const QList<InkPoint> input = reference::sampledAtSpeed(letter, 1500.0, 120.0);

        StrokeSampler sampler;
        const QList<QPointF> path = run(sampler, input);

        const qreal facet = reference::maxTurnDegrees(path, kEndSkipPoints);
        QVERIFY2(facet < kMaxFacetDegrees,
                 qPrintable(QStringLiteral("грань %1°").arg(facet)));
        const qreal deviation = reference::maxDeviation(path, letter);
        QVERIFY2(deviation < 2.5,
                 qPrintable(QStringLiteral("форма ушла на %1").arg(deviation)));
    }

    // Сценарий «Острый угол»: вершина остаётся в месте разворота - не
    // срезается внутрь и не выносится наружу.
    void sharpCornerKeepsItsApex_data()
    {
        QTest::addColumn<QList<QPointF>>("corner");
        QTest::newRow("V") << reference::cornerV();
        QTest::newRow("N") << reference::cornerN();
    }

    void sharpCornerKeepsItsApex()
    {
        QFETCH(QList<QPointF>, corner);
        // Рука на развороте притормаживает: 30 мс стоянки при 400 px/с и 240 Гц.
        const QList<InkPoint> input = reference::sampledAtSpeed(corner, 400.0, 240.0, 0.03);

        StrokeSampler sampler;
        const QList<QPointF> path = run(sampler, input);

        for (int i = 1; i + 1 < corner.size(); ++i) {
            const qreal miss = reference::distanceToPath(corner[i], path);
            QVERIFY2(miss <= kCornerTolerance,
                     qPrintable(QStringLiteral("вершина %1 срезана на %2").arg(i).arg(miss)));
        }

        const QList<QPointF> hull = reference::convexHull(corner);
        for (const QPointF &p : path) {
            const qreal outside = reference::distanceOutsideHull(p, hull);
            QVERIFY2(outside <= kCornerTolerance,
                     qPrintable(QStringLiteral("угол вынесен наружу на %1").arg(outside)));
        }
    }

    // Сценарий «Отсчёты чаще кадров»: зубец, который перо проходит быстрее
    // одного кадра, виден на линии.
    void toothShorterThanFrameSurvives()
    {
        const auto tooth = reference::toothShorterThanFrame();

        StrokeSampler sampler;
        const QList<QPointF> path = run(sampler, tooth.points);

        qreal peak = 0.0;
        for (const QPointF &p : path) {
            if (std::abs(p.x() - tooth.apex.x()) < 6.0)
                peak = qMax(peak, 100.0 - p.y());
        }
        QVERIFY2(peak >= tooth.amplitude * 0.5,
                 qPrintable(QStringLiteral("зубец %1 из %2").arg(peak).arg(tooth.amplitude)));
    }

    // Мелкие петли. При сравнении с rnote на живом письме (задача 6.3) ряд «о» в
    // 8-20 px сжимался в «а» и точки - крупные эталоны этого не ловили. Петля в
    // один оборот с разгона из покоя, на частоте пера устройства. На прежних
    // параметрах средний радиус падал до 61%, самый узкий участок - до 47%.
    void smallLoopsKeepTheirSize_data()
    {
        QTest::addColumn<qreal>("radius");
        QTest::addColumn<qreal>("period");
        for (qreal radius : {4.0, 6.0, 10.0}) {
            for (qreal period : {0.08, 0.15, 0.25}) {
                QTest::newRow(qPrintable(
                    QStringLiteral("r=%1 px, оборот %2 с").arg(radius).arg(period)))
                    << radius << period;
            }
        }
    }

    void smallLoopsKeepTheirSize()
    {
        QFETCH(qreal, radius);
        QFETCH(qreal, period);
        const QPointF center(100.0, 100.0);
        const qreal speed = 2.0 * M_PI * radius / period;

        StrokeSampler sampler;
        const QList<QPointF> path = run(
            sampler, reference::sampledAtSpeed(reference::circle(radius, 1, center, 360), speed,
                                               kDeviceHz, 0.0, 0.6, 0.015));

        // Середина оборота: начало и конец штриха законно не на окружности.
        qreal sum = 0.0;
        qreal narrowest = radius;
        int count = 0;
        for (int i = int(path.size() * 0.15); i < int(path.size() * 0.85); ++i) {
            const qreal r = std::hypot(path[i].x() - center.x(), path[i].y() - center.y());
            sum += r;
            narrowest = qMin(narrowest, r);
            ++count;
        }
        QVERIFY(count > 0);
        QVERIFY2(sum / count >= radius * 0.85,
                 qPrintable(QStringLiteral("средний радиус %1 из %2").arg(sum / count).arg(radius)));
        QVERIFY2(narrowest >= radius * 0.7,
                 qPrintable(QStringLiteral("петля сжата до %1 из %2").arg(narrowest).arg(radius)));
    }

    // Мелкая «M» высотой 20 px: на живом письме её вершины скруглялись. На 150
    // px/с вершина срезается на 1.8 px и здесь, и с параметрами rnote, поэтому в
    // эталоне скорости письма бегло, где на прежних параметрах срез был 1.84 px.
    void smallLetterMKeepsItsApexes_data()
    {
        QTest::addColumn<qreal>("speed");
        QTest::newRow("300 px/с") << 300.0;
        QTest::newRow("600 px/с") << 600.0;
    }

    void smallLetterMKeepsItsApexes()
    {
        QFETCH(qreal, speed);
        const QList<QPointF> letter = {QPointF(40, 120), QPointF(46, 100), QPointF(52, 120),
                                       QPointF(58, 100), QPointF(64, 120)};

        StrokeSampler sampler;
        const QList<QPointF> path =
            run(sampler, reference::sampledAtSpeed(letter, speed, kDeviceHz, 0.0, 0.6, 0.015));

        for (int i = 1; i + 1 < letter.size(); ++i) {
            const qreal miss = reference::distanceToPath(letter[i], path);
            QVERIFY2(miss <= kCornerTolerance,
                     qPrintable(QStringLiteral("вершина %1 срезана на %2").arg(i).arg(miss)));
        }
    }

    // Задача 5.1, сценарий «Кончик линии у пера»: после каждого отсчёта
    // провизорный хвост дотянут до последнего положения пера, а окончательные
    // точки задним числом не меняются.
    void provisionalTailReachesThePen_data()
    {
        QTest::addColumn<int>("shape");
        QTest::newRow("длинная дуга") << 0;
        QTest::newRow("быстрая прямая") << 1;
    }

    void provisionalTailReachesThePen()
    {
        QFETCH(int, shape);
        const QList<QPointF> path = shape == 0
            ? reference::circle(80.0, 1, QPointF(200, 200))
            : reference::straightLine(200, 400.0);
        const QList<InkPoint> input = reference::sampledAtSpeed(path, 1200.0, 240.0);

        StrokeSampler sampler;
        QList<InkPoint> settled;
        for (int i = 0; i < input.size(); ++i) {
            sampler.append(input[i]);

            QVERIFY(sampler.points().size() >= settled.size());
            for (int k = 0; k < settled.size(); ++k) {
                QVERIFY2(sampler.points()[k].position == settled[k].position,
                         qPrintable(QStringLiteral("точка %1 переписана на отсчёте %2")
                                        .arg(k)
                                        .arg(i)));
            }
            settled = sampler.points();

            const QPointF tip = sampler.provisionalPoints().isEmpty()
                ? sampler.points().constLast().position
                : sampler.provisionalPoints().constLast().position;
            const qreal lag = std::hypot(tip.x() - input[i].position.x(),
                                         tip.y() - input[i].position.y());
            QVERIFY2(lag <= 2.0,
                     qPrintable(QStringLiteral("кончик отстал на %1 на отсчёте %2")
                                    .arg(lag)
                                    .arg(i)));
        }

        sampler.finish();
        QVERIFY(sampler.provisionalPoints().isEmpty());
    }

    void resetClearsEverything()
    {
        StrokeSampler sampler;
        run(sampler, reference::fromPositions(reference::straightLine()));
        QVERIFY(!sampler.isEmpty());
        sampler.reset();
        QVERIFY(sampler.isEmpty());
        QCOMPARE(sampler.append({QPointF(1, 1), 0.5, 0.0}).size(), 1);
    }
};

QTEST_APPLESS_MAIN(TestStrokeSampler)
#include "tst_strokesampler.moc"
