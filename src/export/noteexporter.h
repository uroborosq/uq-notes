#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QSizeF>
#include <QString>

#include <functional>

#include "ink/inkdocument.h"
#include "pdf/pdfdocumentmodel.h"

// Экспорт заметки в плоский PDF.
//
// Чернила рисуются в отдельный overlay-PDF того же размера через QPdfWriter -
// векторным выводом QPainter, каждый штрих отдельной заливкой замкнутого
// контура, - а потом накладываются на оригинал через libqpdf. Оригинал при
// этом копируется целиком, поэтому текстовый слой, закладки и метаданные
// остаются на месте (design.md, D6).
//
// Работа идёт на рабочем потоке: на документе в 200 страниц она занимает
// секунды, и держать всё это время UI замороженным нельзя.
class NoteExporter : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ isBusy NOTIFY stateChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY progressChanged)

public:
    explicit NoteExporter(QObject *parent = nullptr);
    ~NoteExporter() override;

    bool isBusy() const { return m_busy; }
    qreal progress() const { return m_progress; }
    QString statusText() const { return m_statusText; }

    // Запускает экспорт. Возвращает false, если экспортировать нечего или
    // экспорт уже идёт.
    Q_INVOKABLE bool exportToPdf(PdfDocumentModel *document, InkDocument *ink,
                                 const QString &rawOutputPath);

    // Дописывает .pdf, если его нет. Диалог сохранения не всегда подставляет
    // расширение сам: имя, введённое без точки, так и уходит без неё, и на
    // выходе получается файл, который система не опознаёт.
    Q_INVOKABLE static QString withPdfSuffix(const QString &path);

    // Синхронный вариант - им пользуются тесты и рабочий поток.
    // displayedPageSizes - размеры страниц с уже применённым поворотом, то есть
    // ровно те, в которых хранятся чернила.
    static bool exportNow(const QString &sourcePdf, const QList<QSizeF> &displayedPageSizes,
                          const QList<int> &pageRotations,
                          const QList<InkDocument::Record> &strokes, const QString &outputPath,
                          QString *errorMessage = nullptr,
                          const std::function<void(int, int)> &onPage = {});

signals:
    void stateChanged();
    void progressChanged();
    void finished(bool ok, const QString &message);

private:
    void setBusy(bool busy);

    bool m_busy = false;
    qreal m_progress = 0.0;
    QString m_statusText;
    QFutureWatcher<bool> m_watcher;
    QString m_lastError;
};
