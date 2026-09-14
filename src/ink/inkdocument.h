#pragma once

#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QQmlEngine>

#include "ink/inktypes.h"

// Модель чернил: штрихи по страницам, вкладыши со своими штрихами плюс отмена
// и повтор.
//
// Чернила хранятся в координатах страницы, поэтому смена масштаба и режима
// отображения их не касается вовсе - это требование «Привязка чернил к
// странице» выполняется тем, что здесь просто нет экранных координат.
//
// Гранулярность отмены - один штрих или одно действие ластика, как требует
// ink-input: поэтому стирание нескольких штрихов одним движением ластика
// кладётся в стек как одна запись.
//
// Вкладыши живут здесь же, а не в отдельной модели: история отмены у страницы
// и вкладышей одна (design.md, D1). Штрихи вкладышей лежат отдельно от штрихов
// страницы, поэтому strokesOnPage() и allStrokes() их не видят - ни геометрия
// страниц, ни экспорт о вкладышах не знают.
class InkDocument : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(int strokeCount READ strokeCount NOTIFY changed)
    Q_PROPERTY(int insertCount READ insertCount NOTIFY changed)

public:
    struct Record
    {
        quint64 id = 0;
        Stroke stroke;
    };

    explicit InkDocument(QObject *parent = nullptr);

    // Добавляет завершённый штрих. Возвращает его идентификатор, 0 при отказе.
    // Штрих вкладыша (stroke.insert != 0) принимается, только если вкладыш
    // существует, и получает страницу вкладыша.
    quint64 addStroke(const Stroke &stroke);
    // Стирает набор штрихов как одно действие ластика.
    void eraseStrokes(const QList<quint64> &ids);

    const QList<Record> &strokesOnPage(int page) const;
    QList<int> pagesWithInk() const;
    // Все штрихи модели: и страниц, и вкладышей.
    int strokeCount() const;
    bool hasInkOnPage(int page) const;

    // Штрихи страницы, задетые кругом ластика, в координатах страницы.
    QList<quint64> strokesNear(int page, const QPointF &pagePoint, qreal radius) const;

    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void clear();

    // --- Вкладыши ---------------------------------------------------------

    // Создание, размер и раскрытие вкладыша в историю не идут и ветку повтора
    // не обрывают: это не правка содержимого (design.md, D7).
    quint64 addInsert(int page, const QRectF &rect);
    bool setInsertRect(quint64 id, const QRectF &rect);
    bool setInsertOpen(quint64 id, bool open);
    // Удаление вкладыша вместе со штрихами - одно действие в истории.
    void removeInsert(quint64 id);

    bool hasInsert(quint64 id) const { return m_inserts.contains(id); }
    InkInsert insertById(quint64 id) const { return m_inserts.value(id); }
    int insertCount() const { return int(m_inserts.size()); }
    // Вкладыши страницы в порядке идентификаторов.
    QList<InkInsert> insertsOnPage(int page) const;
    QList<InkInsert> allInserts() const;
    // Верхний (последний по идентификатору) раскрытый вкладыш страницы, рамка
    // которого содержит точку; 0 - такого нет.
    quint64 insertAt(int page, const QPointF &pagePoint) const;

    const QList<Record> &strokesInInsert(quint64 insert) const;
    QList<Record> allInsertStrokes() const;
    // Штрихи вкладыша, задетые кругом ластика, в локальных координатах вкладыша.
    QList<quint64> strokesNearInInsert(quint64 insert, const QPointF &localPoint,
                                       qreal radius) const;

    // Загрузка из файла заметки идёт мимо истории: открытие заметки - не
    // действие пользователя, отменять там нечего. Штрихи вкладышей приходят в
    // том же списке и отличаются полем stroke.insert.
    void loadStrokes(const QList<Record> &records, quint64 nextId,
                     const QList<InkInsert> &inserts = {}, quint64 nextInsertId = 1);
    // Штрихи страниц, без штрихов вкладышей.
    QList<Record> allStrokes() const;
    quint64 nextId() const { return m_nextId; }
    quint64 nextInsertId() const { return m_nextInsertId; }

signals:
    void changed();
    // Чернила сменились целиком - загрузили другую заметку или очистили.
    // Постраничных уведомлений тут не хватает: измениться могли все страницы
    // сразу, включая те, о которых слушатель уже забыл.
    void reloaded();
    // Изменились чернила страницы - не вкладышей на ней.
    void pageChanged(int page);
    void historyChanged();
    // Штрихи, добавленные и удалённые с момента последнего сохранения:
    // на этом стоит инкрементальная запись в файл заметки. Шлются и для
    // штрихов вкладышей.
    void strokeAdded(quint64 id, int page);
    void strokeRemoved(quint64 id, int page);

    void insertAdded(quint64 id, int page);
    // Изменились рамка или состояние «раскрыт».
    void insertChanged(quint64 id, int page);
    void insertRemoved(quint64 id, int page);
    // Изменился состав штрихов вкладыша.
    void insertInkChanged(quint64 id);

private:
    enum class ActionKind { Add, Erase, RemoveInsert };

    struct Action
    {
        ActionKind kind = ActionKind::Add;
        QList<Record> records; // затронутые штрихи целиком, чтобы вернуть их как были
        InkInsert insert;      // удалённый вкладыш - только для RemoveInsert
    };

    // Возвращает false, если штрих ссылается на несуществующий вкладыш.
    bool insertRecord(const Record &record);
    void removeRecord(quint64 id);
    const Record *findRecord(quint64 id) const;
    void restoreInsert(const InkInsert &insert);
    void dropInsert(quint64 id);
    void apply(const Action &action, bool forward);

    QHash<int, QList<Record>> m_byPage;
    QHash<quint64, int> m_pageOfStroke;
    // Упорядочено по идентификатору: порядок вкладышей после загрузки и в
    // выборках детерминирован.
    QMap<quint64, InkInsert> m_inserts;
    QHash<quint64, QList<Record>> m_byInsert;
    QHash<quint64, quint64> m_insertOfStroke;
    QList<Action> m_undo;
    QList<Action> m_redo;
    quint64 m_nextId = 1;
    quint64 m_nextInsertId = 1;
};
