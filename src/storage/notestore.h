#pragma once

#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QTimer>

#include "ink/inkdocument.h"

// Файл заметки `.uqn`: чернила, состояние просмотра и ссылка на исходный PDF.
//
// Внутри - SQLite в режиме WAL. Единый JSON пришлось бы переписывать целиком
// на каждое сохранение, а это пауза посреди письма; здесь автосохранение - это
// транзакция на несколько строк, и на длинном документе она стоит столько же,
// сколько на коротком (design.md, D8).
//
// Исходный PDF не трогается вовсе: заметка только ссылается на него, храня
// путь, размер и хэш, чтобы заметить подмену.
class NoteStore : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString notePath READ notePath NOTIFY stateChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY stateChanged)
    Q_PROPERTY(SourceState sourceState READ sourceState NOTIFY stateChanged)
    Q_PROPERTY(bool open READ isOpen NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ isDirty NOTIFY dirtyChanged)
    Q_PROPERTY(int autosaveIntervalMs READ autosaveIntervalMs WRITE setAutosaveIntervalMs
                   NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)
    // Состояние просмотра, разобранное по полям: структуру QML не увидит.
    Q_PROPERTY(bool hasViewState READ hasViewState NOTIFY stateChanged)
    Q_PROPERTY(int viewPage READ viewPage NOTIFY stateChanged)
    Q_PROPERTY(qreal viewZoom READ viewZoom NOTIFY stateChanged)
    Q_PROPERTY(int viewPageMode READ viewPageMode NOTIFY stateChanged)
    Q_PROPERTY(qreal viewOriginX READ viewOriginX NOTIFY stateChanged)
    Q_PROPERTY(qreal viewOriginY READ viewOriginY NOTIFY stateChanged)
    // Настройки инструмента живут рядом с состоянием просмотра: перо для
    // конспекта учебника и перо для пометок в статье - разные (design.md, D10).
    Q_PROPERTY(bool hasToolState READ hasToolState NOTIFY stateChanged)
    Q_PROPERTY(qreal toolWidth READ toolWidth NOTIFY stateChanged)
    Q_PROPERTY(QString toolColor READ toolColor NOTIFY stateChanged)

public:
    enum class SourceState {
        Unknown,
        Ok,      // файл на месте и совпадает с записанным
        Missing, // файла нет по сохранённому пути
        Changed, // файл есть, но это уже другая версия
    };
    Q_ENUM(SourceState)

    struct ViewState
    {
        int page = 0;
        qreal zoom = 1.0;
        int pageMode = 0;
        qreal originX = 0.0;
        qreal originY = 0.0;
        bool valid = false;
    };

    explicit NoteStore(QObject *parent = nullptr);
    ~NoteStore() override;

    // Создаёт новый файл заметки рядом с PDF. Существующий файл не трогается.
    Q_INVOKABLE bool create(const QString &notePath, const QString &pdfPath);
    Q_INVOKABLE bool openNote(const QString &notePath);
    // Открывает заметку, если она есть, и создаёт, если нет.
    Q_INVOKABLE bool openOrCreate(const QString &notePath, const QString &pdfPath);
    Q_INVOKABLE void close();

    // Путь заметки по умолчанию для данного PDF: сосед с расширением .uqn.
    Q_INVOKABLE static QString notePathFor(const QString &pdfPath);

    bool isOpen() const { return m_database.isOpen(); }
    QString notePath() const { return m_notePath; }
    QString sourcePath() const { return m_sourcePath; }
    qint64 sourceSize() const { return m_sourceSize; }
    QString sourceHash() const { return m_sourceHash; }
    QString errorString() const { return m_errorString; }

    // Проверяет, на месте ли исходный PDF и тот ли он.
    Q_INVOKABLE SourceState sourceState() const;
    // Переподключает заметку к другому файлу PDF, сохраняя чернила.
    Q_INVOKABLE bool relinkSource(const QString &pdfPath);

    // Подписывает хранилище на изменения чернил: дальше запись идёт сама.
    Q_INVOKABLE void attach(InkDocument *ink);
    Q_INVOKABLE bool load(InkDocument *ink);

    bool isDirty() const
    {
        return !m_pendingStrokes.isEmpty() || m_viewStateDirty || m_toolDirty;
    }
    // Записывает накопленные изменения одной транзакцией.
    Q_INVOKABLE bool flush();

    int autosaveIntervalMs() const { return m_autosave.interval(); }
    void setAutosaveIntervalMs(int intervalMs);

    ViewState viewState() const;
    bool hasViewState() const { return m_viewState.valid; }
    int viewPage() const { return m_viewState.page; }
    qreal viewZoom() const { return m_viewState.zoom; }
    int viewPageMode() const { return m_viewState.pageMode; }
    qreal viewOriginX() const { return m_viewState.originX; }
    qreal viewOriginY() const { return m_viewState.originY; }
    void setViewState(const ViewState &state);
    Q_INVOKABLE void rememberView(int page, qreal zoom, int pageMode, qreal originX,
                                  qreal originY);
    // Толщина - в точках страницы, цвет - строкой "#aarrggbb".
    Q_INVOKABLE void rememberTool(qreal width, const QString &color);

    bool hasToolState() const { return m_toolValid; }
    qreal toolWidth() const { return m_toolWidth; }
    QString toolColor() const { return m_toolColor; }

    static QString hashOfFile(const QString &path);

signals:
    void stateChanged();
    void dirtyChanged();
    void saved();

private:
    bool initializeSchema();
    bool setMeta(const QString &key, const QString &value);
    QString meta(const QString &key) const;
    bool writeStroke(const InkDocument::Record &record);
    void markDirty();

    QSqlDatabase m_database;
    QString m_connectionName;
    QString m_notePath;
    QString m_sourcePath;
    qint64 m_sourceSize = 0;
    QString m_sourceHash;
    QString m_errorString;

    InkDocument *m_ink = nullptr;
    // Штрихи, чья запись в файле разошлась с моделью. Что именно с ними делать -
    // вписать или удалить - решается при сохранении по текущему состоянию
    // модели: так отмена и повтор не могут оставить в файле лишнюю строку.
    QSet<quint64> m_pendingStrokes;
    ViewState m_viewState;
    bool m_viewStateDirty = false;

    qreal m_toolWidth = 0.0;
    QString m_toolColor;
    bool m_toolValid = false;
    bool m_toolDirty = false;

    QTimer m_autosave;
};
