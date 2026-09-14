#include "insertlistmodel.h"

#include <algorithm>

InsertListModel::InsertListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int InsertListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant InsertListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return QVariant();

    const Row &row = m_rows[index.row()];
    switch (role) {
    case InsertIdRole:
        return QVariant::fromValue(qulonglong(row.id));
    case PageRole:
        return row.page;
    case DocRectRole:
        return row.docRect;
    case OpenRole:
        return row.open;
    case StackRole:
        return row.stack;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> InsertListModel::roleNames() const
{
    return {
        {InsertIdRole, "insertId"}, {PageRole, "page"},   {DocRectRole, "docRect"},
        {OpenRole, "open"},         {StackRole, "stack"},
    };
}

void InsertListModel::sync(QList<Row> rows)
{
    const auto byId = [](const Row &a, const Row &b) { return a.id < b.id; };
    std::sort(rows.begin(), rows.end(), byId);

    const auto contains = [&rows, &byId](quint64 id) {
        Row probe;
        probe.id = id;
        const auto it = std::lower_bound(rows.cbegin(), rows.cend(), probe, byId);
        return it != rows.cend() && it->id == id;
    };

    // Исчезнувшие строки снимаются с конца, чтобы номера ещё не просмотренных
    // не съезжали.
    for (int i = int(m_rows.size()) - 1; i >= 0; --i) {
        if (contains(m_rows[i].id))
            continue;
        beginRemoveRows(QModelIndex(), i, i);
        m_rows.removeAt(i);
        endRemoveRows();
    }

    // Теперь старые строки - подмножество новых в том же порядке: всё, что не
    // совпало по id на своём месте, - новая строка.
    for (int i = 0; i < rows.size(); ++i) {
        if (i < m_rows.size() && m_rows[i].id == rows[i].id) {
            if (!(m_rows[i] == rows[i])) {
                m_rows[i] = rows[i];
                emit dataChanged(index(i), index(i));
            }
            continue;
        }
        beginInsertRows(QModelIndex(), i, i);
        m_rows.insert(i, rows[i]);
        endInsertRows();
    }
}
