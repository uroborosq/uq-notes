#include "notestore.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {

// Версия 2 добавила вкладыши. Их таблицы создаются через CREATE TABLE IF NOT
// EXISTS, как и прежние, поэтому файл первой версии получает их при открытии
// без отдельной миграции (design.md, D8).
//
// Версия 3 добавила закон толщины штриха - колонку style в stroke и
// insert_stroke (ink-beauty-pass, design.md, D3). Колонку в существующую
// таблицу CREATE TABLE IF NOT EXISTS не добавит, поэтому для неё миграция
// явная: ensureColumn при каждом открытии.
constexpr int kSchemaVersion = 3;
// Автосохранение раз в пять секунд: спека разрешает терять не больше тридцати,
// и запас нужен на то, чтобы одно пропущенное срабатывание ничего не стоило.
constexpr int kDefaultAutosaveMs = 5000;

// Точки пакуются как три float на точку. Давление -1 означает «устройство
// давления не сообщает» и переживает запись как есть.
QByteArray packPoints(const QList<InkPoint> &points)
{
    QByteArray blob;
    blob.resize(int(points.size()) * 3 * int(sizeof(float)));
    auto *out = reinterpret_cast<float *>(blob.data());
    for (const InkPoint &point : points) {
        *out++ = float(point.position.x());
        *out++ = float(point.position.y());
        *out++ = float(point.pressure);
    }
    return blob;
}

QList<InkPoint> unpackPoints(const QByteArray &blob)
{
    QList<InkPoint> points;
    const int count = blob.size() / (3 * int(sizeof(float)));
    points.reserve(count);
    const auto *in = reinterpret_cast<const float *>(blob.constData());
    for (int i = 0; i < count; ++i) {
        InkPoint point;
        point.position = QPointF(in[i * 3], in[i * 3 + 1]);
        point.pressure = in[i * 3 + 2];
        points.append(point);
    }
    return points;
}

// Закон толщины из файла. Неизвестное значение - от будущей версии - читается
// как Legacy: так же прежние версии читают файл, о колонке не зная.
StrokeStyle styleFromFile(int value)
{
    return value == int(StrokeStyle::Tapered) ? StrokeStyle::Tapered : StrokeStyle::Legacy;
}

} // namespace

NoteStore::NoteStore(QObject *parent)
    : QObject(parent)
{
    m_autosave.setInterval(kDefaultAutosaveMs);
    connect(&m_autosave, &QTimer::timeout, this, [this] {
        if (isDirty())
            flush();
    });
}

NoteStore::~NoteStore()
{
    close();
}

QString NoteStore::notePathFor(const QString &pdfPath)
{
    const QFileInfo info(pdfPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".uqn"));
}

// SHA-256 файла: по нему видно, что PDF подменили другой версией, даже если
// размер совпал.
QString NoteStore::hashOfFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

bool NoteStore::create(const QString &notePath, const QString &pdfPath)
{
    close();

    m_connectionName = QStringLiteral("uqn-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(notePath);
    if (!m_database.open()) {
        m_errorString = m_database.lastError().text();
        return false;
    }

    if (!initializeSchema())
        return false;

    m_notePath = notePath;
    m_sourcePath = QFileInfo(pdfPath).absoluteFilePath();
    m_sourceSize = QFileInfo(pdfPath).size();
    m_sourceHash = hashOfFile(pdfPath);

    setMeta(QStringLiteral("schema"), QString::number(kSchemaVersion));
    setMeta(QStringLiteral("source_path"), m_sourcePath);
    setMeta(QStringLiteral("source_size"), QString::number(m_sourceSize));
    setMeta(QStringLiteral("source_hash"), m_sourceHash);

    m_autosave.start();
    emit stateChanged();
    return true;
}

bool NoteStore::openNote(const QString &notePath)
{
    if (!QFileInfo::exists(notePath)) {
        m_errorString = tr("Файл заметки не найден: %1").arg(notePath);
        return false;
    }

    close();
    m_connectionName = QStringLiteral("uqn-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(notePath);
    if (!m_database.open()) {
        m_errorString = m_database.lastError().text();
        return false;
    }

    if (!initializeSchema())
        return false;

    m_notePath = notePath;
    m_sourcePath = meta(QStringLiteral("source_path"));
    m_sourceSize = meta(QStringLiteral("source_size")).toLongLong();
    m_sourceHash = meta(QStringLiteral("source_hash"));

    // Схема уже приведена к текущей версии в initializeSchema(); здесь это
    // только отмечается. Версию ниже не опускаем: файл из будущей версии
    // останется помеченным своей.
    if (meta(QStringLiteral("schema")).toInt() < kSchemaVersion)
        setMeta(QStringLiteral("schema"), QString::number(kSchemaVersion));

    m_autosave.start();
    emit stateChanged();
    return true;
}

bool NoteStore::openOrCreate(const QString &notePath, const QString &pdfPath)
{
    if (QFileInfo::exists(notePath)) {
        if (!openNote(notePath))
            return false;
        // Путь к исходнику мог протухнуть, а файл при этом лежит рядом с
        // заметкой: подставляем то, что открыли на самом деле.
        if (sourceState() != SourceState::Ok
            && QFileInfo(pdfPath).absoluteFilePath() != m_sourcePath
            && QFileInfo::exists(pdfPath)) {
            relinkSource(pdfPath);
        }
        return true;
    }
    return create(notePath, pdfPath);
}

void NoteStore::close()
{
    if (m_database.isOpen()) {
        if (isDirty())
            flush();
        m_database.close();
    }
    if (!m_connectionName.isEmpty()) {
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
    }
    m_autosave.stop();
    m_notePath.clear();
    m_sourcePath.clear();
    m_viewState = ViewState();
    m_toolWidth = 0.0;
    m_toolColor.clear();
    m_toolValid = false;
    m_toolDirty = false;
    m_sourceSize = 0;
    m_sourceHash.clear();
    m_pendingStrokes.clear();
    m_pendingInserts.clear();
    m_viewStateDirty = false;
    emit stateChanged();
}

bool NoteStore::initializeSchema()
{
    QSqlQuery query(m_database);

    // WAL: запись идёт дельтой, а незавершённая транзакция откатывается сама -
    // отсюда и устойчивость к аварийному завершению без собственного журнала.
    if (!query.exec(QStringLiteral("PRAGMA journal_mode=WAL"))) {
        m_errorString = query.lastError().text();
        return false;
    }
    query.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));

    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS meta ("
                       "  key TEXT PRIMARY KEY,"
                       "  value TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS stroke ("
                       "  id INTEGER PRIMARY KEY,"
                       "  page INTEGER NOT NULL,"
                       "  tool INTEGER NOT NULL,"
                       "  color INTEGER NOT NULL,"
                       "  width REAL NOT NULL,"
                       "  points BLOB NOT NULL,"
                       "  style INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS stroke_page ON stroke(page)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS view_state ("
                       "  id INTEGER PRIMARY KEY CHECK (id = 1),"
                       "  page INTEGER NOT NULL,"
                       "  zoom REAL NOT NULL,"
                       "  page_mode INTEGER NOT NULL,"
                       "  origin_x REAL NOT NULL,"
                       "  origin_y REAL NOT NULL)"),
        // Вкладыши и их штрихи - в своих таблицах, а не колонкой в stroke:
        // версия без вкладышей читает stroke целиком и иначе показала бы
        // чернила вкладышей на странице в чужих координатах.
        QStringLiteral("CREATE TABLE IF NOT EXISTS ink_insert ("
                       "  id INTEGER PRIMARY KEY,"
                       "  page INTEGER NOT NULL,"
                       "  x REAL NOT NULL,"
                       "  y REAL NOT NULL,"
                       "  width REAL NOT NULL,"
                       "  height REAL NOT NULL,"
                       "  open INTEGER NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS insert_stroke ("
                       "  id INTEGER PRIMARY KEY,"
                       "  insert_id INTEGER NOT NULL,"
                       "  tool INTEGER NOT NULL,"
                       "  color INTEGER NOT NULL,"
                       "  width REAL NOT NULL,"
                       "  points BLOB NOT NULL,"
                       "  style INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS insert_stroke_insert "
                       "ON insert_stroke(insert_id)"),
    };
    for (const QString &statement : statements) {
        if (!query.exec(statement)) {
            m_errorString = query.lastError().text();
            return false;
        }
    }

    // Схема 3. DEFAULT 0 - закон Legacy: им читаются все штрихи файла прежней
    // схемы, и такие же строки пишет прежняя версия приложения, которая о
    // колонке не знает (сценарий «Открытие прежней версией приложения»).
    for (const QString &table : {QStringLiteral("stroke"), QStringLiteral("insert_stroke")}) {
        if (!ensureColumn(table, QStringLiteral("style"),
                          QStringLiteral("INTEGER NOT NULL DEFAULT 0")))
            return false;
    }
    return true;
}

bool NoteStore::ensureColumn(const QString &table, const QString &column,
                             const QString &definition)
{
    QSqlQuery query(m_database);
    const auto hasColumn = [&] {
        if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
            return false;
        while (query.next()) {
            if (query.value(1).toString() == column) // колонки: cid, name, type, ...
                return true;
        }
        return false;
    };
    if (hasColumn())
        return true;
    if (!query.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, column, definition))) {
        const QString error = query.lastError().text();
        // Второй экземпляр приложения мог открыть тот же файл и добавить колонку
        // между нашими PRAGMA и ALTER: тогда ALTER падает с «duplicate column
        // name», а миграция на деле уже сделана.
        if (hasColumn())
            return true;
        m_errorString = error;
        return false;
    }
    return true;
}

bool NoteStore::setMeta(const QString &key, const QString &value)
{
    return exec(QStringLiteral("INSERT INTO meta(key, value) VALUES(?, ?) "
                               "ON CONFLICT(key) DO UPDATE SET value = excluded.value"),
                {key, value});
}

QString NoteStore::meta(const QString &key) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    query.addBindValue(key);
    if (query.exec() && query.next())
        return query.value(0).toString();
    return QString();
}

bool NoteStore::exec(const QString &statement, const QVariantList &values)
{
    QSqlQuery query(m_database);
    query.prepare(statement);
    for (const QVariant &value : values)
        query.addBindValue(value);
    if (!query.exec()) {
        m_errorString = query.lastError().text();
        return false;
    }
    return true;
}

NoteStore::SourceState NoteStore::sourceState() const
{
    if (m_sourcePath.isEmpty())
        return SourceState::Unknown;
    const QFileInfo info(m_sourcePath);
    if (!info.exists())
        return SourceState::Missing;
    // Размер проверяется первым: он отсеивает подмену мгновенно, а хэш
    // считается только когда размер совпал.
    if (info.size() != m_sourceSize)
        return SourceState::Changed;
    if (!m_sourceHash.isEmpty() && hashOfFile(m_sourcePath) != m_sourceHash)
        return SourceState::Changed;
    return SourceState::Ok;
}

bool NoteStore::relinkSource(const QString &pdfPath)
{
    if (!isOpen() || !QFileInfo::exists(pdfPath))
        return false;

    m_sourcePath = QFileInfo(pdfPath).absoluteFilePath();
    m_sourceSize = QFileInfo(pdfPath).size();
    m_sourceHash = hashOfFile(pdfPath);

    setMeta(QStringLiteral("source_path"), m_sourcePath);
    setMeta(QStringLiteral("source_size"), QString::number(m_sourceSize));
    setMeta(QStringLiteral("source_hash"), m_sourceHash);

    emit stateChanged();
    return true;
}

void NoteStore::attach(InkDocument *ink)
{
    if (m_ink == ink)
        return;
    if (m_ink)
        m_ink->disconnect(this);

    m_ink = ink;
    if (!m_ink)
        return;

    // Пишем только дельту: отмечаем затронутые штрихи и вкладыши, а что с ними
    // делать, решаем при сохранении.
    const auto touchStroke = [this](quint64 id, int) {
        m_pendingStrokes.insert(id);
        markDirty();
    };
    connect(m_ink, &InkDocument::strokeAdded, this, touchStroke);
    connect(m_ink, &InkDocument::strokeRemoved, this, touchStroke);

    const auto touchInsert = [this](quint64 id, int) {
        m_pendingInserts.insert(id);
        markDirty();
    };
    connect(m_ink, &InkDocument::insertAdded, this, touchInsert);
    connect(m_ink, &InkDocument::insertChanged, this, touchInsert);
    connect(m_ink, &InkDocument::insertRemoved, this, touchInsert);
}

void NoteStore::markDirty()
{
    emit dirtyChanged();
}

bool NoteStore::load(InkDocument *ink)
{
    if (!isOpen() || !ink)
        return false;

    const auto readStroke = [](const QSqlQuery &query, InkDocument::Record *record) {
        record->id = query.value(0).toULongLong();
        record->stroke.tool = InkTool(query.value(2).toInt());
        record->stroke.color = QColor::fromRgba(query.value(3).toUInt());
        record->stroke.width = query.value(4).toReal();
        record->stroke.points = unpackPoints(query.value(5).toByteArray());
        record->stroke.style = styleFromFile(query.value(6).toInt());
    };

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT id, page, tool, color, width, points, style FROM stroke ORDER BY id"))) {
        m_errorString = query.lastError().text();
        return false;
    }

    QList<InkDocument::Record> records;
    QSet<quint64> pageStrokeIds;
    quint64 maxId = 0;
    while (query.next()) {
        InkDocument::Record record;
        readStroke(query, &record);
        record.stroke.page = query.value(1).toInt();
        // Максимум считается и по пропущенным строкам: их id в таблице заняты.
        maxId = qMax(maxId, record.id);
        pageStrokeIds.insert(record.id);
        if (!record.stroke.isValid())
            continue;
        records.append(record);
    }

    QList<InkInsert> inserts;
    quint64 maxInsertId = 0;
    QSqlQuery insertQuery(m_database);
    if (!insertQuery.exec(QStringLiteral(
            "SELECT id, page, x, y, width, height, open FROM ink_insert ORDER BY id"))) {
        m_errorString = insertQuery.lastError().text();
        return false;
    }
    while (insertQuery.next()) {
        InkInsert insert;
        insert.id = insertQuery.value(0).toULongLong();
        insert.page = insertQuery.value(1).toInt();
        insert.rect = QRectF(insertQuery.value(2).toReal(), insertQuery.value(3).toReal(),
                             insertQuery.value(4).toReal(), insertQuery.value(5).toReal());
        insert.open = insertQuery.value(6).toBool();
        maxInsertId = qMax(maxInsertId, insert.id);
        if (insert.isValid())
            inserts.append(insert);
    }

    QSqlQuery insertStrokeQuery(m_database);
    if (!insertStrokeQuery.exec(QStringLiteral(
            "SELECT id, insert_id, tool, color, width, points, style FROM insert_stroke "
            "ORDER BY id"))) {
        m_errorString = insertStrokeQuery.lastError().text();
        return false;
    }
    QList<InkDocument::Record> insertRecords;
    while (insertStrokeQuery.next()) {
        InkDocument::Record record;
        readStroke(insertStrokeQuery, &record);
        record.stroke.insert = insertStrokeQuery.value(1).toULongLong();
        maxId = qMax(maxId, record.id);
        if (record.stroke.insert == 0 || record.stroke.points.size() < 2)
            continue;
        insertRecords.append(record);
    }

    // Коллизия id. Версия без вкладышей раздаёт id штрихов от максимума таблицы
    // stroke и может совпасть со штрихом вкладыша. Модели нужно одно
    // пространство id, поэтому штрих вкладыша получает свободный id, и это
    // пишется в файл сразу: пока перенумерация не записана, удаление штриха
    // страницы с тем же id из обеих таблиц задело бы чужую строку (D8).
    QList<QPair<quint64, quint64>> renames;
    for (InkDocument::Record &record : insertRecords) {
        if (!pageStrokeIds.contains(record.id))
            continue;
        const quint64 fresh = ++maxId;
        renames.append({record.id, fresh});
        record.id = fresh;
    }
    if (!renames.isEmpty()) {
        if (!m_database.transaction()) {
            m_errorString = m_database.lastError().text();
            return false;
        }
        bool ok = true;
        for (const auto &[from, to] : std::as_const(renames)) {
            ok = exec(QStringLiteral("UPDATE insert_stroke SET id = ? WHERE id = ?"),
                      {qulonglong(to), qulonglong(from)})
                && ok;
        }
        if (!ok || !m_database.commit()) {
            if (ok)
                m_errorString = m_database.lastError().text();
            m_database.rollback();
            return false;
        }
    }

    records.append(insertRecords);
    ink->loadStrokes(records, maxId + 1, inserts, maxInsertId + 1);
    m_pendingStrokes.clear();
    m_pendingInserts.clear();

    // Состояние просмотра читается сразу: оно нужно тому же открытию.
    QSqlQuery viewQuery(m_database);
    if (viewQuery.exec(QStringLiteral(
            "SELECT page, zoom, page_mode, origin_x, origin_y FROM view_state WHERE id = 1"))
        && viewQuery.next()) {
        m_viewState.page = viewQuery.value(0).toInt();
        m_viewState.zoom = viewQuery.value(1).toReal();
        m_viewState.pageMode = viewQuery.value(2).toInt();
        m_viewState.originX = viewQuery.value(3).toReal();
        m_viewState.originY = viewQuery.value(4).toReal();
        m_viewState.valid = true;
    }

    const QString width = meta(QStringLiteral("tool_width"));
    const QString color = meta(QStringLiteral("tool_color"));
    if (!width.isEmpty() && !color.isEmpty()) {
        m_toolWidth = width.toDouble();
        m_toolColor = color;
        m_toolValid = true;
        m_toolDirty = false;
    }

    emit dirtyChanged();
    emit stateChanged();
    return true;
}

bool NoteStore::writeStroke(const InkDocument::Record &record)
{
    return exec(
        QStringLiteral(
            "INSERT INTO stroke(id, page, tool, color, width, points, style) "
            "VALUES(?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET page=excluded.page, tool=excluded.tool, "
            "color=excluded.color, width=excluded.width, points=excluded.points, "
            "style=excluded.style"),
        {qulonglong(record.id), record.stroke.page, int(record.stroke.tool),
         record.stroke.color.rgba(), record.stroke.width, packPoints(record.stroke.points),
         int(record.stroke.style)});
}

bool NoteStore::writeInsertStroke(const InkDocument::Record &record)
{
    return exec(
        QStringLiteral("INSERT INTO insert_stroke(id, insert_id, tool, color, width, points, "
                       "style) VALUES(?, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(id) DO UPDATE SET insert_id=excluded.insert_id, "
                       "tool=excluded.tool, color=excluded.color, width=excluded.width, "
                       "points=excluded.points, style=excluded.style"),
        {qulonglong(record.id), qulonglong(record.stroke.insert), int(record.stroke.tool),
         record.stroke.color.rgba(), record.stroke.width, packPoints(record.stroke.points),
         int(record.stroke.style)});
}

bool NoteStore::writeInsert(const InkInsert &insert)
{
    return exec(QStringLiteral("INSERT INTO ink_insert(id, page, x, y, width, height, open) "
                               "VALUES(?, ?, ?, ?, ?, ?, ?) "
                               "ON CONFLICT(id) DO UPDATE SET page=excluded.page, x=excluded.x, "
                               "y=excluded.y, width=excluded.width, height=excluded.height, "
                               "open=excluded.open"),
                {qulonglong(insert.id), insert.page, insert.rect.x(), insert.rect.y(),
                 insert.rect.width(), insert.rect.height(), insert.open ? 1 : 0});
}

bool NoteStore::flush()
{
    if (!isOpen())
        return false;
    if (!isDirty())
        return true;

    if (!m_database.transaction()) {
        m_errorString = m_database.lastError().text();
        return false;
    }

    bool ok = true;

    if (m_ink && !m_pendingStrokes.isEmpty()) {
        // Модель - источник истины: что в ней есть, то вписывается в свою
        // таблицу, чего нет - удаляется. Обход всех штрихов делается один раз
        // на сохранение.
        QHash<quint64, const InkDocument::Record *> pageIndex;
        const QList<InkDocument::Record> pageStrokes = m_ink->allStrokes();
        for (const InkDocument::Record &record : pageStrokes)
            pageIndex.insert(record.id, &record);
        QHash<quint64, const InkDocument::Record *> insertIndex;
        const QList<InkDocument::Record> insertStrokes = m_ink->allInsertStrokes();
        for (const InkDocument::Record &record : insertStrokes)
            insertIndex.insert(record.id, &record);

        for (quint64 id : std::as_const(m_pendingStrokes)) {
            if (const auto it = pageIndex.constFind(id); it != pageIndex.constEnd()) {
                ok = writeStroke(**it) && ok;
            } else if (const auto it = insertIndex.constFind(id); it != insertIndex.constEnd()) {
                ok = writeInsertStroke(**it) && ok;
            } else {
                // Идентификаторы в обеих таблицах различны (load() это
                // обеспечивает), поэтому удалять из обеих безопасно.
                ok = exec(QStringLiteral("DELETE FROM stroke WHERE id = ?"), {qulonglong(id)})
                    && ok;
                ok = exec(QStringLiteral("DELETE FROM insert_stroke WHERE id = ?"),
                          {qulonglong(id)})
                    && ok;
            }
        }
    }

    if (m_ink && !m_pendingInserts.isEmpty()) {
        for (quint64 id : std::as_const(m_pendingInserts)) {
            if (m_ink->hasInsert(id)) {
                ok = writeInsert(m_ink->insertById(id)) && ok;
            } else {
                ok = exec(QStringLiteral("DELETE FROM ink_insert WHERE id = ?"), {qulonglong(id)})
                    && ok;
            }
        }
    }

    if (m_toolDirty) {
        // Таблица meta уже есть в схеме первой версии, поэтому старые заметки
        // читаются и пишутся без миграции.
        ok = setMeta(QStringLiteral("tool_width"), QString::number(m_toolWidth)) && ok;
        ok = setMeta(QStringLiteral("tool_color"), m_toolColor) && ok;
    }

    if (m_viewStateDirty) {
        ok = exec(QStringLiteral(
                      "INSERT INTO view_state(id, page, zoom, page_mode, origin_x, origin_y) "
                      "VALUES(1, ?, ?, ?, ?, ?) "
                      "ON CONFLICT(id) DO UPDATE SET page=excluded.page, zoom=excluded.zoom, "
                      "page_mode=excluded.page_mode, origin_x=excluded.origin_x, "
                      "origin_y=excluded.origin_y"),
                  {m_viewState.page, m_viewState.zoom, m_viewState.pageMode,
                   m_viewState.originX, m_viewState.originY})
            && ok;
    }

    if (!m_database.commit()) {
        m_errorString = m_database.lastError().text();
        m_database.rollback();
        return false;
    }

    m_pendingStrokes.clear();
    m_pendingInserts.clear();
    m_viewStateDirty = false;
    m_toolDirty = false;
    emit dirtyChanged();
    emit saved();
    return ok;
}

void NoteStore::setAutosaveIntervalMs(int intervalMs)
{
    m_autosave.setInterval(qMax(200, intervalMs));
    emit stateChanged();
}

NoteStore::ViewState NoteStore::viewState() const
{
    return m_viewState;
}

void NoteStore::setViewState(const ViewState &state)
{
    m_viewState = state;
    m_viewState.valid = true;
    m_viewStateDirty = true;
    markDirty();
}

void NoteStore::rememberTool(qreal width, const QString &color)
{
    if (m_toolValid && qFuzzyCompare(m_toolWidth, width) && m_toolColor == color)
        return;
    m_toolWidth = width;
    m_toolColor = color;
    m_toolValid = true;
    m_toolDirty = true;
    markDirty();
}

void NoteStore::rememberView(int page, qreal zoom, int pageMode, qreal originX, qreal originY)
{
    ViewState state;
    state.page = page;
    state.zoom = zoom;
    state.pageMode = pageMode;
    state.originX = originX;
    state.originY = originY;
    setViewState(state);
}
