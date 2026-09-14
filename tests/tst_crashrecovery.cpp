// Задача 7.4 и сценарий «Аварийное завершение» из note-storage: процесс
// убивается посреди письма, а заметка после этого открывается и содержит
// работу не старше интервала автосохранения.

#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include "fixtures.h"
#include "ink/inkdocument.h"
#include "storage/notestore.h"

class TestCrashRecovery : public QObject
{
    Q_OBJECT

private:
    // Запускает писателя, дожидается хотя бы одного сохранения и убивает его
    // посреди письма. В lastSavedCount - сколько штрихов обязано пережить аварию.
    static void killWriterMidWriting(const QString &note, const QString &pdf,
                                     const QStringList &extraArguments, int *lastSavedCount)
    {
        // Автосохранение раз в секунду: спека разрешает терять не больше
        // тридцати секунд работы, здесь потеря заведомо меньше.
        QProcess writer;
        writer.start(QStringLiteral(UQ_CRASH_WRITER),
                     QStringList{note, pdf, QStringLiteral("1000")} + extraArguments);
        QVERIFY2(writer.waitForStarted(5000), qPrintable(writer.errorString()));

        *lastSavedCount = 0;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 6000) {
            if (writer.waitForReadyRead(500)) {
                const QByteArray chunk = writer.readAllStandardOutput();
                for (const QByteArray &line : chunk.split('\n')) {
                    if (line.startsWith("saved "))
                        *lastSavedCount = line.mid(6).trimmed().toInt();
                }
            }
            if (*lastSavedCount > 0 && timer.elapsed() > 3000)
                break;
        }
        QVERIFY2(*lastSavedCount > 0, "процесс не успел ни разу сохраниться");

        // Аварийное завершение: SIGKILL посреди письма, без шанса закрыть базу.
        writer.kill();
        QVERIFY(writer.waitForFinished(5000));
        QCOMPARE(writer.exitStatus(), QProcess::CrashExit);
    }

private slots:
    void noteSurvivesKillDuringWriting()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString pdf = dir.filePath(QStringLiteral("source.pdf"));
        QVERIFY(QFile::copy(fixtures::textbook(), pdf));
        const QString note = NoteStore::notePathFor(pdf);

        int lastSavedCount = 0;
        killWriterMidWriting(note, pdf, {}, &lastSavedCount);
        if (QTest::currentTestFailed())
            return;

        // Файл заметки читается, а не оказывается битым.
        InkDocument ink;
        NoteStore store;
        QVERIFY2(store.openNote(note), qPrintable(store.errorString()));
        QVERIFY2(store.load(&ink), qPrintable(store.errorString()));

        QVERIFY2(ink.strokeCount() >= lastSavedCount,
                 qPrintable(QStringLiteral("сохранено %1, восстановлено %2")
                                .arg(lastSavedCount)
                                .arg(ink.strokeCount())));
        // И восстановленные штрихи целы, а не обрезаны на половине записи.
        for (const InkDocument::Record &record : ink.allStrokes()) {
            QVERIFY(record.stroke.isValid());
            QCOMPARE(record.stroke.points.size(), 25);
        }

        QCOMPARE(store.sourceState(), NoteStore::SourceState::Ok);
        // Исходный PDF аварией не задет.
        QCOMPARE(QFileInfo(pdf).size(), QFileInfo(fixtures::textbook()).size());
    }

    // Задача 2.5 add-ink-inserts, сценарий «Аварийное завершение во время
    // письма во вкладыше».
    void insertSurvivesKillDuringWriting()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString pdf = dir.filePath(QStringLiteral("source.pdf"));
        QVERIFY(QFile::copy(fixtures::textbook(), pdf));
        const QString note = NoteStore::notePathFor(pdf);

        int lastSavedCount = 0;
        killWriterMidWriting(note, pdf, {QStringLiteral("inserts")}, &lastSavedCount);
        if (QTest::currentTestFailed())
            return;

        InkDocument ink;
        NoteStore store;
        QVERIFY2(store.openNote(note), qPrintable(store.errorString()));
        QVERIFY2(store.load(&ink), qPrintable(store.errorString()));

        QCOMPARE(ink.insertCount(), 1);
        const InkInsert insert = ink.allInserts().first();
        QCOMPARE(insert.page, 2);
        QCOMPARE(insert.rect, QRectF(60, 80, 400, 500));
        QVERIFY(insert.open);

        // Все штрихи - во вкладыше, на страницах ничего.
        QVERIFY(ink.allStrokes().isEmpty());
        const QList<InkDocument::Record> strokes = ink.strokesInInsert(insert.id);
        QVERIFY2(strokes.size() >= lastSavedCount,
                 qPrintable(QStringLiteral("сохранено %1, восстановлено %2")
                                .arg(lastSavedCount)
                                .arg(strokes.size())));
        for (const InkDocument::Record &record : strokes) {
            QCOMPARE(record.stroke.insert, insert.id);
            QCOMPARE(record.stroke.points.size(), 25);
        }
    }
};

QTEST_GUILESS_MAIN(TestCrashRecovery)
#include "tst_crashrecovery.moc"
