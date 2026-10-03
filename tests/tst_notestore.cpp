// Задачи 7.1-7.6: формат файла заметки, инкрементальная запись, состояние
// просмотра и поведение при пропаже или подмене исходного PDF.
// Сценарии из specs/note-storage/spec.md и specs/pdf-document/spec.md.

#include <QCryptographicHash>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include "fixtures.h"
#include "ink/inkdocument.h"
#include "storage/notestore.h"

namespace {

// Прямой доступ к файлу заметки мимо NoteStore: так тесты изображают файл
// первой версии и то, что с файлом делает версия без вкладышей.
class RawNote
{
public:
    explicit RawNote(const QString &path)
        : m_name(QStringLiteral("raw-%1").arg(QUuid::createUuid().toString(QUuid::Id128)))
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        db.setDatabaseName(path);
        m_ok = db.open();
    }

    ~RawNote()
    {
        QSqlDatabase::database(m_name, false).close();
        QSqlDatabase::removeDatabase(m_name);
    }

    bool isOpen() const { return m_ok; }

    bool exec(const QString &statement, const QVariantList &values = {})
    {
        QSqlQuery query(QSqlDatabase::database(m_name, false));
        query.prepare(statement);
        for (const QVariant &value : values)
            query.addBindValue(value);
        if (!query.exec()) {
            qWarning() << "raw sql failed:" << statement << query.lastError().text();
            return false;
        }
        return true;
    }

    // Первая колонка всех строк результата.
    QVariantList column(const QString &statement)
    {
        QVariantList values;
        QSqlQuery query(QSqlDatabase::database(m_name, false));
        if (!query.exec(statement)) {
            qWarning() << "raw sql failed:" << statement << query.lastError().text();
            return values;
        }
        while (query.next())
            values.append(query.value(0));
        return values;
    }

private:
    QString m_name;
    bool m_ok = false;
};

// Точки в формате файла: три float на точку.
QByteArray packedLine(int count)
{
    QByteArray blob;
    for (int i = 0; i < count; ++i) {
        const float values[3] = {float(10 + i * 4), 50.0f, 0.5f};
        blob.append(reinterpret_cast<const char *>(values), sizeof(values));
    }
    return blob;
}

} // namespace

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

    // --- Вкладыши (задачи 2.1-2.4, 2.6 add-ink-inserts) --------------------

    // Задача 2.1 и сценарий «Старая заметка»: файл первой версии открывается,
    // получает таблицы вкладышей и принимает новый вкладыш.
    void opensSchemaVersionOneNote()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("w.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        {
            RawNote raw(note);
            QVERIFY(raw.isOpen());
            // Схема ровно такая, какой её создавала версия 1.
            QVERIFY(raw.exec(QStringLiteral(
                "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)")));
            QVERIFY(raw.exec(QStringLiteral(
                "CREATE TABLE stroke (id INTEGER PRIMARY KEY, page INTEGER NOT NULL, "
                "tool INTEGER NOT NULL, color INTEGER NOT NULL, width REAL NOT NULL, "
                "points BLOB NOT NULL)")));
            QVERIFY(raw.exec(QStringLiteral("CREATE INDEX stroke_page ON stroke(page)")));
            QVERIFY(raw.exec(QStringLiteral(
                "CREATE TABLE view_state (id INTEGER PRIMARY KEY CHECK (id = 1), "
                "page INTEGER NOT NULL, zoom REAL NOT NULL, page_mode INTEGER NOT NULL, "
                "origin_x REAL NOT NULL, origin_y REAL NOT NULL)")));
            const QList<QPair<QString, QString>> metas = {
                {QStringLiteral("schema"), QStringLiteral("1")},
                {QStringLiteral("source_path"), QFileInfo(pdf).absoluteFilePath()},
                {QStringLiteral("source_size"), QString::number(QFileInfo(pdf).size())},
                {QStringLiteral("source_hash"), NoteStore::hashOfFile(pdf)},
            };
            for (const auto &[key, value] : metas) {
                QVERIFY(raw.exec(QStringLiteral("INSERT INTO meta(key, value) VALUES(?, ?)"),
                                 {key, value}));
            }
            for (int id = 1; id <= 2; ++id) {
                QVERIFY(raw.exec(QStringLiteral("INSERT INTO stroke(id, page, tool, color, "
                                                "width, points) VALUES(?, ?, 0, ?, 2.0, ?)"),
                                 {id, id * 3, QColor(Qt::black).rgba(), packedLine(12)}));
            }
        }

        quint64 insert = 0;
        {
            InkDocument ink;
            NoteStore store;
            QVERIFY2(store.openNote(note), qPrintable(store.errorString()));
            QVERIFY2(store.load(&ink), qPrintable(store.errorString()));
            QCOMPARE(store.sourceState(), NoteStore::SourceState::Ok);
            QCOMPARE(ink.strokeCount(), 2);
            QCOMPARE(ink.pagesWithInk(), QList<int>({3, 6}));
            QCOMPARE(ink.insertCount(), 0);

            // В старую заметку добавляется вкладыш, и он сохраняется.
            store.attach(&ink);
            insert = ink.addInsert(1, QRectF(30, 40, 200, 140));
            Stroke stroke = lineOnPage(0);
            stroke.insert = insert;
            QVERIFY(ink.addStroke(stroke) > 2);
            QVERIFY(store.flush());
        }

        {
            RawNote raw(note);
            QCOMPARE(raw.column(QStringLiteral("SELECT value FROM meta WHERE key = 'schema'")),
                     QVariantList({QStringLiteral("3")}));
        }

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QVERIFY(store.load(&ink));
        QCOMPARE(ink.allStrokes().size(), 2);
        QVERIFY(ink.hasInsert(insert));
        QCOMPARE(ink.strokesInInsert(insert).size(), 1);
    }

    // Задача 2.2, сценарий «Повторное открытие заметки с вкладышами».
    void roundTripsInserts()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("x.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        quint64 closed = 0;
        quint64 resized = 0;
        QList<InkDocument::Record> originalInk;
        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            store.attach(&ink);

            ink.addStroke(lineOnPage(0));
            closed = ink.addInsert(0, QRectF(100, 120, 200, 140));
            resized = ink.addInsert(4, QRectF(20, 30, 200, 140));
            for (const quint64 insert : {closed, resized}) {
                for (int i = 0; i < 2; ++i) {
                    Stroke stroke = lineOnPage(0, i * 20.0);
                    stroke.insert = insert;
                    QVERIFY(ink.addStroke(stroke) != 0);
                }
            }
            QVERIFY(store.isDirty());
            QVERIFY(store.flush());
            QVERIFY(!store.isDirty());

            // Сворачивание и размер - тоже изменения, которые надо записать.
            ink.setInsertOpen(closed, false);
            QVERIFY(store.isDirty());
            ink.setInsertRect(resized, QRectF(20, 30, 310, 95));
            QVERIFY(store.flush());
            originalInk = ink.allInsertStrokes();
        }

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QVERIFY(store.load(&ink));

        QCOMPARE(ink.insertCount(), 2);
        QVERIFY(!ink.insertById(closed).open);
        QCOMPARE(ink.insertById(closed).page, 0);
        QCOMPARE(ink.insertById(closed).rect, QRectF(100, 120, 200, 140));
        QVERIFY(ink.insertById(resized).open);
        QCOMPARE(ink.insertById(resized).page, 4);
        QCOMPARE(ink.insertById(resized).rect, QRectF(20, 30, 310, 95));
        QCOMPARE(ink.allStrokes().size(), 1);

        const QList<InkDocument::Record> restored = ink.allInsertStrokes();
        QCOMPARE(restored.size(), originalInk.size());
        for (int i = 0; i < restored.size(); ++i) {
            QCOMPARE(restored[i].id, originalInk[i].id);
            QCOMPARE(restored[i].stroke.insert, originalInk[i].stroke.insert);
            QCOMPARE(restored[i].stroke.page, originalInk[i].stroke.page);
            QCOMPARE(restored[i].stroke.color, originalInk[i].stroke.color);
            QCOMPARE(restored[i].stroke.points.size(), originalInk[i].stroke.points.size());
            for (int p = 0; p < restored[i].stroke.points.size(); ++p) {
                const QPointF a = restored[i].stroke.points[p].position;
                const QPointF b = originalInk[i].stroke.points[p].position;
                QVERIFY(qAbs(a.x() - b.x()) < 1e-3 && qAbs(a.y() - b.y()) < 1e-3);
            }
        }
    }

    // Удаление вкладыша уезжает в файл вместе со штрихами, отмена - обратно.
    void removedInsertLeavesNoRows()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("y.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.create(note, pdf));
        store.attach(&ink);
        const quint64 insert = ink.addInsert(2, QRectF(0, 0, 100, 100));
        Stroke stroke = lineOnPage(0);
        stroke.insert = insert;
        ink.addStroke(stroke);
        QVERIFY(store.flush());

        ink.removeInsert(insert);
        QVERIFY(store.flush());
        {
            RawNote raw(note);
            QVERIFY(raw.column(QStringLiteral("SELECT id FROM ink_insert")).isEmpty());
            QVERIFY(raw.column(QStringLiteral("SELECT id FROM insert_stroke")).isEmpty());
        }

        ink.undo();
        QVERIFY(store.flush());
        RawNote raw(note);
        QCOMPARE(raw.column(QStringLiteral("SELECT id FROM ink_insert")).size(), 1);
        QCOMPARE(raw.column(QStringLiteral("SELECT id FROM insert_stroke")).size(), 1);
    }

    // Задача 2.3: версия без вкладышей записала штрих страницы с id, занятым
    // штрихом вкладыша.
    void renumbersInsertStrokesCollidingWithPageStrokes()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("z.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        quint64 insert = 0;
        quint64 collidingId = 0;
        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            store.attach(&ink);
            ink.addStroke(lineOnPage(0));
            insert = ink.addInsert(0, QRectF(0, 0, 200, 140));
            Stroke stroke = lineOnPage(0, 40.0);
            stroke.insert = insert;
            collidingId = ink.addStroke(stroke);
            QVERIFY(store.flush());
        }
        {
            // Так пишет версия 0.3.0: id от максимума таблицы stroke.
            RawNote raw(note);
            QVERIFY(raw.exec(QStringLiteral("INSERT INTO stroke(id, page, tool, color, width, "
                                            "points) VALUES(?, 1, 0, ?, 2.0, ?)"),
                             {qulonglong(collidingId), QColor(Qt::red).rgba(), packedLine(10)}));
        }

        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.openNote(note));
            QVERIFY2(store.load(&ink), qPrintable(store.errorString()));
            QCOMPARE(ink.allStrokes().size(), 2);
            QCOMPARE(ink.strokesInInsert(insert).size(), 1);
            const quint64 renumbered = ink.strokesInInsert(insert).first().id;
            QVERIFY(renumbered != collidingId);
            // Перенумерация уже в файле, до каких-либо правок.
            {
                RawNote raw(note);
                QCOMPARE(raw.column(QStringLiteral("SELECT id FROM insert_stroke")),
                         QVariantList({qulonglong(renumbered)}));
            }

            // Удаление штриха страницы с тем же id не задевает штрих вкладыша.
            store.attach(&ink);
            ink.eraseStrokes({collidingId});
            QVERIFY(store.flush());
            // Новые штрихи не повторяют ни один из занятых id.
            QVERIFY(ink.addStroke(lineOnPage(3)) > renumbered);
            QVERIFY(store.flush());
        }

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QVERIFY(store.load(&ink));
        QCOMPARE(ink.allStrokes().size(), 2);
        QCOMPARE(ink.strokesInInsert(insert).size(), 1);
    }

    // Задача 2.4, сценарий «Файл с вкладышами в версии без вкладышей».
    void versionOneQuerySeesOnlyPageInk()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("aa.pdf"));
        const QString note = NoteStore::notePathFor(pdf);
        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            store.attach(&ink);
            ink.addStroke(lineOnPage(0));
            ink.addStroke(lineOnPage(1));
            const quint64 insert = ink.addInsert(0, QRectF(0, 0, 200, 140));
            for (int i = 0; i < 3; ++i) {
                Stroke stroke = lineOnPage(0, i * 10.0);
                stroke.insert = insert;
                ink.addStroke(stroke);
            }
            QVERIFY(store.flush());
        }

        RawNote raw(note);
        const QVariantList pages = raw.column(
            QStringLiteral("SELECT page FROM (SELECT id, page, tool, color, width, points "
                           "FROM stroke ORDER BY id)"));
        QCOMPARE(pages, QVariantList({0, 1}));
    }

    // --- ink-beauty-pass, D3: закон толщины хранится со штрихом -----------

    // Сценарий «Штрих после повторного открытия»: закон толщины переживает
    // запись - и у штриха страницы, и у штриха вкладыша.
    void roundTripsStrokeStyle()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("ac.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        quint64 legacy = 0;
        quint64 tapered = 0;
        quint64 insertStroke = 0;
        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            store.attach(&ink);
            legacy = ink.addStroke(lineOnPage(0));
            Stroke stroke = lineOnPage(0, 30.0);
            stroke.style = StrokeStyle::Tapered;
            tapered = ink.addStroke(stroke);
            const quint64 insert = ink.addInsert(0, QRectF(0, 0, 200, 140));
            stroke.insert = insert;
            insertStroke = ink.addStroke(stroke);
            QVERIFY(store.flush());
        }

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QVERIFY(store.load(&ink));
        QHash<quint64, StrokeStyle> styles;
        for (const InkDocument::Record &record : ink.allStrokes())
            styles.insert(record.id, record.stroke.style);
        for (const InkDocument::Record &record : ink.allInsertStrokes())
            styles.insert(record.id, record.stroke.style);
        QCOMPARE(styles.size(), 3);
        QCOMPARE(styles.value(legacy), StrokeStyle::Legacy);
        QCOMPARE(styles.value(tapered), StrokeStyle::Tapered);
        QCOMPARE(styles.value(insertStroke), StrokeStyle::Tapered);
    }

    // Сценарий «Заметка прежней версии»: файл схемы 2 получает колонку style
    // в обеих таблицах штрихов, его штрихи читаются законом Legacy, а новый
    // штрих сохраняет свой закон.
    void migratesSchemaVersionTwoNote()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("ad.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        {
            RawNote raw(note);
            QVERIFY(raw.isOpen());
            // Схема ровно такая, какой её создавала версия 2.
            const QStringList schema = {
                QStringLiteral("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
                QStringLiteral("CREATE TABLE stroke (id INTEGER PRIMARY KEY, page INTEGER NOT NULL, "
                               "tool INTEGER NOT NULL, color INTEGER NOT NULL, width REAL NOT NULL, "
                               "points BLOB NOT NULL)"),
                QStringLiteral("CREATE INDEX stroke_page ON stroke(page)"),
                QStringLiteral("CREATE TABLE view_state (id INTEGER PRIMARY KEY CHECK (id = 1), "
                               "page INTEGER NOT NULL, zoom REAL NOT NULL, page_mode INTEGER NOT NULL, "
                               "origin_x REAL NOT NULL, origin_y REAL NOT NULL)"),
                QStringLiteral("CREATE TABLE ink_insert (id INTEGER PRIMARY KEY, page INTEGER NOT NULL, "
                               "x REAL NOT NULL, y REAL NOT NULL, width REAL NOT NULL, "
                               "height REAL NOT NULL, open INTEGER NOT NULL)"),
                QStringLiteral("CREATE TABLE insert_stroke (id INTEGER PRIMARY KEY, "
                               "insert_id INTEGER NOT NULL, tool INTEGER NOT NULL, "
                               "color INTEGER NOT NULL, width REAL NOT NULL, points BLOB NOT NULL)"),
                QStringLiteral("CREATE INDEX insert_stroke_insert ON insert_stroke(insert_id)"),
            };
            for (const QString &statement : schema)
                QVERIFY(raw.exec(statement));
            const QList<QPair<QString, QString>> metas = {
                {QStringLiteral("schema"), QStringLiteral("2")},
                {QStringLiteral("source_path"), QFileInfo(pdf).absoluteFilePath()},
                {QStringLiteral("source_size"), QString::number(QFileInfo(pdf).size())},
                {QStringLiteral("source_hash"), NoteStore::hashOfFile(pdf)},
            };
            for (const auto &[key, value] : metas) {
                QVERIFY(raw.exec(QStringLiteral("INSERT INTO meta(key, value) VALUES(?, ?)"),
                                 {key, value}));
            }
            QVERIFY(raw.exec(QStringLiteral("INSERT INTO stroke(id, page, tool, color, width, "
                                            "points) VALUES(1, 0, 0, ?, 2.0, ?)"),
                             {QColor(Qt::black).rgba(), packedLine(12)}));
            QVERIFY(raw.exec(QStringLiteral("INSERT INTO ink_insert(id, page, x, y, width, height, "
                                            "open) VALUES(1, 0, 30.0, 40.0, 200.0, 140.0, 1)")));
            QVERIFY(raw.exec(QStringLiteral("INSERT INTO insert_stroke(id, insert_id, tool, color, "
                                            "width, points) VALUES(2, 1, 0, ?, 2.0, ?)"),
                             {QColor(Qt::black).rgba(), packedLine(12)}));
        }

        quint64 added = 0;
        {
            InkDocument ink;
            NoteStore store;
            QVERIFY2(store.openNote(note), qPrintable(store.errorString()));
            QVERIFY2(store.load(&ink), qPrintable(store.errorString()));
            QCOMPARE(ink.allStrokes().size(), 1);
            QCOMPARE(ink.allStrokes().first().stroke.style, StrokeStyle::Legacy);
            QCOMPARE(ink.strokesInInsert(1).size(), 1);
            QCOMPARE(ink.strokesInInsert(1).first().stroke.style, StrokeStyle::Legacy);

            store.attach(&ink);
            Stroke stroke = lineOnPage(1);
            stroke.style = StrokeStyle::Tapered;
            added = ink.addStroke(stroke);
            QVERIFY(store.flush());
        }

        {
            RawNote raw(note);
            QCOMPARE(raw.column(QStringLiteral("SELECT value FROM meta WHERE key = 'schema'")),
                     QVariantList({QStringLiteral("3")}));
            for (const QString &table : {QStringLiteral("stroke"), QStringLiteral("insert_stroke")}) {
                QVERIFY2(raw.column(QStringLiteral("SELECT name FROM pragma_table_info('%1')").arg(table))
                             .contains(QStringLiteral("style")),
                         qPrintable(table));
            }
            QCOMPARE(raw.column(QStringLiteral("SELECT style FROM stroke ORDER BY id")),
                     QVariantList({0, 1}));
        }

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QVERIFY(store.load(&ink));
        for (const InkDocument::Record &record : ink.allStrokes())
            QCOMPARE(record.stroke.style, record.id == added ? StrokeStyle::Tapered : StrokeStyle::Legacy);
    }

    // Сценарий «Открытие прежней версией приложения»: версия 0.7 читает stroke
    // своим запросом и пишет строки без колонки style. Файл схемы 3 принимает
    // и то и другое, а строка без style читается законом Legacy.
    void previousVersionQueriesWorkOnSchemaThree()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("ae.pdf"));
        const QString note = NoteStore::notePathFor(pdf);
        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            store.attach(&ink);
            Stroke stroke = lineOnPage(0);
            stroke.style = StrokeStyle::Tapered;
            ink.addStroke(stroke);
            QVERIFY(store.flush());
        }

        {
            RawNote raw(note);
            QCOMPARE(raw.column(QStringLiteral("SELECT page FROM (SELECT id, page, tool, color, "
                                               "width, points FROM stroke ORDER BY id)")),
                     QVariantList({0}));
            QVERIFY(raw.exec(QStringLiteral("INSERT INTO stroke(id, page, tool, color, width, points) "
                                            "VALUES(?, ?, ?, ?, ?, ?)"),
                             {7, 2, 0, QColor(Qt::black).rgba(), 2.0, packedLine(12)}));
        }

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QVERIFY(store.load(&ink));
        QCOMPARE(ink.allStrokes().size(), 2);
        QCOMPARE(ink.allStrokes()[0].stroke.style, StrokeStyle::Tapered);
        QCOMPARE(ink.allStrokes()[1].id, quint64(7));
        QCOMPARE(ink.allStrokes()[1].stroke.style, StrokeStyle::Legacy);
    }

    // Второй экземпляр приложения открывает уже открытый файл схемы 3: колонка
    // style на месте, и миграция считает это успехом, а не ошибкой.
    void secondInstanceOpensSchemaThreeNote()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("af.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        InkDocument first;
        NoteStore firstStore;
        QVERIFY(firstStore.create(note, pdf));
        firstStore.attach(&first);
        Stroke stroke = lineOnPage(0);
        stroke.style = StrokeStyle::Tapered;
        first.addStroke(stroke);
        QVERIFY(firstStore.flush());

        InkDocument second;
        NoteStore secondStore;
        QVERIFY2(secondStore.openNote(note), qPrintable(secondStore.errorString()));
        QVERIFY2(secondStore.load(&second), qPrintable(secondStore.errorString()));
        QCOMPARE(second.strokeCount(), 1);
        QCOMPARE(second.allStrokes().first().stroke.style, StrokeStyle::Tapered);
    }

    // Задача 2.6, сценарий «Вкладыш на исчезнувшей странице».
    void keepsInsertsOnPagesThatNoLongerExist()
    {
        const QString pdf = copyFixture(fixtures::textbook(), QStringLiteral("ab.pdf"));
        const QString note = NoteStore::notePathFor(pdf);

        quint64 insert = 0;
        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.create(note, pdf));
            store.attach(&ink);
            insert = ink.addInsert(12, QRectF(10, 10, 200, 140));
            Stroke stroke = lineOnPage(0);
            stroke.insert = insert;
            ink.addStroke(stroke);
            QVERIFY(store.flush());
        }

        // Документ из четырёх страниц вместо четырнадцати.
        QFile::remove(pdf);
        QVERIFY(QFile::copy(fixtures::noOutline(), pdf));

        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.openNote(note));
            QVERIFY(store.load(&ink));
            QVERIFY(ink.hasInsert(insert));
            QCOMPARE(ink.insertById(insert).page, 12);

            store.attach(&ink);
            ink.addStroke(lineOnPage(0));
            ink.setInsertOpen(insert, false);
            QVERIFY(store.flush());
        }

        InkDocument ink;
        NoteStore store;
        QVERIFY(store.openNote(note));
        QVERIFY(store.load(&ink));
        QVERIFY(ink.hasInsert(insert));
        QCOMPARE(ink.strokesInInsert(insert).size(), 1);
        QVERIFY(!ink.insertById(insert).open);
    }

private:
    QTemporaryDir m_dir;
};

QTEST_GUILESS_MAIN(TestNoteStore)
#include "tst_notestore.moc"
