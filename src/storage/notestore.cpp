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

constexpr int kSchemaVersion = 1;
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
                       "  points BLOB NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS stroke_page ON stroke(page)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS view_state ("
                       "  id INTEGER PRIMARY KEY CHECK (id = 1),"
                       "  page INTEGER NOT NULL,"
                       "  zoom REAL NOT NULL,"
                       "  page_mode INTEGER NOT NULL,"
                       "  origin_x REAL NOT NULL,"
                       "  origin_y REAL NOT NULL)"),
    };
    for (const QString &statement : statements) {
        if (!query.exec(statement)) {
            m_errorString = query.lastError().text();
            return false;
        }
    }
    return true;
}

bool NoteStore::setMeta(const QString &key, const QString &value)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO meta(key, value) VALUES(?, ?) "
                                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.addBindValue(key);
    query.addBindValue(value);
    if (!query.exec()) {
        m_errorString = query.lastError().text();
        return false;
    }
    return true;
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

    // Пишем только дельту: отмечаем затронутые штрихи, а что с ними делать,
    // решаем при сохранении.
    const auto touch = [this](quint64 id, int) {
        m_pendingStrokes.insert(id);
        markDirty();
    };
    connect(m_ink, &InkDocument::strokeAdded, this, touch);
    connect(m_ink, &InkDocument::strokeRemoved, this, touch);
}

void NoteStore::markDirty()
{
    emit dirtyChanged();
}

bool NoteStore::load(InkDocument *ink)
{
    if (!isOpen() || !ink)
        return false;

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT id, page, tool, color, width, points FROM stroke ORDER BY id"))) {
        m_errorString = query.lastError().text();
        return false;
    }

    QList<InkDocument::Record> records;
    quint64 maxId = 0;
    while (query.next()) {
        InkDocument::Record record;
        record.id = query.value(0).toULongLong();
        record.stroke.page = query.value(1).toInt();
        record.stroke.tool = InkTool(query.value(2).toInt());
        record.stroke.color = QColor::fromRgba(query.value(3).toUInt());
        record.stroke.width = query.value(4).toReal();
        record.stroke.points = unpackPoints(query.value(5).toByteArray());
        if (!record.stroke.isValid())
            continue;
        maxId = qMax(maxId, record.id);
        records.append(record);
    }

    ink->loadStrokes(records, maxId + 1);
    m_pendingStrokes.clear();

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
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO stroke(id, page, tool, color, width, points) VALUES(?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET page=excluded.page, tool=excluded.tool, "
        "color=excluded.color, width=excluded.width, points=excluded.points"));
    query.addBindValue(qulonglong(record.id));
    query.addBindValue(record.stroke.page);
    query.addBindValue(int(record.stroke.tool));
    query.addBindValue(record.stroke.color.rgba());
    query.addBindValue(record.stroke.width);
    query.addBindValue(packPoints(record.stroke.points));
    if (!query.exec()) {
        m_errorString = query.lastError().text();
        return false;
    }
    return true;
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
        // Модель - источник истины: что в ней есть, то вписывается, чего нет -
        // удаляется. Обход всех штрихов делается один раз на сохранение.
        QHash<quint64, const InkDocument::Record *> index;
        const QList<InkDocument::Record> all = m_ink->allStrokes();
        for (const InkDocument::Record &record : all)
            index.insert(record.id, &record);

        for (quint64 id : std::as_const(m_pendingStrokes)) {
            const auto it = index.constFind(id);
            if (it != index.constEnd()) {
                ok = writeStroke(**it) && ok;
                continue;
            }
            QSqlQuery query(m_database);
            query.prepare(QStringLiteral("DELETE FROM stroke WHERE id = ?"));
            query.addBindValue(qulonglong(id));
            if (!query.exec()) {
                m_errorString = query.lastError().text();
                ok = false;
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
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "INSERT INTO view_state(id, page, zoom, page_mode, origin_x, origin_y) "
            "VALUES(1, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET page=excluded.page, zoom=excluded.zoom, "
            "page_mode=excluded.page_mode, origin_x=excluded.origin_x, "
            "origin_y=excluded.origin_y"));
        query.addBindValue(m_viewState.page);
        query.addBindValue(m_viewState.zoom);
        query.addBindValue(m_viewState.pageMode);
        query.addBindValue(m_viewState.originX);
        query.addBindValue(m_viewState.originY);
        if (!query.exec()) {
            m_errorString = query.lastError().text();
            ok = false;
        }
    }

    if (!m_database.commit()) {
        m_errorString = m_database.lastError().text();
        m_database.rollback();
        return false;
    }

    m_pendingStrokes.clear();
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
