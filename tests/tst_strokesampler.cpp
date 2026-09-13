// Задача 5.2: фильтр One Euro и ресемплинг по дуге. Проверяется ровно то, что
// требует задача: дрожание сглажено, а форма буквы не искажена.

#include <QTest>

#include "ink/strokesampler.h"
#include "support/referencestrokes.h"

class TestStrokeSampler : public QObject
{
    Q_OBJECT

private:
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
