#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QRectF>

// Вкладыши зоны удержания для элементов управления поверх холста: значков,
// кнопок и ручки размера (design.md, D5).
//
// Рамка отдаётся в координатах документа, а не экрана: экранное положение QML
// считает привязками к масштабу и сдвигу холста. Поэтому зум и панорама модель
// не трогают вовсе, и делегаты не пересоздаются на каждом кадре жеста.
class InsertListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        InsertIdRole = Qt::UserRole + 1,
        PageRole,
        DocRectRole,
        OpenRole,
        StackRole,
    };

    struct Row
    {
        quint64 id = 0;
        int page = -1;
        QRectF docRect; // рамка в координатах документа
        bool open = true;
        int stack = 0;  // место в порядке перекрытия: больше - выше

        bool operator==(const Row &other) const
        {
            return id == other.id && page == other.page && docRect == other.docRect
                && open == other.open && stack == other.stack;
        }
    };

    explicit InsertListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Приводит модель к новому набору строк: удаляет исчезнувшие, вставляет
    // появившиеся и шлёт dataChanged только по строкам, которые действительно
    // изменились. Сброса модели не бывает - делегаты под пером не
    // пересоздаются.
    void sync(QList<Row> rows);
    const QList<Row> &rows() const { return m_rows; }

private:
    QList<Row> m_rows; // по возрастанию id
};
