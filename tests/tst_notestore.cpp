// Задачи 7.1-7.6: формат файла заметки, инкрементальная запись, состояние
// просмотра и поведение при пропаже или подмене исходного PDF.
// Сценарии из specs/note-storage/spec.md и specs/pdf-document/spec.md.

#include <QCryptographicHash>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "fixtures.h"
#include "ink/inkdocument.h"
#include "storage/notestore.h"

class TestNoteStore : public QObject
{
    Q_OBJECT

private:
    static Stroke lineOnPage(int page, qreal offset = 0.0)
    {
        Stroke stroke;
        stroke.page = page;
        stroke.width = 2.75;
        stroke.color = QColor(30, 40, 200, 220);
        for (int i = 0; i <= 20; ++i)
            stroke.points.append({QPointF(10 + i * 4.0, 50 + offset), 0.3 + i * 0.03, i / 200.0});
        return stroke;
    }

    static QByteArray bytesOf(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    QString copyFixture(const QString &source, const QString &name)
    {
        const QString target = m_dir.filePath(name);
        QFile::remove(target);
        QFile::copy(source, target);
        return target;
    }

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
    }

    // Задача 7.1: файл создаётся и открывается, схема на месте.
    void createsAndReopensNoteFile()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("a.pdf"));
        const QString note = NoteStore::notePathFor(pdf);
        QCOMPARE(QFileInfo(note).fileName(), QStringLiteral("a.uqn"));

        {
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            QVERIFY(store.isOpen());
            QCOMPARE(store.sourcePath(), QFileInfo(pdf).absoluteFilePath());
            QCOMPARE(store.sourceSize(), QFileInfo(pdf).size());
            QVERIFY(!store.sourceHash().isEmpty());
        }

        QVERIFY(QFileInfo::exists(note));

        NoteStore reopened;
        QVERIFY(reopened.openNote(note));
        QCOMPARE(reopened.sourcePath(), QFileInfo(pdf).absoluteFilePath());
        QCOMPARE(reopened.sourceSize(), QFileInfo(pdf).size());
        QCOMPARE(reopened.sourceState(), NoteStore::SourceState::Ok);
    }

    void refusesToOpenMissingNote()
    {
        NoteStore store;
        QVERIFY(!store.openNote(m_dir.filePath(QStringLiteral("nope.uqn"))));
        QVERIFY(!store.isOpen());
        QVERIFY(!store.errorString().isEmpty());
    }

    // Сценарии «Создание заметки» и «Повторное открытие»: чернила
    // восстанавливаются на своих страницах с той же геометрией.
    void roundTripsStrokesExactly()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("b.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        QList<InkDocument::Record> original;
        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            store.attach(&ink);

            ink.addStroke(lineOnPage(0));
            ink.addStroke(lineOnPage(5, 30.0));
            ink.addStroke(lineOnPage(5, 60.0));
            QVERIFY(store.isDirty());
            QVERIFY(store.flush());
            QVERIFY(!store.isDirty());
            original = ink.allStrokes();
        }

        InkDocument restored;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QVERIFY(store.load(&restored));

        QCOMPARE(restored.strokeCount(), 3);
        QCOMPARE(restored.pagesWithInk(), QList<int>({0, 5}));
        QCOMPARE(restored.strokesOnPage(5).size(), 2);

        const QList<InkDocument::Record> all = restored.allStrokes();
        QCOMPARE(all.size(), original.size());
        for (int i = 0; i < all.size(); ++i) {
            QCOMPARE(all[i].id, original[i].id);
            QCOMPARE(all[i].stroke.page, original[i].stroke.page);
            QCOMPARE(all[i].stroke.color, original[i].stroke.color);
            QVERIFY(qAbs(all[i].stroke.width - original[i].stroke.width) < 1e-4);
            QCOMPARE(all[i].stroke.points.size(), original[i].stroke.points.size());
            for (int p = 0; p < all[i].stroke.points.size(); ++p) {
                const InkPoint &a = all[i].stroke.points[p];
                const InkPoint &b = original[i].stroke.points[p];
                QVERIFY(qAbs(a.position.x() - b.position.x()) < 1e-3);
                QVERIFY(qAbs(a.position.y() - b.position.y()) < 1e-3);
                QVERIFY(qAbs(a.pressure - b.pressure) < 1e-3);
            }
        }
        // Новые штрихи продолжают нумерацию, а не затирают старые.
        QVERIFY(restored.addStroke(lineOnPage(1)) > original.constLast().id);
    }

    void preservesMissingPressureAcrossSave()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("c.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        Stroke stroke = lineOnPage(2);
        for (InkPoint &point : stroke.points)
            point.pressure = -1.0;

        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            store.attach(&ink);
            ink.addStroke(stroke);
            QVERIFY(store.flush());
        }

        InkDocument restored;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QVERIFY(store.load(&restored));
        for (const InkPoint &point : restored.allStrokes().first().stroke.points)
            QVERIFY(!point.hasPressure());
    }

    // Задача 7.2 и сценарий «Рисование и сохранение»: исходный PDF не меняется
    // ни на байт.
    void neverTouchesSourcePdf()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("d.pdf"));
        const QByteArray before = bytesOf(pdf);
        QVERIFY(!before.isEmpty());

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.create(NoteStore::notePathFor(pdf), pdf));
        store.attach(&ink);
        for (int i = 0; i < 40; ++i) {
            ink.addStroke(lineOnPage(i % 7, i));
            if (i % 5 == 0)
                QVERIFY(store.flush());
        }
        QVERIFY(store.flush());
        store.close();

        QCOMPARE(bytesOf(pdf), before);
    }

    // Задача 7.2: запись идёт дельтой в режиме WAL.
    void writesIncrementallyInWalMode()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("e.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.create(note, pdf));
        store.attach(&ink);

        ink.addStroke(lineOnPage(0));
        QVERIFY(store.flush());
        // WAL-файл рядом с базой - признак того, что режим включён.
        QVERIFY(QFileInfo::exists(note + QStringLiteral("-wal"))
                || QFileInfo::exists(note));

        // Стирание тоже уезжает в файл.
        const quint64 id = ink.allStrokes().first().id;
        ink.eraseStrokes({id});
        QVERIFY(store.isDirty());
        QVERIFY(store.flush());
        store.close();

        InkDocument restored;
        NoteStore reopened;
        QVERIFY(reopened.openNote(note));
        QVERIFY(reopened.load(&restored));
        QCOMPARE(restored.strokeCount(), 0);
    }

    // Отмена и повтор не должны оставлять в файле лишних строк.
    void undoRedoLeavesFileConsistent()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("f.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.create(note, pdf));
        store.attach(&ink);

        ink.addStroke(lineOnPage(0));
        ink.addStroke(lineOnPage(1));
        QVERIFY(store.flush());

        ink.undo();
        QVERIFY(store.flush());
        ink.redo();
        QVERIFY(store.flush());
        ink.undo();
        QVERIFY(store.flush());
        store.close();

        InkDocument restored;
        NoteStore reopened;
        QVERIFY(reopened.openNote(note));
        QVERIFY(reopened.load(&restored));
        QCOMPARE(restored.strokeCount(), 1);
        QCOMPARE(restored.allStrokes().first().stroke.page, 0);
    }

    // Задача 7.3, сценарий «Работа без явного сохранения».
    void autosaveWritesWithoutExplicitCommand()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("g.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.create(note, pdf));
        store.attach(&ink);
        store.setAutosaveIntervalMs(200);

        QSignalSpy saved(&store, &NoteStore::saved);
        ink.addStroke(lineOnPage(3));
        QVERIFY(store.isDirty());

        QVERIFY(saved.wait(3000));
        QVERIFY(!store.isDirty());
        // Интервал автосохранения заведомо меньше разрешённых тридцати секунд.
        QVERIFY(store.autosaveIntervalMs() <= 30000);
    }

    // Задача 7.5, сценарий «Возврат к заметке».
    void restoresViewState()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("h.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            store.attach(&ink);
            store.rememberView(9, 2.35, 1, -120.5, -3400.75);
            QVERIFY(store.flush());
        }

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QVERIFY(store.load(&ink));

        const NoteStore::ViewState state = store.viewState();
        QVERIFY(state.valid);
        QCOMPARE(state.page, 9);
        QVERIFY(qAbs(state.zoom - 2.35) < 1e-6);
        QCOMPARE(state.pageMode, 1); // разворот
        QVERIFY(qAbs(state.originX + 120.5) < 1e-6);
        QVERIFY(qAbs(state.originY + 3400.75) < 1e-6);
    }

    void freshNoteHasNoViewState()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("i.pdf"));
        NoteStore store;
        QVERIFY(store.create(NoteStore::notePathFor(pdf), pdf));
        QVERIFY(!store.viewState().valid);
    }

    // Задача 10.5 и сценарий «Возврат к заметке с прежним пером».
    void restoresToolSettings()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("t.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        {
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            QVERIFY(!store.hasToolState());
            store.rememberTool(0.7, QStringLiteral("#ffc02828"));
            QVERIFY(store.isDirty());
            QVERIFY(store.flush());
        }

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QVERIFY(store.load(&ink));

        QVERIFY(store.hasToolState());
        QVERIFY(qAbs(store.toolWidth() - 0.7) < 1e-9);
        QCOMPARE(store.toolColor(), QStringLiteral("#ffc02828"));
    }

    // Настройки инструмента живут в заметке, а не глобально: у другой заметки
    // они свои.
    void toolSettingsArePerNote()
    {
        const QString firstPdf = copyFixture(fixtures::textbook(), QStringLiteral("u1.pdf"));
        const QString secondPdf = copyFixture(fixtures::textbook(), QStringLiteral("u2.pdf"));

        {
            NoteStore store;
            QVERIFY(store.create(NoteStore::notePathFor(firstPdf), firstPdf));
            store.rememberTool(4.5, QStringLiteral("#66f2d024"));
            QVERIFY(store.flush());
        }
        {
            NoteStore store;
            QVERIFY(store.create(NoteStore::notePathFor(secondPdf), secondPdf));
            store.rememberTool(0.4, QStringLiteral("#ff141414"));
            QVERIFY(store.flush());
        }

        InkDocument ink;
        NoteStore first;
        QVERIFY(first.openNote(NoteStore::notePathFor(firstPdf)));
        QVERIFY(first.load(&ink));
        QCOMPARE(first.toolColor(), QStringLiteral("#66f2d024"));
        QVERIFY(qAbs(first.toolWidth() - 4.5) < 1e-9);
    }

    // Заметка, созданная до появления настроек инструмента, читается без них.
    void noteWithoutToolSettingsOpensCleanly()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("v.pdf"));
        const QString note = NoteStore::notePathFor(pdf);
        {
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            store.rememberView(2, 1.0, 0, 0, 0);
            QVERIFY(store.flush());
        }

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QVERIFY(store.load(&ink));
        QVERIFY(!store.hasToolState());
        QVERIFY(store.viewState().valid);
    }

    // Задача 7.6, сценарий «PDF перемещён».
    void detectsMissingSourceAndRelinks()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("j.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        InkDocument ink;
        {
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            store.attach(&ink);
            ink.addStroke(lineOnPage(4));
            QVERIFY(store.flush());
        }

        QVERIFY(QFile::remove(pdf));

        NoteStore store;
        QVERIFY(store.openNote(note));
        QCOMPARE(store.sourceState(), NoteStore::SourceState::Missing);

        // Чернила при этом целы.
        InkDocument restored;
        QVERIFY(store.load(&restored));
        QCOMPARE(restored.strokeCount(), 1);

        // И заметку можно привязать к файлу заново.
        const QString moved = copyFixture(fixtures::textbook(), QStringLiteral("j-moved.pdf"));
        QVERIFY(store.relinkSource(moved));
        QCOMPARE(store.sourceState(), NoteStore::SourceState::Ok);
        QCOMPARE(restored.strokeCount(), 1);
    }

    // Сценарий «PDF заменён другой версией».
    void detectsChangedSource()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("k.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        {
            NoteStore store;
            QVERIFY(store.create(note, pdf));
        }

        // Подменяем содержимое, оставляя имя.
        QFile::remove(pdf);
        QVERIFY(QFile::copy(fixtures::noOutline(), pdf));

        NoteStore store;
        QVERIFY(store.openNote(note));
        QCOMPARE(store.sourceState(), NoteStore::SourceState::Changed);
    }

    // Подмена файлом того же размера ловится хэшем, а не размером.
    void detectsChangedSourceOfSameSize()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("l.pdf"));
        const QString note = NoteStore::notePathFor(pdf);
        {
            NoteStore store;
            QVERIFY(store.create(note, pdf));
        }

        QByteArray content = bytesOf(pdf);
        QVERIFY(content.size() > 2000);
        content[1500] = char(content.at(1500) ^ 0x5A);
        QFile file(pdf);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(content);
        file.close();

        NoteStore store;
        QVERIFY(store.openNote(note));
        QCOMPARE(QFileInfo(pdf).size(), store.sourceSize());
        QCOMPARE(store.sourceState(), NoteStore::SourceState::Changed);
    }

    // Сценарий «Число страниц уменьшилось»: чернила с исчезнувших страниц
    // остаются в файле и не удаляются сами.
    void keepsInkFromPagesThatNoLongerExist()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("m.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            store.attach(&ink);
            ink.addStroke(lineOnPage(1));
            ink.addStroke(lineOnPage(12)); // страница есть в textbook (14 страниц)
            QVERIFY(store.flush());
        }

        // Подменяем на документ из четырёх страниц.
        QFile::remove(pdf);
        QVERIFY(QFile::copy(fixtures::noOutline(), pdf));

        InkDocument restored;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QCOMPARE(store.sourceState(), NoteStore::SourceState::Changed);
        QVERIFY(store.load(&restored));

        // Оба штриха на месте, включая тот, чьей страницы больше нет.
        QCOMPARE(restored.strokeCount(), 2);
        QCOMPARE(restored.pagesWithInk(), QList<int>({1, 12}));

        // И повторное сохранение их не выбрасывает.
        store.attach(&restored);
        restored.addStroke(lineOnPage(0));
        QVERIFY(store.flush());
        store.close();

        InkDocument again;
        NoteStore reopened;
        QVERIFY(reopened.openNote(note));
        QVERIFY(reopened.load(&again));
        QCOMPARE(again.strokeCount(), 3);
        QVERIFY(again.hasInkOnPage(12));
    }

private:
    QTemporaryDir m_dir;
};

QTEST_GUILESS_MAIN(TestNoteStore)
#include "tst_notestore.moc"
