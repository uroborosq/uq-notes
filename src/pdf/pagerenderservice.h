#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPdfDocumentRenderOptions>
#include <QSize>

class QPdfDocument;
class QPdfPageRenderer;

// Асинхронный рендер страниц. Растеризация идёт на рабочем потоке
// (`QPdfPageRenderer` в режиме MultiThreaded), поэтому поток UI не растеризует
// ничего и кадр из-за рендера не удлиняется - требование из design.md.
//
// На страницу держится не больше одного незакрытого запроса: при быстром зуме
// запросы сыплются пачками, и без этого очередь росла бы быстрее, чем
// разгребается.
class PageRenderService : public QObject
{
    Q_OBJECT

public:
    explicit PageRenderService(QObject *parent = nullptr);
    ~PageRenderService() override;

    void setDocument(QPdfDocument *document);
    QPdfDocument *document() const { return m_document; }

    // Запрашивает страницу размером pixelSize. Повторный запрос того же
    // размера, который уже в работе, ничего не делает.
    void request(int page, const QSize &pixelSize);

    // Отменяет ожидание результата по странице: результат, если он всё-таки
    // придёт, будет отброшен.
    void cancel(int page);
    void cancelAll();

    // Размер, который сейчас рендерится для страницы, или пустой размер.
    QSize pendingSize(int page) const;
    int pendingCount() const { return int(m_pending.size()); }

signals:
    void rendered(int page, const QSize &pixelSize, const QImage &image);

private:
    void onPageRendered(int page, QSize imageSize, const QImage &image,
                        QPdfDocumentRenderOptions options, quint64 requestId);

    struct Pending
    {
        quint64 requestId = 0;
        QSize size;
    };

    QPdfDocument *m_document = nullptr;
    QPdfPageRenderer *m_renderer = nullptr;
    QHash<int, Pending> m_pending;
};
