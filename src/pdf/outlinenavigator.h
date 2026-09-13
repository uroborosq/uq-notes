#pragma once

#include <QAbstractItemModel>
#include <QList>
#include <QModelIndex>
#include <QObject>
#include <QQmlEngine>

#include "pdf/pdfdocumentmodel.h"

class QPdfBookmarkModel;

// Оглавление документа. Дерево берётся из закладок PDF как есть
// (`QPdfBookmarkModel` - уже готовая `QAbstractItemModel`, её и отдаём в
// TreeView), а сверху добавляется то, чего в ней нет: отражение текущей
// позиции просмотра и проверка, что пункт вообще куда-то ведёт.
class OutlineNavigator : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PdfDocumentModel *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(QAbstractItemModel *model READ model NOTIFY documentChanged)
    Q_PROPERTY(bool hasOutline READ hasOutline NOTIFY documentChanged)
    Q_PROPERTY(int entryCount READ entryCount NOTIFY documentChanged)
    Q_PROPERTY(int currentPage READ currentPage WRITE setCurrentPage NOTIFY currentChanged)
    Q_PROPERTY(QModelIndex currentIndex READ currentIndex NOTIFY currentChanged)

public:
    explicit OutlineNavigator(QObject *parent = nullptr);

    PdfDocumentModel *document() const { return m_document; }
    void setDocument(PdfDocumentModel *document);

    QAbstractItemModel *model() const;
    bool hasOutline() const { return !m_entries.isEmpty(); }
    int entryCount() const { return int(m_entries.size()); }

    int currentPage() const { return m_currentPage; }
    void setCurrentPage(int page);
    QModelIndex currentIndex() const { return m_currentIndex; }

    // Страница, на которую указывает пункт. -1, если ссылка никуда не ведёт.
    Q_INVOKABLE int pageOf(const QModelIndex &index) const;
    // Ведёт ли пункт на существующую страницу - сценарий «битая ссылка».
    Q_INVOKABLE bool isReachable(const QModelIndex &index) const;
    Q_INVOKABLE QString titleOf(const QModelIndex &index) const;
    // Сравнение индексов делается здесь: в QML QModelIndex сравнивать нечем.
    Q_INVOKABLE bool isCurrent(const QModelIndex &index) const;

signals:
    void documentChanged();
    void currentChanged();

private:
    struct Entry
    {
        QPersistentModelIndex index;
        int page = -1;
        int level = 0;
    };

    void rebuild();
    void collect(const QModelIndex &parent, int level);
    void updateCurrentIndex();

    PdfDocumentModel *m_document = nullptr;
    QPdfBookmarkModel *m_bookmarks = nullptr;
    QList<Entry> m_entries; // в порядке обхода дерева
    int m_currentPage = 0;
    QModelIndex m_currentIndex;
};
