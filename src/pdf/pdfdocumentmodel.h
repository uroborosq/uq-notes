#pragma once

#include <QHash>
#include <QObject>
#include <QPdfDocument>
#include <QQmlEngine>
#include <QSizeF>
#include <QString>
#include <QUrl>

// Документ-подложка заметки: открытие PDF, размеры и поворот страниц,
// внятные ошибки. Держит `QPdfDocument` внутри, потому что вокруг него нужен
// слой, которого в нём нет: поворот страницы (`/Rotate` берётся через qpdf) и
// разбор ошибок в сообщения, пригодные для показа пользователю.
//
// Состояние открытого документа и результат последней попытки открытия
// разделены намеренно: по сценарию «повреждённый или зашифрованный файл»
// неудачное открытие обязано оставить приложение работоспособным, то есть не
// трогать уже открытый документ.
class PdfDocumentModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString source READ source NOTIFY documentChanged)
    Q_PROPERTY(State state READ state NOTIFY documentChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY documentChanged)
    Q_PROPERTY(bool ready READ isReady NOTIFY documentChanged)
    Q_PROPERTY(OpenResult lastOpenResult READ lastOpenResult NOTIFY lastOpenResultChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY lastOpenResultChanged)
    Q_PROPERTY(QString attemptedSource READ attemptedSource NOTIFY lastOpenResultChanged)
    Q_PROPERTY(QUrl suggestedExportUrl READ suggestedExportUrl NOTIFY documentChanged)
    Q_PROPERTY(QUrl folderUrl READ folderUrl NOTIFY documentChanged)

public:
    enum class State {
        Empty, // ничего не открыто
        Ready, // документ открыт и готов
    };
    Q_ENUM(State)

    enum class OpenResult {
        None,
        Success,
        FileNotFound,
        InvalidFormat,
        PasswordRequired,
        UnsupportedEncryption,
        Unknown,
    };
    Q_ENUM(OpenResult)

    explicit PdfDocumentModel(QObject *parent = nullptr);
    ~PdfDocumentModel() override;

    // Возвращает true при успешном открытии. При неудаче ранее открытый
    // документ остаётся открытым, а причина ложится в lastOpenResult.
    Q_INVOKABLE bool open(const QString &path, const QString &password = QString());
    Q_INVOKABLE void close();

    // Путь к файлу из URL, который отдают диалоги QtQuick. Через строковую
    // замену "file://" это делать нельзя: URL процентно-закодирован, и путь с
    // кириллицей или пробелами превращается в нечитаемый.
    Q_INVOKABLE static QString toLocalPath(const QUrl &url);

    // Куда предложить сохранить экспорт: рядом с исходником, с пометкой в имени.
    QUrl suggestedExportUrl() const;
    QUrl folderUrl() const;

    // Размер страницы в точках, с уже применённым поворотом - то есть тот
    // прямоугольник, который страница занимает на экране.
    Q_INVOKABLE QSizeF pageSize(int page) const;
    // Поворот страницы из PDF: 0, 90, 180 или 270 градусов по часовой стрелке.
    Q_INVOKABLE int pageRotation(int page) const;

    QString source() const { return m_source; }
    State state() const { return m_state; }
    int pageCount() const;
    bool isReady() const { return m_state == State::Ready; }

    OpenResult lastOpenResult() const { return m_lastOpenResult; }
    QString errorString() const { return m_errorString; }
    QString attemptedSource() const { return m_attemptedSource; }

    QPdfDocument *document() { return m_document; }
    const QPdfDocument *document() const { return m_document; }

signals:
    void documentChanged();
    void lastOpenResultChanged();

private:
    void loadRotations(const QString &path, const QString &password);
    bool fail(OpenResult result, const QString &path);

    QPdfDocument *m_document = nullptr;
    QString m_source;
    QString m_password;
    State m_state = State::Empty;

    OpenResult m_lastOpenResult = OpenResult::None;
    QString m_errorString;
    QString m_attemptedSource;

    QHash<int, int> m_rotations;
};
