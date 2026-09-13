#include "outlinenavigator.h"

#include <QPdfBookmarkModel>

OutlineNavigator::OutlineNavigator(QObject *parent)
    : QObject(parent)
    , m_bookmarks(new QPdfBookmarkModel(this))
{
}

QAbstractItemModel *OutlineNavigator::model() const
{
    return m_bookmarks;
}

void OutlineNavigator::setDocument(PdfDocumentModel *document)
{
    if (m_document == document)
        return;
    if (m_document)
        m_document->disconnect(this);

    m_document = document;
    if (m_document)
        connect(m_document, &PdfDocumentModel::documentChanged, this, &OutlineNavigator::rebuild);

    rebuild();
}

void OutlineNavigator::rebuild()
{
    m_bookmarks->setDocument(m_document && m_document->isReady() ? m_document->document()
                                                                : nullptr);
    m_entries.clear();
    if (m_document && m_document->isReady())
        collect(QModelIndex(), 0);

    m_currentIndex = QModelIndex();
    updateCurrentIndex();
    emit documentChanged();
}

void OutlineNavigator::collect(const QModelIndex &parent, int level)
{
    const int rows = m_bookmarks->rowCount(parent);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = m_bookmarks->index(row, 0, parent);
        Entry entry;
        entry.index = index;
        entry.page = index.data(int(QPdfBookmarkModel::Role::Page)).toInt();
        entry.level = level;
        m_entries.append(entry);
        collect(index, level + 1);
    }
}

int OutlineNavigator::pageOf(const QModelIndex &index) const
{
    if (!index.isValid())
        return -1;
    return index.data(int(QPdfBookmarkModel::Role::Page)).toInt();
}

// Битые пункты в реальных документах встречаются: спайк 1.2 нашёл такой в
// учебнике, где закладка отдаёт страницу -1. Поэтому переход всегда проверяет
// цель по числу страниц документа.
bool OutlineNavigator::isReachable(const QModelIndex &index) const
{
    if (!m_document || !m_document->isReady())
        return false;
    const int page = pageOf(index);
    return page >= 0 && page < m_document->pageCount();
}

QString OutlineNavigator::titleOf(const QModelIndex &index) const
{
    return index.data(int(QPdfBookmarkModel::Role::Title)).toString();
}

bool OutlineNavigator::isCurrent(const QModelIndex &index) const
{
    return index.isValid() && index == m_currentIndex;
}

void OutlineNavigator::setCurrentPage(int page)
{
    if (m_currentPage == page)
        return;
    m_currentPage = page;
    updateCurrentIndex();
}

// Текущий раздел - самый глубокий пункт, который начинается не позже текущей
// страницы. Дерево обходится в порядке чтения, поэтому подходит последний
// подходящий пункт.
void OutlineNavigator::updateCurrentIndex()
{
    QModelIndex found;
    for (const Entry &entry : m_entries) {
        if (entry.page < 0 || entry.page > m_currentPage)
            continue;
        found = entry.index;
    }

    m_currentIndex = found;
    emit currentChanged();
}
