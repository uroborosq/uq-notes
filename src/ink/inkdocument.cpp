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
} // namespace

InkDocument::InkDocument(QObject *parent)
    : QObject(parent)
{
}

quint64 InkDocument::addStroke(const Stroke &stroke)
{
    if (!stroke.isValid())
        return 0;

    Record record;
    record.id = m_nextId++;
    record.stroke = stroke;

    insert(record);

    // Новое действие обрывает ветку повтора.
    m_redo.clear();
    m_undo.append({ActionKind::Add, {record}});

    emit historyChanged();
    return record.id;
}

void InkDocument::eraseStrokes(const QList<quint64> &ids)
{
    QList<Record> erased;
    for (quint64 id : ids) {
        const auto pageIt = m_pageOfStroke.constFind(id);
        if (pageIt == m_pageOfStroke.constEnd())
            continue;
        const QList<Record> &records = m_byPage[pageIt.value()];
        for (const Record &record : records) {
            if (record.id == id) {
                erased.append(record);
                break;
            }
        }
    }
    if (erased.isEmpty())
        return;

    for (const Record &record : erased)
        remove(record.id);

    // Одно движение ластика - одна запись в истории, сколько бы штрихов оно
    // ни задело.
    m_redo.clear();
    m_undo.append({ActionKind::Erase, erased});
    emit historyChanged();
}

void InkDocument::insert(const Record &record)
{
    m_byPage[record.stroke.page].append(record);
    m_pageOfStroke.insert(record.id, record.stroke.page);
    emit strokeAdded(record.id, record.stroke.page);
    emit pageChanged(record.stroke.page);
    emit changed();
}

void InkDocument::remove(quint64 id)
{
    const auto pageIt = m_pageOfStroke.constFind(id);
    if (pageIt == m_pageOfStroke.constEnd())
        return;
    const int page = pageIt.value();

    QList<Record> &records = m_byPage[page];
    for (int i = 0; i < records.size(); ++i) {
        if (records[i].id == id) {
            records.removeAt(i);
            break;
        }
    }
    if (records.isEmpty())
        m_byPage.remove(page);
    m_pageOfStroke.remove(id);

    emit strokeRemoved(id, page);
    emit pageChanged(page);
    emit changed();
}

void InkDocument::apply(const Action &action, bool forward)
{
    const bool shouldInsert = (action.kind == ActionKind::Add) == forward;
    for (const Record &record : action.records) {
        if (shouldInsert)
            insert(record);
        else
            remove(record.id);
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
    m_undo.clear();
    m_redo.clear();
    m_nextId = 1;
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
    return int(m_pageOfStroke.size());
}

QList<quint64> InkDocument::strokesNear(int page, const QPointF &pagePoint, qreal radius) const
{
    QList<quint64> hits;
    for (const Record &record : strokesOnPage(page)) {
        // Радиус ластика плюс полутолщина штриха: касание края линии тоже
        // считается попаданием.
        const qreal reach = radius + record.stroke.width * 0.5;
        const QList<InkPoint> &points = record.stroke.points;
        for (int i = 0; i + 1 < points.size(); ++i) {
            if (distanceToSegment(pagePoint, points[i].position, points[i + 1].position) <= reach) {
                hits.append(record.id);
                break;
            }
        }
    }
    return hits;
}

void InkDocument::loadStrokes(const QList<Record> &records, quint64 nextId)
{
    m_byPage.clear();
    m_pageOfStroke.clear();
    m_undo.clear();
    m_redo.clear();
    for (const Record &record : records) {
        m_byPage[record.stroke.page].append(record);
        m_pageOfStroke.insert(record.id, record.stroke.page);
    }
    m_nextId = qMax<quint64>(nextId, 1);
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
