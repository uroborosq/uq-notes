#include "pagerenderservice.h"

#include <QPdfDocument>
#include <QPdfPageRenderer>

PageRenderService::PageRenderService(QObject *parent)
    : QObject(parent)
    , m_renderer(new QPdfPageRenderer(this))
{
    m_renderer->setRenderMode(QPdfPageRenderer::RenderMode::MultiThreaded);
    connect(m_renderer, &QPdfPageRenderer::pageRendered, this, &PageRenderService::onPageRendered);
}

PageRenderService::~PageRenderService() = default;

void PageRenderService::setDocument(QPdfDocument *document)
{
    if (m_document == document)
        return;
    cancelAll();
    m_document = document;
    m_renderer->setDocument(document);
}

void PageRenderService::request(int page, const QSize &pixelSize)
{
    if (!m_document || page < 0 || page >= m_document->pageCount())
        return;
    if (pixelSize.width() <= 0 || pixelSize.height() <= 0)
        return;

    const auto it = m_pending.constFind(page);
    if (it != m_pending.constEnd() && it->size == pixelSize)
        return; // ровно этот размер уже в работе

    Pending pending;
    pending.size = pixelSize;
    pending.requestId = m_renderer->requestPage(page, pixelSize, QPdfDocumentRenderOptions());
    m_pending.insert(page, pending);
}

void PageRenderService::cancel(int page)
{
    m_pending.remove(page);
}

void PageRenderService::cancelAll()
{
    m_pending.clear();
}

QSize PageRenderService::pendingSize(int page) const
{
    return m_pending.value(page).size;
}

void PageRenderService::onPageRendered(int page, QSize imageSize, const QImage &image,
                                       QPdfDocumentRenderOptions options, quint64 requestId)
{
    Q_UNUSED(options)

    const auto it = m_pending.constFind(page);
    // Результат отменённого или устаревшего запроса выбрасывается: во время
    // зума таких набирается много, и подмена текстуры старым размером дала бы
    // видимый откат резкости.
    if (it == m_pending.constEnd() || it->requestId != requestId)
        return;

    m_pending.erase(it);
    if (!image.isNull())
        emit rendered(page, imageSize, image);
}
