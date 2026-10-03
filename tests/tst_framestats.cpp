// ink-beauty-pass, шаг 0 (design.md, D1): честная латентность. Метка события
// переводится в шкалу FrameStats::nowNs(), задержка до показа сводится по
// кадрам. Протокол wp_presentation здесь не участвует - его проверяет ручной
// шаг на устройстве (PERFORMANCE.md); тут только арифметика, без дисплея.

#include <QTest>

#include <cstdint>

#include "perf/framestats.h"
#include "perf/presentationstats.h"

namespace {

constexpr qint64 kMs = 1'000'000;

// Метка события так, как её отдаёт QInputEvent::timestamp() на Wayland:
// миллисекунды CLOCK_MONOTONIC, усечённые до 32 бит, в секундах.
qreal waylandStampSeconds(qint64 monotonicMs)
{
    return qreal(uint32_t(monotonicMs)) / 1000.0;
}

} // namespace

class TestFrameStats : public QObject
{
    Q_OBJECT

private slots:
    void eventStampMapsToMonotonicClock()
    {
        const qint64 now = 5'000'123 * kMs + 456'789; // 5000 с аптайма
        const qint64 mapped =
            FrameStats::eventTimeToNs(waylandStampSeconds(5'000'123 - 7), now);
        QCOMPARE(mapped, (5'000'123 - 7) * kMs);
    }

    void eventStampSurvivesWrapAround()
    {
        // Младшие 32 бита часов уже перевалили через ноль, а метка - ещё нет.
        const qint64 nowMs = (qint64(1) << 32) + 10;
        const qint64 mapped =
            FrameStats::eventTimeToNs(waylandStampSeconds(nowMs - 15), nowMs * kMs);
        QCOMPARE(mapped, (nowMs - 15) * kMs);
    }

    void foreignStampsAreRejected_data()
    {
        QTest::addColumn<qint64>("ageMs");
        QTest::newRow("из будущего") << qint64(-50);
        QTest::newRow("старше 5 с") << qint64(60'000);
        QTest::newRow("часы QTest с нуля") << qint64(5'000'123 - 1'000);
    }

    void foreignStampsAreRejected()
    {
        QFETCH(qint64, ageMs);
        const qint64 nowMs = 5'000'123;
        QCOMPARE(FrameStats::eventTimeToNs(waylandStampSeconds(nowMs - ageMs), nowMs * kMs),
                 qint64(-1));
    }

    void missingStampIsNotForeign()
    {
        QCOMPARE(FrameStats::eventTimeToNs(-1.0, 5'000'123 * kMs), qint64(-1));

        FrameStats stats;
        stats.markInput();
        stats.markInput(-1.0);
        QCOMPARE(stats.foreignInputStamps(), 0);
    }

    void markInputCountsForeignStamps()
    {
        FrameStats stats;
        const qint64 nowMs = FrameStats::nowNs() / kMs;
        stats.markInput(waylandStampSeconds(nowMs - 3));
        QCOMPARE(stats.foreignInputStamps(), 0);
        stats.markInput(waylandStampSeconds(nowMs - 60'000));
        QCOMPARE(stats.foreignInputStamps(), 1);
        QVERIFY(stats.summary().contains(QStringLiteral("foreign stamps 1")));
    }

    void frameKeepsOldestAndNewestInput()
    {
        FrameInputs inputs;
        QVERIFY(inputs.isEmpty());
        inputs.add(0); // «метки нет» не считается событием
        QVERIFY(inputs.isEmpty());
        inputs.add(30 * kMs);
        inputs.add(10 * kMs);
        inputs.add(20 * kMs);
        QCOMPARE(inputs.oldestNs, 10 * kMs);
        QCOMPARE(inputs.newestNs, 30 * kMs);
    }

    void summaryGivesMedianAndP95()
    {
        PresentationStats stats;
        // 20 кадров: самое раннее событие кадра на 10..29 мс раньше показа,
        // самое позднее - на 2 мс раньше.
        for (int i = 0; i < 20; ++i) {
            FrameInputs inputs;
            const qint64 presented = 1000 * kMs + i * 8 * kMs;
            inputs.add(presented - (10 + i) * kMs);
            inputs.add(presented - 2 * kMs);
            stats.addPresented(inputs, presented);
        }
        stats.addDiscarded();

        const PresentationSummary summary = stats.takeSummary();
        QCOMPARE(summary.presented, 20);
        QCOMPARE(summary.discarded, 1);
        QCOMPARE(summary.rejected, 0);
        QCOMPARE(summary.oldestMedianMs, 20.0); // отсортированная выборка, элемент 10
        QCOMPARE(summary.oldestP95Ms, 29.0);    // элемент 19
        QCOMPARE(summary.newestMedianMs, 2.0);
        QCOMPARE(summary.maxMs, 29.0);

        // Сводка за секунду: накопленное сброшено.
        QVERIFY(stats.takeSummary().isEmpty());
    }

    void presentationBeforeEventIsAClockMismatch()
    {
        PresentationStats stats;
        FrameInputs inputs;
        inputs.add(50 * kMs);
        stats.addPresented(inputs, 40 * kMs);
        stats.addPresented(FrameInputs(), 60 * kMs); // кадр без ввода не считается

        const PresentationSummary summary = stats.takeSummary();
        QCOMPARE(summary.presented, 0);
        QCOMPARE(summary.rejected, 1);
        QVERIFY(summary.toString().contains(QStringLiteral("clock mismatch 1")));
    }

    // Чужая метка не идёт в задержку до показа: запасное «сейчас» занизило бы
    // [present]. Латентность до swap по-прежнему считается от обработки.
    void foreignStampIsNotAPresentationSample()
    {
        const qint64 nowMs = 5'000'123;
        const qint64 now = nowMs * kMs + 456'789;

        const FrameStats::InputMark own =
            FrameStats::classifyInput(waylandStampSeconds(nowMs - 7), now);
        QVERIFY(!own.foreign);
        QCOMPARE(own.latencyFromNs, (nowMs - 7) * kMs);
        QCOMPARE(own.presentationNs, (nowMs - 7) * kMs);

        const FrameStats::InputMark foreign =
            FrameStats::classifyInput(waylandStampSeconds(nowMs - 60'000), now);
        QVERIFY(foreign.foreign);
        QCOMPARE(foreign.latencyFromNs, now);
        QCOMPARE(foreign.presentationNs, qint64(0));

        // Без метки (сценарный ввод) - от обработки, и это не чужая метка.
        const FrameStats::InputMark none = FrameStats::classifyInput(-1.0, now);
        QVERIFY(!none.foreign);
        QCOMPARE(none.latencyFromNs, now);
        QCOMPARE(none.presentationNs, now);
    }

    // Огромная метка (сбой драйвера, чужие часы) - чужая, без переполнения
    // в llround.
    void hugeStampIsForeign()
    {
        const qint64 now = 5'000'123 * kMs;
        QCOMPARE(FrameStats::eventTimeToNs(1e300, now), qint64(-1));
        QCOMPARE(FrameStats::eventTimeToNs(qreal(1ULL << 63), now), qint64(-1));
        QCOMPARE(FrameStats::eventTimeToNs(qQNaN(), now), qint64(-1));

        FrameStats stats;
        stats.markInput(1e300);
        QCOMPARE(stats.foreignInputStamps(), 1);
    }

    // Кадр забирает события, отмеченные до синхронизации; отмеченное позже -
    // следующему кадру.
    void trackerSplitsInputsAtSync()
    {
        FrameInputTracker tracker;
        tracker.noteInput(10 * kMs);
        tracker.noteInput(12 * kMs);
        tracker.beginFrame();
        tracker.noteInput(15 * kMs); // после синхронизации
        const FrameInputs first = tracker.takeForRenderedFrame();
        QCOMPARE(first.oldestNs, 10 * kMs);
        QCOMPARE(first.newestNs, 12 * kMs);
        QVERIFY(tracker.takeForRenderedFrame().isEmpty()); // кадр забирает один раз
        tracker.endFrame();

        tracker.beginFrame();
        const FrameInputs second = tracker.takeForRenderedFrame();
        QCOMPARE(second.oldestNs, 15 * kMs);
        QCOMPARE(second.newestNs, 15 * kMs);
    }

    // Кадр закончился без отрисовки (beginFrame не удался, окно не готово) -
    // его события не приписываются следующему кадру, который отрисуется без
    // синхронизации.
    void frameWithoutRenderDropsItsInputs()
    {
        FrameInputTracker tracker;
        tracker.noteInput(10 * kMs);
        tracker.beginFrame();
        tracker.endFrame();
        QVERIFY(tracker.takeForRenderedFrame().isEmpty());
    }

};

QTEST_MAIN(TestFrameStats)
#include "tst_framestats.moc"
