// ink-beauty-pass, шаг 0 (design.md, D1): честная латентность. Метка события
// переводится в шкалу FrameStats::nowNs(), задержка до показа сводится по
// кадрам. Протокол wp_presentation здесь не участвует - его проверяет ручной
// шаг на устройстве (PERFORMANCE.md); тут только арифметика, без дисплея.

#include <QTest>

#include <cstdint>

#include "perf/framestats.h"

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

};

QTEST_MAIN(TestFrameStats)
#include "tst_framestats.moc"
