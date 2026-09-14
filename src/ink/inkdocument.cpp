#include "inkdocument.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace {
const QList<InkDocument::Record> kNoStrokes;

// Расстояние от точки до отрезка - основа проверки попадания ластика.
qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const qreal lengthSquared = QPointF::dotProduct(ab, ab);
    if (lengthSquared < 1e-12)
        return std::hypot(p.x() - a.x(), p.y() - a.y());
    qreal t = QPointF::dotProduct(p - a, ab) / lengthSquared;
    t = qBound(0.0, t, 1.0);
    const QPointF projection = a + ab * t;
    return std::hypot(p.x() - projection.x(), p.y() - projection.y());
}

QList<quint64> strokesNearIn(const QList<InkDocument::Record> &records, const QPointF &point,
                             qreal radius)
{
    QList<quint64> hits;
    for (const InkDocument::Record &record : records) {
        // Радиус ластика плюс полутолщина штриха: касание края линии тоже
        // считается попаданием.
        const qreal reach = radius + record.stroke.width * 0.5;
        const QList<InkPoint> &points = record.stroke.points;
        for (int i = 0; i + 1 < points.size(); ++i) {
            if (distanceToSegment(point, points[i].position, points[i + 1].position) <= reach) {
                hits.append(record.id);
                break;
            }
        }
    }
    return hits;
}
} // namespace

InkDocument::InkDocument(QObject *parent)
    : QObject(parent)
{
}

quint64 InkDocument::addStroke(const Stroke &stroke)
{
    Record record;
    record.stroke = stroke;
    if (stroke.insert != 0) {
        const auto it = m_inserts.constFind(stroke.insert);
        if (it == m_inserts.constEnd())
            return 0;
        record.stroke.page = it->page;
    }
    if (!record.stroke.isValid())
        return 0;

    record.id = m_nextId++;
    insertRecord(record);

    // Новое действие обрывает ветку повтора.
    m_redo.clear();
    m_undo.append({ActionKind::Add, {record}, {}});

    emit historyChanged();
    return record.id;
}

void InkDocument::eraseStrokes(const QList<quint64> &ids)
{
    QList<Record> erased;
    for (quint64 id : ids) {
        if (const Record *record = findRecord(id))
            erased.append(*record);
    }
    if (erased.isEmpty())
        return;

    for (const Record &record : erased)
        removeRecord(record.id);

    // Одно движение ластика - одна запись в истории, сколько бы штрихов оно
    // ни задело.
    m_redo.clear();
    m_undo.append({ActionKind::Erase, erased, {}});
    emit historyChanged();
}

const InkDocument::Record *InkDocument::findRecord(quint64 id) const
{
    // Константный operator[] у QHash возвращает копию: адрес брать только у
    // значения, найденного итератором.
    const QList<Record> *records = nullptr;
    if (const auto pageIt = m_pageOfStroke.constFind(id); pageIt != m_pageOfStroke.constEnd()) {
        records = &strokesOnPage(pageIt.value());
    } else if (const auto insertIt = m_insertOfStroke.constFind(id);
               insertIt != m_insertOfStroke.constEnd()) {
        records = &strokesInInsert(insertIt.value());
    }
    if (!records)
        return nullptr;
    for (const Record &record : *records) {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

bool InkDocument::insertRecord(const Record &record)
{
    const quint64 insert = record.stroke.insert;
    if (insert == 0) {
        m_byPage[record.stroke.page].append(record);
        m_pageOfStroke.insert(record.id, record.stroke.page);
        emit strokeAdded(record.id, record.stroke.page);
        emit pageChanged(record.stroke.page);
        emit changed();
        return true;
    }

    // Инвариант истории: запись применяется только к существующему вкладышу.
    // Вкладыш исчезает лишь через RemoveInsert, а стек - LIFO, так что
    // срабатывание здесь - ошибка в модели, а не в данных пользователя.
    if (!m_inserts.contains(insert)) {
        qWarning("InkDocument: штрих %llu ссылается на несуществующий вкладыш %llu",
                 qulonglong(record.id), qulonglong(insert));
        return false;
    }
    m_byInsert[insert].append(record);
    m_insertOfStroke.insert(record.id, insert);
    emit strokeAdded(record.id, record.stroke.page);
    emit insertInkChanged(insert);
    emit changed();
    return true;
}

void InkDocument::removeRecord(quint64 id)
{
    const auto removeFrom = [id](QList<Record> &records) {
        for (int i = 0; i < records.size(); ++i) {
            if (records[i].id == id) {
                records.removeAt(i);
                return;
            }
        }
    };

    if (const auto pageIt = m_pageOfStroke.constFind(id); pageIt != m_pageOfStroke.constEnd()) {
        const int page = pageIt.value();
        QList<Record> &records = m_byPage[page];
        removeFrom(records);
        if (records.isEmpty())
            m_byPage.remove(page);
        m_pageOfStroke.remove(id);

        emit strokeRemoved(id, page);
        emit pageChanged(page);
        emit changed();
        return;
    }

    const auto insertIt = m_insertOfStroke.constFind(id);
    if (insertIt == m_insertOfStroke.constEnd())
        return;
    const quint64 insert = insertIt.value();
    QList<Record> &records = m_byInsert[insert];
    removeFrom(records);
    if (records.isEmpty())
        m_byInsert.remove(insert);
    m_insertOfStroke.remove(id);

    emit strokeRemoved(id, m_inserts.value(insert).page);
    emit insertInkChanged(insert);
    emit changed();
}

void InkDocument::apply(const Action &action, bool forward)
{
    if (action.kind == ActionKind::RemoveInsert) {
        if (forward) {
            for (const Record &record : action.records)
                removeRecord(record.id);
            dropInsert(action.insert.id);
        } else {
            // Сначала вкладыш, потом его штрихи: штрих без вкладыша не встанет.
            restoreInsert(action.insert);
            for (const Record &record : action.records)
                insertRecord(record);
        }
        return;
    }

    const bool shouldInsert = (action.kind == ActionKind::Add) == forward;
    for (const Record &record : action.records) {
        if (shouldInsert)
            insertRecord(record);
        else
            removeRecord(record.id);
    }
}

void InkDocument::undo()
{
    if (m_undo.isEmpty())
        return;
    const Action action = m_undo.takeLast();
    apply(action, false);
    m_redo.append(action);
    emit historyChanged();
}

void InkDocument::redo()
{
    if (m_redo.isEmpty())
        return;
    const Action action = m_redo.takeLast();
    apply(action, true);
    m_undo.append(action);
    emit historyChanged();
}

void InkDocument::clear()
{
    m_byPage.clear();
    m_pageOfStroke.clear();
    m_inserts.clear();
    m_byInsert.clear();
    m_insertOfStroke.clear();
    m_undo.clear();
    m_redo.clear();
    m_nextId = 1;
    m_nextInsertId = 1;
    emit reloaded();
    emit historyChanged();
    emit changed();
}

const QList<InkDocument::Record> &InkDocument::strokesOnPage(int page) const
{
    const auto it = m_byPage.constFind(page);
    return it == m_byPage.constEnd() ? kNoStrokes : it.value();
}

QList<int> InkDocument::pagesWithInk() const
{
    QList<int> pages = m_byPage.keys();
    std::sort(pages.begin(), pages.end());
    return pages;
}

bool InkDocument::hasInkOnPage(int page) const
{
    return m_byPage.contains(page);
}

int InkDocument::strokeCount() const
{
    return int(m_pageOfStroke.size() + m_insertOfStroke.size());
}

QList<quint64> InkDocument::strokesNear(int page, const QPointF &pagePoint, qreal radius) const
{
    return strokesNearIn(strokesOnPage(page), pagePoint, radius);
}

// --- Вкладыши -----------------------------------------------------------------

quint64 InkDocument::addInsert(int page, const QRectF &rect)
{
    InkInsert insert;
    insert.id = m_nextInsertId;
    insert.page = page;
    insert.rect = rect;
    insert.open = true;
    if (!insert.isValid())
        return 0;

    ++m_nextInsertId;
    restoreInsert(insert);
    return insert.id;
}

bool InkDocument::setInsertRect(quint64 id, const QRectF &rect)
{
    const auto it = m_inserts.find(id);
    if (it == m_inserts.end() || rect.isEmpty())
        return false;
    if (it->rect == rect)
        return true;
    it->rect = rect;
    emit insertChanged(id, it->page);
    emit changed();
    return true;
}

bool InkDocument::setInsertOpen(quint64 id, bool open)
{
    const auto it = m_inserts.find(id);
    if (it == m_inserts.end())
        return false;
    if (it->open == open)
        return true;
    it->open = open;
    emit insertChanged(id, it->page);
    emit changed();
    return true;
}

void InkDocument::removeInsert(quint64 id)
{
    const auto it = m_inserts.constFind(id);
    if (it == m_inserts.constEnd())
        return;

    Action action;
    action.kind = ActionKind::RemoveInsert;
    action.insert = it.value();
    action.records = strokesInInsert(id);

    apply(action, true);
    m_redo.clear();
    m_undo.append(action);
    emit historyChanged();
}

void InkDocument::restoreInsert(const InkInsert &insert)
{
    m_inserts.insert(insert.id, insert);
    m_nextInsertId = qMax(m_nextInsertId, insert.id + 1);
    emit insertAdded(insert.id, insert.page);
    emit changed();
}

void InkDocument::dropInsert(quint64 id)
{
    const auto it = m_inserts.constFind(id);
    if (it == m_inserts.constEnd())
        return;
    const int page = it->page;
    // Штрихи снимаются до вкладыша вызывающим; здесь - только остатки, если
    // они почему-то есть, без записи в историю они бы потерялись молча.
    Q_ASSERT(!m_byInsert.contains(id));
    m_inserts.remove(id);
    emit insertRemoved(id, page);
    emit changed();
}

QList<InkInsert> InkDocument::insertsOnPage(int page) const
{
    QList<InkInsert> result;
    for (const InkInsert &insert : m_inserts) {
        if (insert.page == page)
            result.append(insert);
    }
    return result;
}

QList<InkInsert> InkDocument::allInserts() const
{
    return m_inserts.values();
}

quint64 InkDocument::insertAt(int page, const QPointF &pagePoint) const
{
    quint64 found = 0;
    for (const InkInsert &insert : m_inserts) {
        if (insert.page == page && insert.open && insert.rect.contains(pagePoint))
            found = insert.id; // порядок по возрастанию id: последний - верхний
    }
    return found;
}

const QList<InkDocument::Record> &InkDocument::strokesInInsert(quint64 insert) const
{
    const auto it = m_byInsert.constFind(insert);
    return it == m_byInsert.constEnd() ? kNoStrokes : it.value();
}

QList<InkDocument::Record> InkDocument::allInsertStrokes() const
{
    QList<Record> all;
    for (auto it = m_inserts.cbegin(); it != m_inserts.cend(); ++it)
        all.append(strokesInInsert(it.key()));
    return all;
}

QList<quint64> InkDocument::strokesNearInInsert(quint64 insert, const QPointF &localPoint,
                                                qreal radius) const
{
    return strokesNearIn(strokesInInsert(insert), localPoint, radius);
}

// -----------------------------------------------------------------------------

void InkDocument::loadStrokes(const QList<Record> &records, quint64 nextId,
                              const QList<InkInsert> &inserts, quint64 nextInsertId)
{
    m_byPage.clear();
    m_pageOfStroke.clear();
    m_inserts.clear();
    m_byInsert.clear();
    m_insertOfStroke.clear();
    m_undo.clear();
    m_redo.clear();

    for (const InkInsert &insert : inserts) {
        if (insert.isValid() && insert.id != 0)
            m_inserts.insert(insert.id, insert);
    }
    quint64 maxInsertId = 0;
    for (auto it = m_inserts.cbegin(); it != m_inserts.cend(); ++it)
        maxInsertId = qMax(maxInsertId, it.key());

    quint64 maxId = 0;
    for (Record record : records) {
        if (record.stroke.insert == 0) {
            m_byPage[record.stroke.page].append(record);
            m_pageOfStroke.insert(record.id, record.stroke.page);
        } else {
            const auto it = m_inserts.constFind(record.stroke.insert);
            if (it == m_inserts.constEnd())
                continue; // штрих потерянного вкладыша не показать - и не на странице
            record.stroke.page = it->page;
            m_byInsert[record.stroke.insert].append(record);
            m_insertOfStroke.insert(record.id, record.stroke.insert);
        }
        maxId = qMax(maxId, record.id);
    }

    m_nextId = std::max<quint64>({nextId, maxId + 1, 1});
    m_nextInsertId = std::max<quint64>({nextInsertId, maxInsertId + 1, 1});
    emit reloaded();
    emit historyChanged();
    emit changed();
}

QList<InkDocument::Record> InkDocument::allStrokes() const
{
    QList<Record> all;
    for (int page : pagesWithInk())
        all.append(strokesOnPage(page));
    return all;
}
