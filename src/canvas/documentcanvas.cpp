#include "documentcanvas.h"

#include <QQuickWindow>

#include <algorithm>
#include <cmath>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QSGTransformNode>

#include "pdf/pagerenderservice.h"
#include "pdf/pdfdocumentmodel.h"
#include "perf/framestats.h"

namespace {

// Задержка после последнего изменения масштаба, по истечении которой страницы
// перерисовываются под новое разрешение.
constexpr int kLodSettleMs = 120;

// Инерция: экспоненциальное затухание с постоянной времени kFlickTau и
// остановкой на пороге kFlickStopSpeed. Значения подобраны так, чтобы бросок
// проходил примерно экран за полсекунды и вставал без рывка.
constexpr qreal kFlickTau = 0.32;
constexpr qreal kFlickStopSpeed = 24.0;   // пикселей в секунду
constexpr qreal kFlickMaxSpeed = 6000.0;  // пикселей в секунду
constexpr int kFlickIntervalMs = 4;

inline QColor placeholderColor() { return QColor(238, 238, 240); }

// Ограничение на размер текстуры страницы: при 800% страница A4 дала бы
// 4760x6735 пикселей, что и в память не влезает, и не нужно.
constexpr int kMaxTextureEdge = 4096;

// Сколько страниц за кадр разрешено загружать в текстуры и пересобирать по
// чернилам. Загрузка большой текстуры стоит единицы миллисекунд, и при быстрой
// прокрутке несколько таких загрузок в одном кадре складываются в пропуск
// кадра. Остальное переносится на следующие кадры: заполнитель на лишний кадр
// незаметен, пропущенный кадр - заметен.
constexpr int kMaxTextureUploadsPerFrame = 1;
constexpr int kMaxInkRebuildsPerFrame = 1;
// Уничтожение узла страницы уносит с собой её текстуру, а освобождение текстуры
// на драйвере может подождать кадр, если она ещё в работе. При быстрой
// прокрутке страницы уходят пачками, и несколько таких освобождений в одном
// кадре складываются в пропуск кадра - поэтому их тоже приходится растягивать.
constexpr int kMaxNodeRemovalsPerFrame = 1;

// Пока вид движется, страницы растеризуются в уменьшенном разрешении. Растр
// страницы считает pdfium на рабочем потоке, и его работа конкурирует за
// процессор с потоком рендера; при кадровом бюджете в 8.3 мс этого хватает,
// чтобы изредка пропустить кадр. Уменьшенная страница на лету не видна - её
// всё равно тут же сменит резкая, - а стоит она вчетверо дешевле.
constexpr qreal kMovingResolutionScale = 0.5;

} // namespace

// Узел страницы: собственная система координат страницы, заполнитель под
// текстурой и сама текстура. Индекс страницы хранится в узле, чтобы карта
// «страница -> узел» переживала пересоздание графа сцены.
class PageSceneNode : public QSGTransformNode
{
public:
    // Слои страницы снизу вверх. Порядок детей узла - это и есть порядок
    // отрисовки: более поздний брат оказывается ближе к зрителю. Появляются же
    // дети вразнобой - растр приходит с рабочего потока когда придётся, чернила
    // собираются, когда страница вернулась в вид, - поэтому место ребёнка
    // задаётся слоем, а не тем, что успело появиться раньше.
    enum Layer {
        PlaceholderLayer,
        RasterLayer,
        DryInkLayer,
        WetInkLayer,
    };

    explicit PageSceneNode(int page)
        : page(page)
    {
    }

    // Включает ребёнка в узел на место его слоя.
    void addToLayer(QSGNode *child, Layer layer)
    {
        for (QSGNode *sibling = firstChild(); sibling; sibling = sibling->nextSibling()) {
            if (layerOf(sibling) > layer) {
                insertChildNodeBefore(child, sibling);
                return;
            }
        }
        appendChildNode(child);
    }

    int page = -1;
    QSGSimpleRectNode *placeholder = nullptr;
    QSGSimpleTextureNode *texture = nullptr;

    // Сухие чернила: узел на штрих, чтобы у каждого был свой цвет.
    QSGNode *dryInk = nullptr;
    QList<quint64> dryInkIds;

    // Мокрый штрих разложен на куски постоянного размера: заполненный кусок
    // больше не трогается, а на кадр перезаливается только последний,
    // незаполненный. Так объём работы на кадр не зависит от длины штриха.
    QSGNode *wetInk = nullptr;
    QList<QSGGeometryNode *> wetChunks;
    int wetVertexCount = 0;

private:
    Layer layerOf(const QSGNode *child) const
    {
        if (child == texture)
            return RasterLayer;
        if (child == dryInk)
            return DryInkLayer;
        if (child == wetInk)
            return WetInkLayer;
        return PlaceholderLayer; // заполнитель и всё неопознанное - в самый низ
    }
};

namespace {

// Размер куска мокрого штриха в вершинах и перекрытие между кусками.
// Перекрытие в две вершины сохраняет непрерывность треугольной полосы и её
// чётность на границе куска.
constexpr int kWetChunkVertices = 512;
constexpr int kWetChunkOverlap = 2;

QSGGeometryNode *makeStripNode(const QColor &color)
{
    auto *node = new QSGGeometryNode;

    auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry, true);

    auto *material = new QSGFlatColorMaterial;
    material->setColor(color);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial, true);

    return node;
}

void uploadStrip(QSGGeometryNode *node, const StrokeTessellator::Vertex *source, int count)
{
    QSGGeometry *geometry = node->geometry();
    geometry->allocate(count);
    auto *target = geometry->vertexDataAsPoint2D();
    for (int i = 0; i < count; ++i) {
        target[i].x = source[i].x;
        target[i].y = source[i].y;
    }
    node->markDirty(QSGNode::DirtyGeometry);
}

int wetChunkStart(int index)
{
    return index * (kWetChunkVertices - kWetChunkOverlap);
}

} // namespace

DocumentCanvas::DocumentCanvas(QQuickItem *parent)
    : QQuickItem(parent)
    , m_renderer(new PageRenderService(this))
{
    setFlag(ItemHasContents, true);
    setClip(true);
    // Событиям пера и мыши нужен принимающий элемент; касания сюда не
    // приходят намеренно - ими занимаются жестовые обработчики в QML.
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);

    connect(m_renderer, &PageRenderService::rendered, this, &DocumentCanvas::onPageRendered);

    m_lodTimer.setSingleShot(true);
    m_lodTimer.setInterval(kLodSettleMs);
    connect(&m_lodTimer, &QTimer::timeout, this, [this] { requestRenders(true); });

    m_flickTimer.setInterval(kFlickIntervalMs);
    connect(&m_flickTimer, &QTimer::timeout, this, &DocumentCanvas::stepFlick);
}

DocumentCanvas::~DocumentCanvas() = default;

void DocumentCanvas::setDocument(PdfDocumentModel *document)
{
    if (m_document == document)
        return;
    if (m_document)
        m_document->disconnect(this);

    m_document = document;

    if (m_document)
        connect(m_document, &PdfDocumentModel::documentChanged, this,
                &DocumentCanvas::rebuildLayout);

    rebuildLayout();
    emit documentChanged();
}

void DocumentCanvas::setFrameStats(FrameStats *stats)
{
    if (m_frameStats == stats)
        return;
    m_frameStats = stats;
    emit frameStatsChanged();
}

int DocumentCanvas::liveTextureCount() const
{
    int count = 0;
    for (auto it = m_pages.cbegin(); it != m_pages.cend(); ++it) {
        if (!it->textureSize.isEmpty())
            ++count;
    }
    return count;
}

int DocumentCanvas::placeholderPageCount() const
{
    int count = 0;
    for (int page : m_visiblePages) {
        const auto it = m_pages.constFind(page);
        if (it == m_pages.constEnd() || it->textureSize.isEmpty())
            ++count;
    }
    return count;
}

QSize DocumentCanvas::textureSizeOf(int page) const
{
    return m_pages.value(page).textureSize;
}

int DocumentCanvas::inkGeometryStrokeCount(int page) const
{
    return int(m_inkGeometry.value(page).size());
}

// Порядок детей узла страницы - это и есть порядок отрисовки, поэтому вопрос
// «видны ли чернила» решается обходом детей, а не разглядыванием пикселей:
// программный бэкенд отрисовки, на котором идут тесты, полос из чернил не
// растеризует вовсе.
DocumentCanvas::InkLayering DocumentCanvas::inkLayering(int page) const
{
    PageSceneNode *node = m_nodes.value(page);
    if (!node || !node->texture || (!node->dryInk && !node->wetInk))
        return InkLayering::Incomplete;

    bool rasterSeen = false;
    bool dryInkSeen = false;
    for (QSGNode *child = node->firstChild(); child; child = child->nextSibling()) {
        if (child == node->texture) {
            rasterSeen = true;
        } else if (child == node->dryInk) {
            if (!rasterSeen)
                return InkLayering::BelowRaster;
            dryInkSeen = true;
        } else if (child == node->wetInk) {
            if (!rasterSeen)
                return InkLayering::BelowRaster;
            if (node->dryInk && !dryInkSeen)
                return InkLayering::BelowRaster; // мокрый штрих ушёл под сухие чернила
        }
    }
    return InkLayering::AboveRaster;
}

int DocumentCanvas::pendingNodeRemovalCount() const
{
    return int(m_droppedPages.size());
}

void DocumentCanvas::rebuildLayout()
{
    // Все текстуры снимаются: документ другой, старые растры к нему отношения
    // не имеют. Учётная запись при этом пустая - вернуть текстуру чужого
    // документа страница с тем же номером не должна.
    for (auto it = m_pages.cbegin(); it != m_pages.cend(); ++it)
        m_droppedPages.insert(it.key(), PageEntry());
    m_pages.clear();
    m_visiblePages.clear();
    m_renderer->cancelAll();

    QList<QSizeF> sizes;
    if (m_document && m_document->isReady()) {
        m_renderer->setDocument(m_document->document());
        sizes.reserve(m_document->pageCount());
        for (int page = 0; page < m_document->pageCount(); ++page)
            sizes.append(m_document->pageSize(page));
    } else {
        m_renderer->setDocument(nullptr);
    }

    m_layout.setPageSizes(sizes);
    m_currentPage = 0;

    if (!sizes.isEmpty() && width() > 0)
        fitPage(0);
    else
        clampOrigin();

    updateVisiblePages();
    update();
    emit currentPageChanged();
    emit texturesChanged();
}

void DocumentCanvas::setPageMode(PageMode mode)
{
    if (pageMode() == mode)
        return;

    // Позиция чтения сохраняется: запоминаем текущую страницу и после смены
    // раскладки возвращаемся к ней - сценарии переключения режимов.
    const int page = m_currentPage;
    const qreal zoom = m_zoom;

    m_layout.setMode(DocumentLayout::Mode(int(mode)));
    setZoom(zoom);
    goToPage(page);

    updateVisiblePages();
    update();
    emit pageModeChanged();
}

void DocumentCanvas::setZoom(qreal zoom)
{
    zoomAt(QPointF(width() / 2.0, height() / 2.0), zoom);
}

void DocumentCanvas::zoomAt(const QPointF &viewPoint, qreal newZoom)
{
    // Выход за границы диапазона останавливает масштаб на границе, но не рвёт
    // позицию документа - сценарий «границы диапазона».
    const qreal clamped = qBound(kMinZoom, newZoom, kMaxZoom);
    if (qFuzzyCompare(clamped, m_zoom))
        return;

    const QPointF anchor = viewToDocument(viewPoint);
    m_zoom = clamped;
    m_origin = viewPoint - anchor * m_zoom;
    clampOrigin();

    updateVisiblePages();
    // Текстуры не перерисовываются немедленно: узлы просто масштабируются,
    // а перерендер под новое разрешение откладывается до остановки зума.
    m_lodTimer.start();
    update();
    emit viewChanged();
}

void DocumentCanvas::zoomBy(const QPointF &viewPoint, qreal factor)
{
    zoomAt(viewPoint, m_zoom * factor);
}

void DocumentCanvas::panBy(const QPointF &deltaPixels)
{
    if (deltaPixels.isNull())
        return;
    m_origin += deltaPixels;
    clampOrigin();
    updateVisiblePages();
    update();
    emit viewChanged();
}

void DocumentCanvas::flick(const QPointF &velocityPixelsPerSecond)
{
    QPointF velocity = velocityPixelsPerSecond;
    const qreal speed = std::hypot(velocity.x(), velocity.y());
    if (speed < kFlickStopSpeed) {
        stopFlick();
        return;
    }
    if (speed > kFlickMaxSpeed)
        velocity *= kFlickMaxSpeed / speed;

    const bool was = isFlicking();
    m_flickVelocity = velocity;
    m_flickClock.restart();
    m_flickTimer.start();
    if (!was)
        emit flickingChanged();
}

void DocumentCanvas::stopFlick()
{
    if (!isFlicking())
        return;
    m_flickTimer.stop();
    m_flickVelocity = QPointF();
    // Инерция кончилась - самое время подтянуть резкость под текущий масштаб.
    m_lodTimer.start();
    emit flickingChanged();
}

void DocumentCanvas::stepFlick()
{
    const qreal dt = qMin(m_flickClock.restart() / 1000.0, 0.05);
    if (dt <= 0.0)
        return;

    const QPointF before = m_origin;
    panBy(m_flickVelocity * dt);

    // Упёрлись в край документа - продолжать бросок бессмысленно.
    if (qFuzzyCompare(before.x() + 1.0, m_origin.x() + 1.0)
        && qFuzzyCompare(before.y() + 1.0, m_origin.y() + 1.0)) {
        stopFlick();
        return;
    }

    m_flickVelocity *= std::exp(-dt / kFlickTau);
    if (std::hypot(m_flickVelocity.x(), m_flickVelocity.y()) < kFlickStopSpeed)
        stopFlick();
}

void DocumentCanvas::clampOrigin()
{
    const QSizeF content = m_layout.documentSize() * m_zoom;

    if (content.width() <= width())
        m_origin.setX((width() - content.width()) / 2.0);
    else
        m_origin.setX(qBound(width() - content.width(), m_origin.x(), 0.0));

    if (content.height() <= height())
        m_origin.setY((height() - content.height()) / 2.0);
    else
        m_origin.setY(qBound(height() - content.height(), m_origin.y(), 0.0));
}

QPointF DocumentCanvas::viewToDocument(const QPointF &viewPoint) const
{
    if (qFuzzyIsNull(m_zoom))
        return QPointF();
    return (viewPoint - m_origin) / m_zoom;
}

QPointF DocumentCanvas::documentToView(const QPointF &documentPoint) const
{
    return documentPoint * m_zoom + m_origin;
}

QPointF DocumentCanvas::viewToPage(int page, const QPointF &viewPoint) const
{
    return viewToDocument(viewPoint) - m_layout.pageRect(page).topLeft();
}

int DocumentCanvas::pageAtView(const QPointF &viewPoint) const
{
    return m_layout.pageAt(viewToDocument(viewPoint));
}

QRectF DocumentCanvas::visibleDocumentRect() const
{
    if (qFuzzyIsNull(m_zoom))
        return QRectF();
    return QRectF(viewToDocument(QPointF(0, 0)), viewToDocument(QPointF(width(), height())));
}

void DocumentCanvas::goToPage(int page)
{
    const QRectF rect = m_layout.pageRect(page);
    if (rect.isNull())
        return;
    // Строка целиком: в развороте целевая страница показывается вместе с парной.
    const QRectF row = m_layout.rowRect(m_layout.rowOf(page));
    m_origin.setY(-row.top() * m_zoom);
    clampOrigin();
    updateVisiblePages();

    // Целевая страница объявляется текущей явно. В развороте вычислить её из
    // положения окна нельзя - в кадре две страницы, и обе одинаково видны.
    if (m_currentPage != page) {
        m_currentPage = page;
        emit currentPageChanged();
    }

    update();
    emit viewChanged();
}

void DocumentCanvas::fitPage(int page)
{
    const QRectF row = m_layout.rowRect(m_layout.rowOf(page));
    if (row.isEmpty() || width() <= 0 || height() <= 0)
        return;

    const qreal scale = qMin(width() / row.width(), height() / row.height());
    m_zoom = qBound(kMinZoom, scale * 0.98, kMaxZoom);
    m_origin = QPointF(0, -row.top() * m_zoom);
    clampOrigin();
    m_lodTimer.start();
    updateVisiblePages();
    update();
    emit viewChanged();
}

void DocumentCanvas::restoreViewState(int page, qreal zoom, int pageMode, qreal originX,
                                      qreal originY, bool valid)
{
    if (m_layout.isEmpty())
        return;

    if (!valid) {
        fitPage(0);
        return;
    }

    setPageMode(PageMode(pageMode));
    m_zoom = qBound(kMinZoom, zoom, kMaxZoom);
    m_origin = QPointF(originX, originY);
    clampOrigin();

    const int clampedPage = qBound(0, page, qMax(0, m_layout.pageCount() - 1));
    if (m_currentPage != clampedPage) {
        m_currentPage = clampedPage;
        emit currentPageChanged();
    }

    updateVisiblePages();
    m_lodTimer.start();
    update();
    emit viewChanged();
}

QSize DocumentCanvas::targetPixelSize(int page) const
{
    const QSizeF size = m_layout.pageRect(page).size();
    if (size.isEmpty())
        return QSize();

    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    // Вид считается движущимся, пока не истёк таймер устаканивания или пока
    // идёт инерция.
    const bool moving = m_lodTimer.isActive() || isFlicking();
    const qreal scale = moving ? kMovingResolutionScale : 1.0;
    QSizeF pixels = size * m_zoom * dpr * scale;

    const qreal longest = qMax(pixels.width(), pixels.height());
    if (longest > kMaxTextureEdge)
        pixels *= kMaxTextureEdge / longest;

    return QSize(qMax(1, qRound(pixels.width())), qMax(1, qRound(pixels.height())));
}

qint64 DocumentCanvas::retainedRasterBytes() const
{
    qint64 bytes = 0;
    for (auto it = m_pages.cbegin(); it != m_pages.cend(); ++it)
        bytes += it->bytes();
    return bytes;
}

void DocumentCanvas::setRasterMemoryBudget(qint64 bytes)
{
    if (m_rasterBudget == bytes)
        return;
    m_rasterBudget = bytes;
    updateVisiblePages();
}

// Диапазон строк вокруг видимых, расширенный на margin строк в обе стороны.
// Границы документа накладывает сама раскладка: pagesInRows зажимает диапазон.
QPair<int, int> DocumentCanvas::retentionRowRange(int margin) const
{
    int firstRow = -1;
    int lastRow = -1;
    if (!m_visiblePages.isEmpty()) {
        // pagesIntersecting отдаёт страницы сверху вниз, так что крайние строки
        // - это строки первой и последней страницы списка.
        firstRow = m_layout.rowOf(m_visiblePages.first());
        lastRow = m_layout.rowOf(m_visiblePages.last());
    } else {
        // Кадр может целиком попасть в промежуток между строками; удерживать
        // при этом нужно окрестность текущей страницы, а не пустоту.
        firstRow = lastRow = m_layout.rowOf(m_currentPage);
    }
    if (firstRow < 0 || lastRow < 0)
        return {0, -1};

    return {firstRow - margin, lastRow + margin};
}

// Снимает со страницы растр: запрос отменяется, растр в памяти больше не
// держим, а размер живой пока текстуры - держим, потому что узел страницы
// снимается не в этом кадре. Геометрию чернил страницы это не трогает: она
// живёт по зоне удержания, весит несопоставимо меньше растра и обязана быть
// готовой раньше, чем страница покажется.
void DocumentCanvas::dropPageRaster(int page)
{
    const auto it = m_pages.find(page);
    if (it == m_pages.end())
        return;

    m_renderer->cancel(page);
    PageEntry pending = it.value();
    pending.image = QImage();
    pending.imageDirty = false;
    m_droppedPages.insert(page, pending);
    m_pages.erase(it);
}

// Вытесняет самые дальние от кадра страницы, пока память под растрами не
// уложится в бюджет. Ширина зоны удержания от масштаба не зависит, а вес
// страницы - ещё как: на 800% одна страница тянет десятки мегабайт.
void DocumentCanvas::evictOverBudget()
{
    qint64 bytes = retainedRasterBytes();
    if (bytes <= m_rasterBudget)
        return;

    // Видимые страницы и их ближайшие соседи не вытесняются никогда: даже когда
    // одни они в бюджет не влезают, заполнителей в кадре быть не должно.
    const QPair<int, int> protectedRows = retentionRowRange(kMinRetentionRows);
    const QList<int> protectedPages =
        m_layout.pagesInRows(protectedRows.first, protectedRows.second);
    QSet<int> protectedSet(protectedPages.cbegin(), protectedPages.cend());
    for (int page : std::as_const(m_visiblePages))
        protectedSet.insert(page);

    QList<int> candidates;
    for (auto it = m_pages.cbegin(); it != m_pages.cend(); ++it) {
        if (!protectedSet.contains(it.key()))
            candidates.append(it.key());
    }

    // Дальние от кадра уходят первыми, при равном расстоянии - те, что тяжелее:
    // так на освобождение бюджета уходит меньше страниц.
    const QPair<int, int> visibleRows = retentionRowRange(0);
    const auto distance = [this, visibleRows](int page) {
        const int row = m_layout.rowOf(page);
        return qMax(0, qMax(visibleRows.first - row, row - visibleRows.second));
    };
    std::sort(candidates.begin(), candidates.end(), [this, &distance](int a, int b) {
        const int da = distance(a);
        const int db = distance(b);
        if (da != db)
            return da > db;
        return m_pages.value(a).bytes() > m_pages.value(b).bytes();
    });

    for (int page : std::as_const(candidates)) {
        if (bytes <= m_rasterBudget)
            break;
        bytes -= m_pages.value(page).bytes();
        dropPageRaster(page);
    }
}

void DocumentCanvas::updateVisiblePages()
{
    if (m_layout.isEmpty())
        return;

    const QRectF visible = visibleDocumentRect();
    m_visiblePages = m_layout.pagesIntersecting(visible);

    // Зона удержания шире кадра на kRetentionRows строк в обе стороны: при
    // прокрутке и при раззуме соседние страницы уже готовы, а всё, что дальше,
    // отдаёт текстуру - иначе документ на 200 страниц держал бы в памяти все
    // растры сразу. Считается она в строках раскладки, а не в высотах кадра:
    // кадр в координатах документа сжимается с ростом масштаба, и зона,
    // отмеренная от него, на большом зуме не дотягивалась бы до соседей.
    const QPair<int, int> rows = retentionRowRange(kRetentionRows);
    const QList<int> keep = m_layout.pagesInRows(rows.first, rows.second);
    const QSet<int> keepSet(keep.cbegin(), keep.cend());

    const QList<int> resident = m_pages.keys();
    for (int page : resident) {
        if (!keepSet.contains(page))
            dropPageRaster(page);
    }

    // Страница вернулась раньше, чем её узел успели снять: забираем учётную
    // запись обратно вместе с ещё живой текстурой. Иначе страница мигнула бы
    // заполнителем и потребовала бы нового растра - того же самого.
    for (int page : keep) {
        const auto dropped = m_droppedPages.constFind(page);
        if (dropped == m_droppedPages.constEnd() || dropped->textureSize.isEmpty())
            continue; // готовой текстуры в узле нет: страница отрисуется заново
        if (!m_pages.contains(page))
            m_pages.insert(page, *dropped);
        m_droppedPages.erase(dropped);
    }

    // Зона удержания одинаково широка на любом масштабе, поэтому в память она
    // упирается именно на большом зуме: там страницы тяжёлые.
    evictOverBudget();

    // Геометрия чернил живёт ровно на удерживаемых страницах.
    for (auto it = m_inkGeometry.begin(); it != m_inkGeometry.end();) {
        if (!keepSet.contains(it.key())) {
            m_inkDirtyPages.remove(it.key());
            it = m_inkGeometry.erase(it);
        } else {
            ++it;
        }
    }
    for (int page : keep)
        ensureInkGeometry(page);

    requestRenders(false);
    updateCurrentPage();
    emit texturesChanged();
}

void DocumentCanvas::requestRenders(bool immediate)
{
    if (!m_document || !m_document->isReady())
        return;

    for (int page : std::as_const(m_visiblePages)) {
        const QSize target = targetPixelSize(page);
        if (!target.isValid() || target.isEmpty())
            continue;

        PageEntry &entry = m_pages[page];

        // Страница без текстуры вовсе нужна немедленно: пустое место вместо
        // страницы запрещено, а заполнитель - это только чем прикрыть ожидание.
        const bool never = entry.textureSize.isEmpty();
        if (!never && !immediate)
            continue;

        // Перерисовываем, только если разрешение заметно разошлось с нужным:
        // иначе каждое дрожание масштаба тянуло бы новый рендер.
        const qreal ratio = entry.textureSize.isEmpty()
            ? 0.0
            : qreal(entry.textureSize.width()) / qreal(target.width());
        if (!never && ratio > 0.9 && ratio < 1.15)
            continue;

        if (entry.requestedSize == target)
            continue;

        entry.requestedSize = target;
        m_renderer->request(page, target);
    }
}

void DocumentCanvas::onPageRendered(int page, const QSize &pixelSize, const QImage &image)
{
    const auto it = m_pages.find(page);
    if (it == m_pages.end())
        return; // страница успела уйти из зоны удержания

    it->image = image;
    it->textureSize = pixelSize;
    it->imageDirty = true;
    // Растры приезжают асинхронно, между сменами вида: бюджет проверяется и
    // здесь, иначе страница, отрисованная в новом разрешении, перебирала бы его
    // до ближайшего движения.
    evictOverBudget();
    update();
    emit texturesChanged();
}

void DocumentCanvas::updateCurrentPage()
{
    if (m_layout.isEmpty())
        return;
    // Текущей считается страница под верхней третью окна: так подсветка в
    // оглавлении меняется тогда же, когда меняется то, что читают.
    const QPointF probe = viewToDocument(QPointF(width() / 2.0, height() / 3.0));
    int page = m_layout.nearestPage(probe);

    // В развороте в кадре две страницы. Если текущая среди них - она и
    // остаётся текущей: иначе переход по оглавлению на правую страницу
    // разворота тут же перескакивал бы на левую.
    const QList<int> row = m_layout.pagesInRow(m_layout.rowOf(page));
    if (row.contains(m_currentPage))
        page = m_currentPage;

    if (page != m_currentPage && page >= 0) {
        m_currentPage = page;
        emit currentPageChanged();
    }
}

void DocumentCanvas::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() == oldGeometry.size())
        return;

    if (oldGeometry.isEmpty() && !m_layout.isEmpty())
        fitPage(0);
    else
        clampOrigin();

    updateVisiblePages();
    m_lodTimer.start();
    update();
}

void DocumentCanvas::releaseResources()
{
    // Граф сцены будет уничтожен: указатели на узлы протухают, а текстуры
    // придётся создать заново из свежих растров. Снимать уже нечего, и вернуть
    // из очереди тоже нечего - текстур, на которые она ссылается, больше нет.
    m_nodes.clear();
    m_droppedPages.clear();
    invalidateTextures();
    QQuickItem::releaseResources();
}

void DocumentCanvas::invalidateTextures()
{
    for (auto it = m_pages.begin(); it != m_pages.end(); ++it) {
        it->textureSize = QSize();
        it->requestedSize = QSize();
        it->imageDirty = false;
        it->image = QImage();
    }
    m_renderer->cancelAll();
    QMetaObject::invokeMethod(this, [this] { requestRenders(true); }, Qt::QueuedConnection);
}

QSGNode *DocumentCanvas::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *root = static_cast<QSGTransformNode *>(oldNode);
    if (!root) {
        root = new QSGTransformNode;
        m_nodes.clear();
        m_droppedPages.clear();
    }

    // Зум и панорама - только матрица корневого узла. Содержимое дочерних узлов
    // при этом не трогается, поэтому масштабирование не стоит перерисовки.
    QMatrix4x4 matrix;
    matrix.translate(float(m_origin.x()), float(m_origin.y()));
    matrix.scale(float(m_zoom));
    root->setMatrix(matrix);

    bool deferred = false;

    int removals = 0;
    for (auto it = m_droppedPages.begin(); it != m_droppedPages.end();) {
        if (removals >= kMaxNodeRemovalsPerFrame) {
            deferred = true;
            break;
        }
        if (PageSceneNode *node = m_nodes.take(it.key())) {
            root->removeChildNode(node);
            delete node;
            ++removals;
        }
        it = m_droppedPages.erase(it);
    }

    int textureUploads = 0;
    int inkRebuilds = 0;

    // Видимые страницы обслуживаются первыми: их заполнители пользователь
    // видит, а страницы из зоны удержания - нет.
    QList<int> order = m_visiblePages;
    for (auto it = m_pages.cbegin(); it != m_pages.cend(); ++it) {
        if (!order.contains(it.key()))
            order.append(it.key());
    }

    for (int page : std::as_const(order)) {
        const auto it = m_pages.find(page);
        if (it == m_pages.end())
            continue;
        const QRectF rect = m_layout.pageRect(page);
        if (rect.isEmpty())
            continue;

        PageSceneNode *node = m_nodes.value(page);
        if (!node) {
            node = new PageSceneNode(page);
            root->appendChildNode(node);
            m_nodes.insert(page, node);
        }

        // Узел страницы стоит в начале координат страницы: всё, что лежит
        // внутри - и растр, и будущие чернила - живёт в координатах страницы.
        QMatrix4x4 pageMatrix;
        pageMatrix.translate(float(rect.x()), float(rect.y()));
        node->setMatrix(pageMatrix);

        const QRectF local(QPointF(0, 0), rect.size());

        // Заполнитель стоит под текстурой и виден, пока страница не отрисована:
        // пустого места на месте страницы быть не должно.
        if (!node->placeholder) {
            node->placeholder = new QSGSimpleRectNode(local, placeholderColor());
            node->addToLayer(node->placeholder, PageSceneNode::PlaceholderLayer);
        }
        node->placeholder->setRect(local);

        if (it->imageDirty && !it->image.isNull() && window()
            && textureUploads >= kMaxTextureUploadsPerFrame) {
            deferred = true;
        } else if (it->imageDirty && !it->image.isNull() && window()) {
            ++textureUploads;
            QSGTexture *texture = window()->createTextureFromImage(
                it->image, QQuickWindow::TextureCanUseAtlas);
            if (texture) {
                // Узел текстуры получает содержимое до включения в граф сцены:
                // узел без текстуры невалиден, и рендер, наткнувшийся на него,
                // разыменовывает ноль.
                const bool fresh = !node->texture;
                if (fresh) {
                    node->texture = new QSGSimpleTextureNode;
                    node->texture->setOwnsTexture(true);
                    node->texture->setFiltering(QSGTexture::Linear);
                }
                node->texture->setTexture(texture);
                node->texture->setRect(local);
                if (fresh)
                    node->addToLayer(node->texture, PageSceneNode::RasterLayer);
                it->imageDirty = false;
                it->image = QImage(); // растр уже в текстуре, копия не нужна
            }
        }

        // Текстура растягивается на прямоугольник страницы: при зуме это и есть
        // «масштабированная версия уже отрисованного изображения», пока не
        // подоспеет перерендер в новом разрешении.
        if (node->texture)
            node->texture->setRect(local);

        if (updateDryInkNodes(node, inkRebuilds >= kMaxInkRebuildsPerFrame))
            ++inkRebuilds;
        else if (inkRebuilds >= kMaxInkRebuildsPerFrame)
            deferred = true;
        updateWetInkNodes(node);
    }

    // Что не поместилось в этот кадр, доедет следующим.
    if (deferred)
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);

    return root;
}

// Сухие чернила пересобираются только когда состав штрихов на странице
// изменился: обычная прокрутка и зум их не трогают вовсе.
// Возвращает true, если пересборка действительно случилась.
bool DocumentCanvas::updateDryInkNodes(PageSceneNode *node, bool budgetExhausted)
{
    const QList<StrokeGeometry> geometry = m_inkGeometry.value(node->page);

    QList<quint64> ids;
    ids.reserve(geometry.size());
    for (const StrokeGeometry &entry : geometry)
        ids.append(entry.id);

    if (ids == node->dryInkIds)
        return false;
    if (budgetExhausted)
        return false;

    if (node->dryInk) {
        node->removeChildNode(node->dryInk);
        delete node->dryInk;
        node->dryInk = nullptr;
    }

    node->dryInkIds = ids;
    if (geometry.isEmpty())
        return true;

    node->dryInk = new QSGNode;
    node->addToLayer(node->dryInk, PageSceneNode::DryInkLayer);
    for (const StrokeGeometry &entry : geometry) {
        QSGGeometryNode *strokeNode = makeStripNode(entry.color);
        uploadStrip(strokeNode, entry.strip.constData(), entry.strip.size());
        node->dryInk->appendChildNode(strokeNode);
    }
    return true;
}

void DocumentCanvas::updateWetInkNodes(PageSceneNode *node)
{
    const bool active = m_wet.isActive() && m_wetPage == node->page;
    const QVector<StrokeTessellator::Vertex> &strip = m_wet.strip();

    if (!active || strip.size() < 4) {
        if (node->wetInk) {
            node->removeChildNode(node->wetInk);
            delete node->wetInk;
            node->wetInk = nullptr;
            node->wetChunks.clear();
            node->wetVertexCount = 0;
        }
        return;
    }

    if (!node->wetInk) {
        node->wetInk = new QSGNode;
        node->addToLayer(node->wetInk, PageSceneNode::WetInkLayer);
        node->wetChunks.clear();
        node->wetVertexCount = 0;
    }

    const int needed =
        qMax(1, (strip.size() - kWetChunkOverlap + kWetChunkVertices - kWetChunkOverlap - 1)
                    / (kWetChunkVertices - kWetChunkOverlap));

    while (node->wetChunks.size() < needed) {
        QSGGeometryNode *chunk = makeStripNode(m_penColor);
        node->wetInk->appendChildNode(chunk);
        node->wetChunks.append(chunk);
    }

    // Перезаливается только последний, ещё растущий кусок; предыдущие уже
    // заполнены целиком и больше не меняются.
    const int firstDirty = qMax(0, node->wetVertexCount == 0
                                       ? 0
                                       : (node->wetVertexCount - 1) / (kWetChunkVertices
                                                                       - kWetChunkOverlap));
    for (int index = firstDirty; index < needed; ++index) {
        const int start = wetChunkStart(index);
        const int count = qMin(kWetChunkVertices, strip.size() - start);
        if (count < 4)
            continue;
        uploadStrip(node->wetChunks[index], strip.constData() + start, count);
    }
    node->wetVertexCount = strip.size();
}

// --- Чернила -----------------------------------------------------------------

void DocumentCanvas::setInk(InkDocument *ink)
{
    if (m_ink == ink)
        return;
    if (m_ink)
        m_ink->disconnect(this);

    m_ink = ink;
    m_inkGeometry.clear();
    m_inkDirtyPages.clear();

    if (m_ink) {
        connect(m_ink, &InkDocument::pageChanged, this, &DocumentCanvas::onInkPageChanged);
        connect(m_ink, &InkDocument::reloaded, this, &DocumentCanvas::onInkReloaded);
    }

    updateVisiblePages();
    update();
    emit inkChanged();
}

void DocumentCanvas::setTool(Tool tool)
{
    if (m_tool == tool)
        return;
    m_tool = tool;
    emit toolChanged();
}

void DocumentCanvas::setPenWidth(qreal width)
{
    width = qBound(minPenWidth(), width, maxPenWidth());
    if (qFuzzyCompare(m_penWidth, width))
        return;
    m_penWidth = width;
    emit penChanged();
}

void DocumentCanvas::setPenColor(const QColor &color)
{
    if (m_penColor == color)
        return;
    m_penColor = color;
    emit penChanged();
}

void DocumentCanvas::setEraserRadius(qreal radius)
{
    radius = qBound(1.0, radius, 60.0);
    if (qFuzzyCompare(m_eraserRadius, radius))
        return;
    m_eraserRadius = radius;
    emit penChanged();
}

void DocumentCanvas::setStylusInContact(bool inContact)
{
    if (m_stylusInContact == inContact)
        return;
    m_stylusInContact = inContact;
    emit stylusActiveChanged();
}

void DocumentCanvas::setPointerIsEraser(bool eraser)
{
    if (m_pointerIsEraser == eraser)
        return;
    // Инструмент пользователя не трогается: перевернули перо обратно - и
    // прежний инструмент вернулся сам.
    m_pointerIsEraser = eraser;
    emit toolChanged();
}

bool DocumentCanvas::beginInkAt(const QPointF &viewPoint, qreal pressure, bool eraser)
{
    if (!m_ink || m_layout.isEmpty())
        return false;

    // Инерция и штрих несовместимы: перо коснулось - документ встаёт.
    stopFlick();
    setStylusInContact(true);

    if (eraser || m_tool == Tool::Eraser) {
        m_erasedInCurrentStroke.clear();
        eraseAt(viewPoint);
        return true;
    }

    const int page = m_layout.pageAt(viewToDocument(viewPoint));
    if (page < 0)
        return false; // касание в промежутке между страницами не рисует

    Stroke prototype;
    prototype.tool = InkTool::Pen;
    prototype.color = m_penColor;
    prototype.width = m_penWidth;

    m_wetPage = page;
    // Масштаб фиксируется на начало штриха: пока перо в контакте, жесты не
    // начинаются, поэтому внутри штриха он не меняется.
    m_wet.begin(page, prototype, m_zoom);
    // Чернила складываются в координатах страницы, а не экрана.
    m_wet.addPoint({viewToPage(page, viewPoint), pressure, FrameStats::nowNs() / 1e9});

    if (m_frameStats)
        m_frameStats->markInput();
    update();
    emit stylusActiveChanged();
    return true;
}

void DocumentCanvas::continueInkAt(const QPointF &viewPoint, qreal pressure)
{
    // Начатый штрих доводится до конца тем же инструментом: нажатие кнопки на
    // корпусе посреди линии не должно превращать её в стирание.
    if (!m_wet.isActive()) {
        if ((m_pointerIsEraser || m_tool == Tool::Eraser) && m_stylusInContact)
            eraseAt(viewPoint);
        return;
    }

    m_wet.addPoint({viewToPage(m_wetPage, viewPoint), pressure, FrameStats::nowNs() / 1e9});

    if (m_frameStats)
        m_frameStats->markInput();
    update();
}

void DocumentCanvas::endInk()
{
    setStylusInContact(false);

    if (!m_erasedInCurrentStroke.isEmpty()) {
        m_erasedInCurrentStroke.clear();
        return;
    }
    if (!m_wet.isActive())
        return;

    // Штрих переезжает в постоянную геометрию страницы.
    const Stroke stroke = m_wet.finish();
    const int page = m_wetPage;
    m_wetPage = -1;
    m_wet.reset();

    if (m_ink && stroke.isValid())
        m_ink->addStroke(stroke); // пометит страницу грязной через pageChanged
    else
        update();

    emit stylusActiveChanged();
}

void DocumentCanvas::cancelInk()
{
    setStylusInContact(false);
    m_wet.reset();
    m_wetPage = -1;
    m_erasedInCurrentStroke.clear();
    update();
    emit stylusActiveChanged();
}

void DocumentCanvas::eraseAt(const QPointF &viewPoint)
{
    if (!m_ink)
        return;
    const QPointF documentPoint = viewToDocument(viewPoint);
    const int page = m_layout.pageAt(documentPoint);
    if (page < 0)
        return;

    const QList<quint64> hits =
        m_ink->strokesNear(page, viewToPage(page, viewPoint), m_eraserRadius);
    QList<quint64> fresh;
    for (quint64 id : hits) {
        if (!m_erasedInCurrentStroke.contains(id)) {
            m_erasedInCurrentStroke.append(id);
            fresh.append(id);
        }
    }
    if (fresh.isEmpty())
        return;

    // Всё движение ластика - одно действие в истории, поэтому стирание идёт
    // накопленным списком, а не по одному штриху за событие.
    m_ink->eraseStrokes(fresh);
    if (m_frameStats)
        m_frameStats->markInput();
    update();
}

void DocumentCanvas::onInkPageChanged(int page)
{
    m_inkDirtyPages.insert(page);

    // Геометрия пересобирается здесь, на потоке UI, а не внутри updatePaintNode:
    // тесселяции на потоке рендера быть не должно. Без этого страница осталась
    // бы со старой геометрией до ближайшей смены видимой области - то есть
    // законченный штрих появлялся бы только после зума или прокрутки.
    if (m_pages.contains(page))
        ensureInkGeometry(page);
    else
        dropInkGeometry(page); // страница вне зоны удержания: соберём, когда вернётся

    update();
}

// Чернила сменились целиком: вся накопленная геометрия недействительна.
void DocumentCanvas::onInkReloaded()
{
    m_inkGeometry.clear();
    m_inkDirtyPages.clear();
    for (auto it = m_pages.cbegin(); it != m_pages.cend(); ++it) {
        m_inkDirtyPages.insert(it.key());
        ensureInkGeometry(it.key());
    }
    update();
}

void DocumentCanvas::ensureInkGeometry(int page)
{
    if (!m_ink)
        return;
    if (m_inkGeometry.contains(page) && !m_inkDirtyPages.contains(page))
        return;

    QList<StrokeGeometry> geometry;
    StrokeTessellator tessellator;
    for (const InkDocument::Record &record : m_ink->strokesOnPage(page)) {
        tessellator.setWidth(record.stroke.width);
        StrokeGeometry entry;
        entry.id = record.id;
        entry.color = record.stroke.color;
        entry.strip = tessellator.tessellate(record.stroke.points);
        if (!StrokeTessellator::isDegenerate(entry.strip))
            geometry.append(entry);
    }

    if (geometry.isEmpty())
        m_inkGeometry.remove(page);
    else
        m_inkGeometry.insert(page, geometry);
    m_inkDirtyPages.remove(page);
}

void DocumentCanvas::dropInkGeometry(int page)
{
    m_inkGeometry.remove(page);
    m_inkDirtyPages.remove(page);
}

// --- Ввод --------------------------------------------------------------------

bool DocumentCanvas::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
    case QEvent::TabletEnterProximity:
    case QEvent::TabletLeaveProximity:
        if (handleTabletEvent(static_cast<QTabletEvent *>(event)))
            return true;
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease:
        if (handleMouseEvent(static_cast<QMouseEvent *>(event)))
            return true;
        break;
    case QEvent::NativeGesture:
        if (handleNativeGesture(static_cast<QNativeGestureEvent *>(event)))
            return true;
        break;
    default:
        break;
    }
    // Касания сюда не попадают намеренно: ими занимаются жестовые обработчики
    // в QML, и чернил они не создают ни при каких условиях. Отклонение ладони
    // получается из этого само собой.
    return QQuickItem::event(event);
}

bool DocumentCanvas::handleTabletEvent(QTabletEvent *event)
{
    const auto *device = event->pointingDevice();
    const bool eraserEnd =
        device && device->pointerType() == QPointingDevice::PointerType::Eraser;
    const bool barrelButton = event->buttons() & Qt::MiddleButton;
    setPointerIsEraser(eraserEnd || barrelButton);

    if (event->type() == QEvent::TabletEnterProximity
        || event->type() == QEvent::TabletLeaveProximity) {
        return false; // не наш случай, но тип указателя уже учтён
    }

    const QPointF position = event->position();
    // Устройство может не сообщать давление: тогда штрих идёт постоянной
    // толщиной, а не нулевой.
    const bool reportsPressure = device
        && device->capabilities().testFlag(QPointingDevice::Capability::Pressure);
    const qreal pressure = reportsPressure ? event->pressure() : -1.0;

    switch (event->type()) {
    case QEvent::TabletPress:
        beginInkAt(position, pressure, m_pointerIsEraser);
        event->accept();
        return true;
    case QEvent::TabletMove:
        if (m_stylusInContact) {
            continueInkAt(position, pressure);
            event->accept();
            return true;
        }
        return false;
    case QEvent::TabletRelease:
        endInk();
        event->accept();
        return true;
    default:
        break;
    }
    return false;
}

// Перо на целевом устройстве приходит в обработчик мышиных событий, а не
// планшетных: Qt Quick доставляет элементу QMouseEvent, у которого устройство -
// Stylus, а точка несёт настоящее давление. Спайк 1.1 показал ровно это -
// планшетных событий не приходит вовсе, а давление доступно и непрерывно.
DocumentCanvas::PointerIntent DocumentCanvas::intentFor(const QPointingDevice *device,
                                                        Qt::MouseButtons buttons,
                                                        qreal pointPressure)
{
    PointerIntent intent;
    intent.stylus = device
        && (device->type() == QInputDevice::DeviceType::Stylus
            || device->type() == QInputDevice::DeviceType::Airbrush);
    intent.contact = buttons.testFlag(Qt::LeftButton);

    // Ластик включается двумя способами: перевёрнутым концом пера, если он у
    // пера есть, и кнопкой на корпусе. Оба - «пока держишь»: отпустил, и
    // вернулся прежний инструмент. У обычной мыши средняя кнопка тянет холст и
    // ластиком не становится.
    const bool invertedEnd =
        intent.stylus && device->pointerType() == QPointingDevice::PointerType::Eraser;
    const bool barrelButton = intent.stylus && buttons.testFlag(Qt::MiddleButton);
    intent.eraser = invertedEnd || barrelButton;

    // У мыши давления нет - штрих идёт постоянной толщиной.
    intent.pressure = intent.stylus ? pointPressure : -1.0;
    return intent;
}

bool DocumentCanvas::handleMouseEvent(QMouseEvent *event)
{
    if (event->source() != Qt::MouseEventNotSynthesized)
        return false; // синтетика от тача - не рисуем

    const PointerIntent intent = intentFor(event->pointingDevice(), event->buttons(),
                                           event->points().constFirst().pressure());
    setPointerIsEraser(intent.eraser);

    switch (event->type()) {
    case QEvent::MouseButtonPress:
        if (event->button() != Qt::LeftButton)
            return false; // нажатие одной лишь кнопки на корпусе штриха не начинает
        beginInkAt(event->position(), intent.pressure, intent.eraser);
        event->accept();
        return true;
    case QEvent::MouseMove:
        if (!m_stylusInContact || !intent.contact)
            return false;
        continueInkAt(event->position(), intent.pressure);
        event->accept();
        return true;
    case QEvent::MouseButtonRelease:
        if (event->button() != Qt::LeftButton || !m_stylusInContact)
            return false;
        endInk();
        event->accept();
        return true;
    default:
        break;
    }
    return false;
}

// Пинч на тачпаде приходит не касаниями, а готовым жестом: Qt Wayland
// превращает zwp_pointer_gestures_v1 в последовательность Begin -> Pan / Rotate /
// Zoom -> End. PinchHandler в QML его не видит - ему нужны точки касания.
bool DocumentCanvas::handleNativeGesture(QNativeGestureEvent *event)
{
    const auto startGesture = [this] {
        // Новый жест отменяет инерцию - как у жестовых обработчиков в QML.
        stopFlick();
        m_gestureActive = true;
        m_gestureStartZoom = m_zoom;
        m_gestureScale = 1.0;
    };

    switch (event->gestureType()) {
    case Qt::BeginNativeGesture:
        startGesture();
        break;
    case Qt::EndNativeGesture:
        // Инерции у тачпада нет: пальцы оторвались - документ стоит, где стоял.
        m_gestureActive = false;
        break;
    case Qt::ZoomNativeGesture:
        // Приоритет у штриха: пока перо в контакте, жест ничего не двигает.
        if (m_stylusInContact)
            break;
        // Begin пропущен - жест начался вне холста. Начинаем его с середины.
        if (!m_gestureActive)
            startGesture();
        // Накопитель зажат по границам масштаба: на упоре обратное движение
        // пальцев сразу уменьшает масштаб, а не отматывает перелёт (D4).
        m_gestureScale = qBound(kMinZoom / m_gestureStartZoom, m_gestureScale + event->value(),
                                kMaxZoom / m_gestureStartZoom);
        zoomAt(event->position(), m_gestureStartZoom * m_gestureScale);
        break;
    case Qt::PanNativeGesture:
        if (!m_stylusInContact)
            panBy(event->delta());
        break;
    case Qt::RotateNativeGesture:
        // Поворота холста нет. Событие принимается, чтобы не досталось родителю.
        break;
    default:
        return false;
    }
    event->accept();
    return true;
}
