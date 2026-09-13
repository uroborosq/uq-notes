#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QQmlEngine>

#include "ink/inktypes.h"

// Модель чернил: штрихи по страницам плюс отмена и повтор.
//
// Чернила хранятся в координатах страницы, поэтому смена масштаба и режима
// отображения их не касается вовсе - это требование «Привязка чернил к
// странице» выполняется тем, что здесь просто нет экранных координат.
//
// Гранулярность отмены - один штрих или одно действие ластика, как требует
// ink-input: поэтому стирание нескольких штрихов одним движением ластика
// кладётся в стек как одна запись.
class InkDocument : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(int strokeCount READ strokeCount NOTIFY changed)

public:
    struct Record
    {
        quint64 id = 0;
        Stroke stroke;
    };

    explicit InkDocument(QObject *parent = nullptr);

    // Добавляет завершённый штрих. Возвращает его идентификатор, 0 при отказе.
    quint64 addStroke(const Stroke &stroke);
    // Стирает набор штрихов как одно действие ластика.
    void eraseStrokes(const QList<quint64> &ids);

    const QList<Record> &strokesOnPage(int page) const;
    QList<int> pagesWithInk() const;
    int strokeCount() const;
    bool hasInkOnPage(int page) const;

    // Штрихи, задетые кругом ластика, в координатах страницы.
    QList<quint64> strokesNear(int page, const QPointF &pagePoint, qreal radius) const;

    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void clear();

    // Загрузка из файла заметки идёт мимо истории: открытие заметки - не
    // действие пользователя, отменять там нечего.
    void loadStrokes(const QList<Record> &records, quint64 nextId);
    QList<Record> allStrokes() const;
    quint64 nextId() const { return m_nextId; }

signals:
    void changed();
    // Чернила сменились целиком - загрузили другую заметку или очистили.
    // Постраничных уведомлений тут не хватает: измениться могли все страницы
    // сразу, включая те, о которых слушатель уже забыл.
    void reloaded();
    void pageChanged(int page);
    void historyChanged();
    // Штрихи, добавленные и удалённые с момента последнего сохранения:
    // на этом стоит инкрементальная запись в файл заметки.
    void strokeAdded(quint64 id, int page);
    void strokeRemoved(quint64 id, int page);

private:
    enum class ActionKind { Add, Erase };

    struct Action
    {
        ActionKind kind = ActionKind::Add;
        QList<Record> records; // затронутые штрихи целиком, чтобы вернуть их как были
    };

    void insert(const Record &record);
    void remove(quint64 id);
    void apply(const Action &action, bool forward);

    QHash<int, QList<Record>> m_byPage;
    QHash<quint64, int> m_pageOfStroke;
    QList<Action> m_undo;
    QList<Action> m_redo;
    quint64 m_nextId = 1;
};
