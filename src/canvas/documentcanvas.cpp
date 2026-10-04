#include "documentcanvas.h"

#include <QQuickWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <QSGClipNode>
#include <QSGGeometryNode>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QSGTransformNode>

#include "canvas/insertlistmodel.h"
#include "canvas/inkmaterial.h"
#include "canvas/inknodepool.h"
#include "pdf/pagerenderservice.h"
#include "pdf/pdfdocumentmodel.h"
#include "perf/framestats.h"

namespace {

// Задержка после последнего изменения масштаба, по истечении которой страницы
// перерисовываются под новое разрешение.
constexpr int kLodSettleMs = 120;

// Простой пера и вида, после которого недавние штрихи переезжают в сухой слой.
// Перенос перезаливает слитый батч сухих чернил страницы - один длинный кадр
// на плотной странице, - поэтому он ждёт паузы, а не идёт на каждом отрыве:
// на беглом письме отрывов 2-4 в секунду, паузы между словами короче.
constexpr int kRecentFlushMs = 700;

// Инерция: экспоненциальное затухание с постоянной времени kFlickTau и
// остановкой на пороге kFlickStopSpeed. Значения подобраны так, чтобы бросок
// проходил примерно экран за полсекунды и вставал без рывка.
constexpr qreal kFlickTau = 0.32;
constexpr qreal kFlickStopSpeed = 24.0;   // пикселей в секунду
constexpr qreal kFlickMaxSpeed = 6000.0;  // пикселей в секунду
constexpr int kFlickIntervalMs = 4;

inline QColor placeholderColor() { return QColor(238, 238, 240); }

// Вкладыш - белый непрозрачный листок с тонкой границей постоянной экранной
// толщины: на 25% граница в точках страницы исчезла бы, на 800% стала бы
// полосой.
inline QColor insertBackgroundColor() { return QColor(255, 255, 255); }
inline QColor insertBorderColor() { return QColor(120, 124, 140); }
constexpr qreal kInsertBorderPixels = 1.0;

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

// Чернила одной поверхности в графе сцены - страницы или вкладыша. Слои снизу
// вверх: сухие, недавние, мокрый штрих.
struct InkLayerNodes
{
    // Сухие чернила: узел на штрих, чтобы у каждого был свой цвет.
    QSGNode *dryInk = nullptr;
    QList<quint64> dryInkIds;
    // Первая сборка чернил узла прошла. Она идёт сразу в сухой слой: штрихи
    // открытой заметки или страницы, вернувшейся в зону удержания, - не
    // недавние, и в слотах (по батчу на слот) им не место.
    bool built = false;

    // Недавние штрихи: законченные после последнего простоя. Отрыв пера
    // кладёт новый штрих в свободный слот пула, а не узлом в сухой слой:
    // добавление узла под корнем батча перезалило бы слитый батч сухих чернил
    // страницы (на плотной странице - 26 МБ, кадр около 12 мс). В сухой слой
    // они переезжают разом, когда перо и вид в покое (DocumentCanvas,
    // kRecentFlushMs). recentIds - штрихи слотов 0..n-1 в порядке фиксации.
    std::unique_ptr<InkNodePool> recent;
    QList<quint64> recentIds;

    // Мокрый штрих разложен на куски постоянного размера: заполненный кусок
    // больше не трогается, а на кадр перезаливается только последний,
    // незаполненный. Так объём работы на кадр не зависит от длины штриха.
    // Куски - пул, который живёт, пока живёт узел поверхности: между
    // штрихами куски пустеют, а не удаляются (design.md ink-beauty-pass, D7).
    std::unique_ptr<InkNodePool> wet;
    // Сколько кусков пула заняты нынешним штрихом; остальные пусты.
    int wetUsedChunks = 0;
    // Сколько кусков занимал самый длинный штрих поверхности: по нему пул
    // растёт заранее, в кадре переноса недавних штрихов.
    int wetHighWater = 0;
    // Номер штриха, который лежит в кусках: по нему виден новый штрих, даже
    // если между отрывом и касанием не было кадра.
    int wetStrokeSerial = -1;
    // Длина окончательной части полосы мокрого штриха на прошлом кадре: до неё
    // куски не перезаливаются.
    int wetSettledCount = 0;

    QSGNode *recentInk() const { return recent ? recent->node() : nullptr; }
    QSGNode *wetInk() const { return wet ? wet->node() : nullptr; }
};

// Узел страницы: собственная система координат страницы, заполнитель под
// текстурой и сама текстура. Какой странице он служит, знает только карта
// DocumentCanvas::m_nodes: узел из запаса переходит от страницы к странице.
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
        RecentInkLayer,
        WetInkLayer,
    };

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

    QSGSimpleRectNode *placeholder = nullptr;
    QSGSimpleTextureNode *texture = nullptr;

    InkLayerNodes ink;

private:
    Layer layerOf(const QSGNode *child) const
    {
        if (child == texture)
            return RasterLayer;
        if (child == ink.dryInk)
            return DryInkLayer;
        if (child == ink.recentInk())
            return RecentInkLayer;
        if (child == ink.wetInk())
            return WetInkLayer;
        return PlaceholderLayer; // заполнитель и всё неопознанное - в самый низ
    }
};

// Узел раскрытого вкладыша. Стоит в слое вкладышей поверх всех страниц, его
// система координат - локальная система вкладыша: начало в левом верхнем углу
// рамки, единица - точка страницы. Чернила лежат под узлом обрезки по рамке:
// прямоугольная обрезка без поворота рисуется как scissor и лишнего прохода не
// стоит (design.md, D4).
class InsertSceneNode : public QSGTransformNode
{
public:
    explicit InsertSceneNode(quint64 id)
        : id(id)
    {
        background = new QSGSimpleRectNode(QRectF(), insertBackgroundColor());
        appendChildNode(background);
        for (QSGSimpleRectNode *&edge : border) {
            edge = new QSGSimpleRectNode(QRectF(), insertBorderColor());
            appendChildNode(edge);
        }
        clip = new QSGClipNode;
        clip->setIsRectangular(true);
        // Геометрия нужна обрезке на случай, когда рендер не сведёт её к
        // scissor; прямоугольник совпадает с clipRect.
        auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4);
        clip->setGeometry(geometry);
        clip->setFlag(QSGNode::OwnsGeometry, true);
        appendChildNode(clip);
    }

    // Размер рамки и толщина границы - в точках страницы. Граница лежит
    // снаружи рамки и не отнимает места под письмо.
    void setFrame(const QSizeF &size, qreal borderWidth)
    {
        if (size == m_size && qFuzzyCompare(borderWidth, m_borderWidth))
            return;
        m_size = size;
        m_borderWidth = borderWidth;

        const QRectF local(QPointF(0, 0), size);
        const qreal b = borderWidth;
        background->setRect(local);
        border[0]->setRect(QRectF(-b, -b, size.width() + 2 * b, b));
        border[1]->setRect(QRectF(-b, size.height(), size.width() + 2 * b, b));
        border[2]->setRect(QRectF(-b, 0, b, size.height()));
        border[3]->setRect(QRectF(size.width(), 0, b, size.height()));
        QSGGeometry::updateRectGeometry(clip->geometry(), local);
        clip->setClipRect(local);
        clip->markDirty(QSGNode::DirtyGeometry);
    }

    quint64 id = 0;
    QSGSimpleRectNode *background = nullptr;
    std::array<QSGSimpleRectNode *, 4> border{};
    QSGClipNode *clip = nullptr;
    InkLayerNodes ink;

private:
    QSizeF m_size;
    qreal m_borderWidth = -1.0;
};

namespace {

// Размер куска мокрого штриха в вершинах и перекрытие между кусками.
// Перекрытие в две вершины воспроизводит на границе куска все треугольники
// полосы - и сегменты, и веера вокруг центров стыков (повтор центральной
// вершины): треугольник i - это вершины i, i+1, i+2.
constexpr int kWetChunkVertices = 512;
constexpr int kWetChunkOverlap = 2;
// Кусков в пуле мокрого слоя с самого начала: около 4000 вершин - штрих в
// 3-4 с непрерывного письма; штрих рукописи много короче. Рост пула - это
// добавление узлов под корнем батча страницы, то есть перезаливка её
// слитого батча сухих чернил (на плотной странице 28 МБ, около 20 мс
// рендера). Поэтому пул растёт заранее, в кадре переноса недавних штрихов,
// который перезаливает этот батч и так (appendToDryInk), а посреди штриха -
// только если штрих длиннее всего пула.
constexpr int kWetPoolInitialChunks = 8;

// Слотов недавних штрихов на поверхность. Переполнение - немедленный перенос
// в сухой слой, редкий всплеск: столько штрихов без паузы в 0.7 с не пишут.
constexpr int kRecentSlots = 64;
// Ключи батча недавних слотов - за пределами кусков мокрого штриха: тип
// материала у них общий, и слоты не должны сливаться с кусками.
constexpr int kRecentBatchKeyBase = 1 << 16;

// В пуле лежит мокрый штрих: хотя бы один кусок с вершинами.
bool hasWetInk(const InkLayerNodes &layer)
{
    return layer.wetUsedChunks > 0;
}

bool hasRecentInk(const InkLayerNodes &layer)
{
    return !layer.recentIds.isEmpty();
}

int wetChunkStart(int index)
{
    return index * (kWetChunkVertices - kWetChunkOverlap);
}

// Время отсчёта пера из события, в секундах. Нулевая метка значит, что
// платформа времени не сообщила, - тогда берётся момент обработки.
qreal eventTimeSeconds(const QInputEvent *event)
{
    return event->timestamp() > 0 ? event->timestamp() / 1000.0 : -1.0;
}

qreal inputTimeSeconds(qreal timeSeconds)
{
    return timeSeconds >= 0.0 ? timeSeconds : FrameStats::nowNs() / 1e9;
}

} // namespace

DocumentCanvas::DocumentCanvas(QQuickItem *parent)
    : QQuickItem(parent)
    , m_renderer(new PageRenderService(this))
    , m_insertModel(new InsertListModel(this))
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

    m_recentFlushTimer.setSingleShot(true);
    m_recentFlushTimer.setInterval(kRecentFlushMs);
    connect(&m_recentFlushTimer, &QTimer::timeout, this, [this] {
        // Перо прижато и стоит - это не простой: перенос подождёт отрыва.
        if (m_wet.isActive()) {
            m_recentFlushTimer.start();
            return;
        }
        m_flushRecentRequested = true;
        update();
    });
    // Зум и прокрутка - не простой: перенос подождёт их конца.
    connect(this, &DocumentCanvas::viewChanged, this, [this] {
        if (m_recentFlushTimer.isActive())
            restartRecentFlush();
    });
}

void DocumentCanvas::restartRecentFlush()
{
    m_recentFlushTimer.start();
}

void DocumentCanvas::flushRecentInk()
{
    m_recentFlushTimer.stop();
    m_flushRecentRequested = true;
    update();
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

QVector<StrokeTessellator::Vertex> DocumentCanvas::inkGeometryStrip(int page, quint64 id) const
{
    for (const StrokeGeometry &entry : m_inkGeometry.value(page)) {
        if (entry.id == id)
            return entry.strip;
    }
    return {};
}

namespace {

// Докуда куски мокрого штриха покрывают полосу, в вершинах; -1 - мокрого
// штриха нет (все куски пула пусты).
int wetCoveredVertexCount(const InkLayerNodes &layer)
{
    if (!hasWetInk(layer))
        return -1;
    int covered = 0;
    for (int index = 0; index < layer.wet->size(); ++index) {
        // Пустой кусок пула ничего не покрывает; кусок со старой геометрией
        // за концом полосы - покрывает и даёт число больше её длины.
        const int count = layer.wet->vertexCount(index);
        if (count > 0)
            covered = qMax(covered, wetChunkStart(index) + count);
    }
    return covered;
}

} // namespace

int DocumentCanvas::wetInkCoveredVertexCount(int page) const
{
    PageSceneNode *node = m_nodes.value(page);
    return node ? wetCoveredVertexCount(node->ink) : -1;
}

int DocumentCanvas::wetInsertCoveredVertexCount(quint64 id) const
{
    InsertSceneNode *node = m_insertNodes.value(id);
    return node ? wetCoveredVertexCount(node->ink) : -1;
}

// Порядок детей узла страницы - это и есть порядок отрисовки, поэтому вопрос
// «видны ли чернила» решается обходом детей, а не разглядыванием пикселей:
// программный бэкенд отрисовки, на котором идут тесты, полос из чернил не
// растеризует вовсе.
DocumentCanvas::InkLayering DocumentCanvas::inkLayering(int page) const
{
    PageSceneNode *node = m_nodes.value(page);
    // Слои недавних штрихов и мокрого штриха живут и пустыми: чернилами они
    // считаются, только пока в них есть вершины.
    const bool recent = node && hasRecentInk(node->ink);
    const bool wet = node && hasWetInk(node->ink);
    if (!node || !node->texture || (!node->ink.dryInk && !recent && !wet))
        return InkLayering::Incomplete;

    bool rasterSeen = false;
    bool dryInkSeen = false;
    bool recentSeen = false;
    for (QSGNode *child = node->firstChild(); child; child = child->nextSibling()) {
        if (child == node->texture) {
            rasterSeen = true;
        } else if (child == node->ink.dryInk) {
            if (!rasterSeen)
                return InkLayering::BelowRaster;
            dryInkSeen = true;
        } else if (child == node->ink.recentInk() && recent) {
            if (!rasterSeen || (node->ink.dryInk && !dryInkSeen))
                return InkLayering::BelowRaster; // недавние ушли под сухие чернила
            recentSeen = true;
        } else if (child == node->ink.wetInk() && wet) {
            if (!rasterSeen)
                return InkLayering::BelowRaster;
            if ((node->ink.dryInk && !dryInkSeen) || (recent && !recentSeen))
                return InkLayering::BelowRaster; // мокрый штрих ушёл под сухие чернила
        }
    }
    return InkLayering::AboveRaster;
}

DocumentCanvas::InkMaterialStats DocumentCanvas::inkMaterialStats(int page) const
{
    InkMaterialStats stats;
    PageSceneNode *node = m_nodes.value(page);
    if (!node)
        return stats;

    enum class Kind { Dry, Recent, Wet };
    QList<QSGMaterialType *> dryTypes;
    const InkMaterial *previousWet = nullptr;
    const auto visit = [&](QSGNode *layer, Kind kind) {
        if (!layer)
            return;
        const InkMaterial::Layer expected =
            kind == Kind::Dry ? InkMaterial::Layer::Dry : InkMaterial::Layer::Wet;
        for (QSGNode *child = layer->firstChild(); child; child = child->nextSibling()) {
            if (kind == Kind::Dry)
                ++stats.dryNodes;
            else if (kind == Kind::Recent)
                ++stats.recentPoolNodes;
            else
                ++stats.wetPoolNodes;
            if (child->type() != QSGNode::GeometryNodeType) {
                ++stats.foreignNodes;
                continue;
            }
            auto *geometryNode = static_cast<QSGGeometryNode *>(child);
            // Пустой слот пула не рисуется и штрихом не считается.
            const bool filled = geometryNode->geometry()->vertexCount() > 0;
            if (kind == Kind::Recent && filled)
                ++stats.recentNodes;
            if (kind == Kind::Wet && filled)
                ++stats.wetNodes;
            const auto *material = dynamic_cast<const InkMaterial *>(geometryNode->material());
            if (!material || material->layer() != expected
                || geometryNode->geometry()->sizeOfVertex() != int(sizeof(AxisVertex))) {
                ++stats.foreignNodes;
                continue;
            }
            if (kind == Kind::Dry) {
                dryTypes.append(material->type());
                continue;
            }
            if (dryTypes.contains(material->type()))
                stats.dryAndWetTypesDiffer = false;
            if (previousWet && previousWet->compare(material) == 0)
                stats.wetChunksSeparate = false;
            previousWet = material;
        }
    };
    visit(node->ink.dryInk, Kind::Dry);
    visit(node->ink.recentInk(), Kind::Recent);
    visit(node->ink.wetInk(), Kind::Wet);
    return stats;
}

// Вкладыш виден поверх страниц, когда его узел - в слое вкладышей, этот слой
// идёт последним ребёнком корня после слоя страниц, а чернила вкладыша лежат
// под узлом обрезки.
DocumentCanvas::InsertLayering DocumentCanvas::insertLayering(quint64 id) const
{
    InsertSceneNode *node = m_insertNodes.value(id);
    if (!node || !m_insertsRoot || !m_pagesRoot || node->parent() != m_insertsRoot)
        return InsertLayering::Missing;
    if (m_pagesRoot->nextSibling() != m_insertsRoot || m_insertsRoot->nextSibling())
        return InsertLayering::BelowPages;
    for (const QSGNode *layer : {node->ink.dryInk, node->ink.recentInk(), node->ink.wetInk()}) {
        if (layer && layer->parent() != node->clip)
            return InsertLayering::Unclipped;
    }
    return InsertLayering::AbovePages;
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
    if (m_layout.isEmpty()) {
        m_keepPages.clear();
        syncInsertGeometry();
        syncInsertModel();
        return;
    }

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
    for (auto it = m_inkRecordIds.begin(); it != m_inkRecordIds.end();) {
        if (!keepSet.contains(it.key()))
            it = m_inkRecordIds.erase(it);
        else
            ++it;
    }
    for (int page : keep)
        ensureInkGeometry(page);

    // Вкладыши живут по той же зоне удержания. Модель при этом меняется только
    // вместе с составом зоны: на обычном кадре зума и панорамы строки те же, и
    // сигналов нет.
    m_keepPages = keepSet;
    syncInsertGeometry();
    syncInsertModel();

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

    const QPointF origin = m_origin;
    const qreal zoom = m_zoom;

    if (oldGeometry.isEmpty() && !m_layout.isEmpty())
        fitPage(0);
    else
        clampOrigin();

    updateVisiblePages();
    m_lodTimer.start();
    update();

    // Смена размера двигает начало координат вида: у свойств originX/originY
    // это обязано отражаться в уведомлении, иначе элементы управления
    // вкладышами, привязанные к ним в QML, отстанут от графа сцены.
    if (m_origin != origin || !qFuzzyCompare(m_zoom, zoom))
        emit viewChanged();
}

void DocumentCanvas::releaseResources()
{
    // Граф сцены будет уничтожен: указатели на узлы протухают, а текстуры
    // придётся создать заново из свежих растров. Снимать уже нечего, и вернуть
    // из очереди тоже нечего - текстур, на которые она ссылается, больше нет.
    m_nodes.clear();
    m_spareNodes.clear();
    m_droppedPages.clear();
    m_insertNodes.clear();
    m_pagesRoot = nullptr;
    m_insertsRoot = nullptr;
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
        m_spareNodes.clear();
        m_droppedPages.clear();
        // Два постоянных слоя: страницы и поверх них вкладыши. Так вкладыш
        // рисуется поверх любой страницы, в каком бы порядке страницы ни
        // попадали в граф сцены (design.md, D4).
        //
        // Слои - узлы преобразования с единичной матрицей, а не простые
        // QSGNode. Программный рендер берёт преобразование и обрезку для
        // добавленного узла из состояния его родителя, а состояние запоминает
        // только у узлов преобразования, обрезки и прозрачности: страница,
        // добавленная под простой узел кадром позже, рисовалась бы без масштаба
        // и сдвига холста.
        m_pagesRoot = new QSGTransformNode;
        m_insertsRoot = new QSGTransformNode;
        root->appendChildNode(m_pagesRoot);
        root->appendChildNode(m_insertsRoot);
        m_insertNodes.clear();
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
            parkPageNode(node);
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
        if (!node && !m_spareNodes.isEmpty()) {
            node = m_spareNodes.takeLast();
            m_nodes.insert(page, node);
        }
        if (!node) {
            node = new PageSceneNode;
            m_pagesRoot->appendChildNode(node);
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

        const AttachInkNode attach = [node](QSGNode *child, InkSublayer sublayer) {
            node->addToLayer(child, sublayer == InkSublayer::Wet      ? PageSceneNode::WetInkLayer
                                    : sublayer == InkSublayer::Recent ? PageSceneNode::RecentInkLayer
                                                                      : PageSceneNode::DryInkLayer);
        };
        ensureInkPools(node->ink, attach);
        if (updateDryInkNodes(node->ink, m_inkGeometry.value(page),
                              inkRebuilds >= kMaxInkRebuildsPerFrame, attach))
            ++inkRebuilds;
        else if (inkRebuilds >= kMaxInkRebuildsPerFrame)
            deferred = true;
        // Мокрый штрих вкладыша рисуется во вкладыше, а не на его странице.
        updateWetInkNodes(node->ink, m_wet.isActive() && m_wetPage == page && m_wetInsert == 0);
    }

    updateInsertNodes(inkRebuilds, deferred);

    // Перенос недавних штрихов сделан везде, где был нужен; отложенный
    // бюджетом кадра доедет следующим кадром с тем же флагом.
    if (!deferred)
        m_flushRecentRequested = false;

    // Что не поместилось в этот кадр, доедет следующим.
    if (deferred)
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);

    return root;
}

void DocumentCanvas::parkPageNode(PageSceneNode *node)
{
    // Узлы в запасе не удаляются: новый узел создаётся, только когда запас
    // пуст, поэтому узлов всего - не больше, чем страниц бывало в зоне
    // удержания одновременно. Удалённый на сужении зоны узел (зум туда и
    // обратно меняет её на десятки страниц) на расширении пришлось бы
    // создать заново, а новый корень батча - тоже полная пересборка. Пустой
    // узел - около 70 пустых узлов пулов чернил, десятки КБ.
    // Снимаются только узлы-листья и простые узлы: удаление листьев под
    // корнем батча полной пересборки не требует. Текстура освобождается
    // вместе со своим узлом - по одной странице за кадр, как раньше.
    const auto drop = [node](QSGNode *&child) {
        if (!child)
            return;
        node->removeChildNode(child);
        delete child;
        child = nullptr;
    };
    QSGNode *texture = node->texture;
    QSGNode *placeholder = node->placeholder;
    drop(texture);
    drop(placeholder);
    node->texture = nullptr;
    node->placeholder = nullptr;
    InkLayerNodes &ink = node->ink;
    drop(ink.dryInk);
    ink.dryInkIds.clear();
    ink.built = false;
    if (ink.recent) {
        for (int slot = 0; slot < ink.recent->size(); ++slot)
            ink.recent->clear(slot);
    }
    ink.recentIds.clear();
    if (ink.wet) {
        for (int index = 0; index < ink.wet->size(); ++index)
            ink.wet->clear(index);
    }
    ink.wetUsedChunks = 0;
    ink.wetHighWater = 0;
    ink.wetStrokeSerial = -1;
    ink.wetSettledCount = 0;
    m_spareNodes.append(node);
}

// Пулы недавних штрихов и мокрого штриха появляются вместе с узлом
// поверхности, в том же кадре, что и остальные его узлы, - не на отрыве и не
// на касании пером.
void DocumentCanvas::ensureInkPools(InkLayerNodes &layer, const AttachInkNode &attach)
{
    if (!layer.recent) {
        layer.recent = std::make_unique<InkNodePool>(InkMaterial::Layer::Wet,
                                                     kRecentBatchKeyBase, kRecentSlots);
        layer.recentIds.clear();
        attach(layer.recent->node(), InkSublayer::Recent);
    }
    if (!layer.wet) {
        // Номер куска - ключ батча: куски не сливаются в один батч.
        layer.wet = std::make_unique<InkNodePool>(InkMaterial::Layer::Wet, 0,
                                                  kWetPoolInitialChunks);
        layer.wetUsedChunks = 0;
        layer.wetSettledCount = 0;
        attach(layer.wet->node(), InkSublayer::Wet);
    }
}

// Штрихи geometry[from..] - в конец сухого слоя: по узлу на штрих, прежние
// узлы не пересоздаются. Недавние слоты пустеют - их штрихи теперь здесь.
// Батч-рендер перезальёт слитый батч сухих чернил (узел добавлен под корнем
// батча) - один кадр, поэтому перенос идёт по простою.
void DocumentCanvas::appendToDryInk(InkLayerNodes &layer, const QList<StrokeGeometry> &geometry,
                                    int from, const AttachInkNode &attach)
{
    if (!layer.dryInk) {
        layer.dryInk = new QSGNode;
        attach(layer.dryInk, InkSublayer::Dry);
    }
    for (int index = from; index < geometry.size(); ++index) {
        const StrokeGeometry &entry = geometry[index];
        QSGGeometryNode *strokeNode = InkNodePool::makeNode(entry.color, InkMaterial::Layer::Dry);
        InkNodePool::upload(strokeNode, entry.strip.constData(), int(entry.strip.size()));
        layer.dryInk->appendChildNode(strokeNode);
        layer.dryInkIds.append(entry.id);
    }
    for (int slot = 0; slot < layer.recentIds.size(); ++slot)
        layer.recent->clear(slot);
    layer.recentIds.clear();
    // Батч страницы в этом кадре перезаливается всё равно: здесь же пул
    // мокрого слоя дорастает до вдвое большего, чем занимал самый длинный
    // штрих, если тот занимал больше половины пула. Такой же штрих потом
    // узлов не добавит.
    if (layer.wet && 2 * layer.wetHighWater > layer.wet->size())
        layer.wet->grow(2 * layer.wetHighWater);
    ++m_dryInkNodeRebuilds;
}

// Сухие чернила пересобираются только когда состав штрихов на странице
// изменился: обычная прокрутка и зум их не трогают вовсе. Показано -
// сухой слой и за ним недавние слоты; новые штрихи в конце идут в слоты,
// перенос слотов в сухой слой - по простою (m_flushRecentRequested) или
// когда слоты кончились. Любое другое изменение - стирание, отмена,
// перезагрузка - пересобирает сухой слой и очищает слоты вместе с ним: иначе
// стёртый штрих остался бы виден или штрих нарисовался бы дважды.
// Возвращает true, если узлы действительно менялись.
bool DocumentCanvas::updateDryInkNodes(InkLayerNodes &layer, const QList<StrokeGeometry> &geometry,
                                       bool budgetExhausted, const AttachInkNode &attach)
{
    // Пустой поверхности собирать нечего: первая сборка - и есть пустота, и
    // бюджет кадра на неё не тратится.
    if (!layer.built && geometry.isEmpty() && !layer.dryInk && layer.recentIds.isEmpty()) {
        layer.built = true;
        return false;
    }
    const qsizetype dryCount = layer.dryInkIds.size();
    const qsizetype shownCount = dryCount + layer.recentIds.size();
    const auto idAt = [&geometry](qsizetype index) { return geometry[index].id; };
    bool continues = geometry.size() >= shownCount;
    for (qsizetype index = 0; continues && index < dryCount; ++index)
        continues = idAt(index) == layer.dryInkIds[index];
    for (qsizetype index = 0; continues && index < layer.recentIds.size(); ++index)
        continues = idAt(dryCount + index) == layer.recentIds[index];

    if (continues && geometry.size() == shownCount && layer.built) {
        if (!m_flushRecentRequested || !hasRecentInk(layer) || budgetExhausted)
            return false;
        appendToDryInk(layer, geometry, int(dryCount), attach);
        return true;
    }
    if (budgetExhausted)
        return false;

    // Новые штрихи в конце: в свободные слоты, узлы не добавляются. Геометрия
    // прежних штрихов при тех же id та же (кэш дописывается, а не
    // пересобирается), поэтому трогать показанное незачем.
    if (continues && layer.built) {
        const qsizetype fresh = geometry.size() - shownCount;
        if (layer.recentIds.size() + fresh <= layer.recent->size()) {
            for (qsizetype index = shownCount; index < geometry.size(); ++index) {
                const StrokeGeometry &entry = geometry[index];
                layer.recent->fill(int(layer.recentIds.size()), entry.strip.constData(),
                                   int(entry.strip.size()), entry.color);
                layer.recentIds.append(entry.id);
            }
            // Слоты наполнил не только отрыв пера (он сам взводит перенос), но
            // и повтор или отмена стирания: перенос по простою - и для них.
            // Таймер живёт на потоке UI, а здесь - синхронизация с рендером.
            QMetaObject::invokeMethod(
                this,
                [this] {
                    if (!m_recentFlushTimer.isActive())
                        restartRecentFlush();
                },
                Qt::QueuedConnection);
            return true;
        }
        // Слоты кончились: всё недавнее и новое - в сухой слой сразу.
        appendToDryInk(layer, geometry, int(dryCount), attach);
        return true;
    }

    layer.built = true;
    for (int slot = 0; slot < layer.recentIds.size(); ++slot)
        layer.recent->clear(slot);
    layer.recentIds.clear();
    if (layer.dryInk) {
        layer.dryInk->parent()->removeChildNode(layer.dryInk);
        delete layer.dryInk;
        layer.dryInk = nullptr;
    }
    layer.dryInkIds.clear();
    if (geometry.isEmpty()) {
        ++m_dryInkNodeRebuilds;
        return true;
    }
    appendToDryInk(layer, geometry, 0, attach);
    return true;
}

void DocumentCanvas::updateWetInkNodes(InkLayerNodes &layer, bool active)
{
    const QVector<StrokeTessellator::Vertex> &strip = m_wet.strip();
    InkNodePool &pool = *layer.wet;

    const int needed =
        !active || strip.size() < 4
            ? 0
            : qMax(1, (strip.size() - kWetChunkOverlap + kWetChunkVertices - kWetChunkOverlap - 1)
                          / (kWetChunkVertices - kWetChunkOverlap));

    // Новый штрих - всё с начала, даже если прошлый не успел опустеть: между
    // отрывом и касанием кадра могло не быть.
    if (needed > 0 && layer.wetStrokeSerial != m_wetStrokeSerial) {
        layer.wetStrokeSerial = m_wetStrokeSerial;
        layer.wetSettledCount = 0;
    }

    // Куски за концом полосы пустеют, а не удаляются: полоса укорачивается,
    // когда пересобирается провизорный хвост, а после отрыва пустеют все.
    for (int index = needed; index < layer.wetUsedChunks; ++index)
        pool.clear(index);
    if (needed == 0) {
        layer.wetUsedChunks = 0;
        layer.wetSettledCount = 0;
        return;
    }

    // Пул растёт вдвое: длинный штрих добавляет узлы только при первом
    // переполнении, следующие такие же обходятся без добавления.
    if (needed > pool.size())
        pool.grow(qMax(needed, 2 * pool.size()));
    const int previouslyUsed = layer.wetUsedChunks;
    layer.wetUsedChunks = needed;
    layer.wetHighWater = qMax(layer.wetHighWater, needed);

    // Перезаливается всё, начиная с куска, где на прошлом кадре кончалась
    // окончательная часть полосы: до неё вершины не меняются никогда, а после
    // неё мог смениться весь провизорный хвост (design.md, D5). Кусок, занятый
    // только сейчас, заливается в любом случае.
    const int firstChanged = qMin(layer.wetSettledCount, int(strip.size()));
    const int firstDirty =
        qMin(previouslyUsed,
             qMax(0, (firstChanged - kWetChunkOverlap) / (kWetChunkVertices - kWetChunkOverlap)));
    for (int index = firstDirty; index < needed; ++index) {
        const int start = wetChunkStart(index);
        // Кусок с номером меньше needed всегда начинается раньше двух последних
        // вершин полосы, так что в нём не меньше трёх вершин - один треугольник.
        const int count = qMin(kWetChunkVertices, int(strip.size()) - start);
        pool.fill(index, strip.constData() + start, count, m_penColor);
    }
    layer.wetSettledCount = m_wet.settledVertexCount();
}

// Узлы вкладышей: только у раскрытых вкладышей удерживаемых страниц, в порядке
// перекрытия. Узел вкладыша текстур не держит, поэтому снимается сразу, без
// растягивания по кадрам, как у страниц.
void DocumentCanvas::updateInsertNodes(int &inkRebuilds, bool &deferred)
{
    if (!m_insertsRoot)
        return;

    const qreal borderWidth = qFuzzyIsNull(m_zoom) ? 1.0 : kInsertBorderPixels / m_zoom;
    QSet<quint64> alive;
    QSGNode *previous = nullptr;

    for (quint64 id : std::as_const(m_insertZOrder)) {
        const InkInsert insert = m_ink ? m_ink->insertById(id) : InkInsert();
        if (!insert.open || !m_keepPages.contains(insert.page))
            continue;
        const QRectF rect = insertDocRect(insert);
        if (rect.isEmpty())
            continue;

        InsertSceneNode *node = m_insertNodes.value(id);
        if (!node) {
            node = new InsertSceneNode(id);
            m_insertNodes.insert(id, node);
        }
        // Порядок детей слоя вкладышей - порядок перекрытия.
        if (node->parent() && node->previousSibling() != previous) {
            node->parent()->removeChildNode(node);
        }
        if (!node->parent()) {
            if (previous)
                m_insertsRoot->insertChildNodeAfter(node, previous);
            else
                m_insertsRoot->prependChildNode(node);
        }
        previous = node;

        QMatrix4x4 matrix;
        matrix.translate(float(rect.x()), float(rect.y()));
        node->setMatrix(matrix);
        node->setFrame(rect.size(), borderWidth);

        const AttachInkNode attach = [node](QSGNode *child, InkSublayer sublayer) {
            // Под обрезкой тот же порядок, что на странице: сухие, недавние,
            // мокрый штрих.
            QSGNode *above = nullptr;
            if (sublayer == InkSublayer::Dry)
                above = node->ink.recentInk() ? node->ink.recentInk() : node->ink.wetInk();
            else if (sublayer == InkSublayer::Recent)
                above = node->ink.wetInk();
            if (above && above->parent() == node->clip)
                node->clip->insertChildNodeBefore(child, above);
            else
                node->clip->appendChildNode(child);
        };
        ensureInkPools(node->ink, attach);
        if (updateDryInkNodes(node->ink, m_insertGeometry.value(id),
                              inkRebuilds >= kMaxInkRebuildsPerFrame, attach))
            ++inkRebuilds;
        else if (inkRebuilds >= kMaxInkRebuildsPerFrame)
            deferred = true;
        updateWetInkNodes(node->ink, m_wet.isActive() && m_wetInsert == id);
        alive.insert(id);
    }

    for (auto it = m_insertNodes.begin(); it != m_insertNodes.end();) {
        if (alive.contains(it.key())) {
            ++it;
            continue;
        }
        if (QSGNode *parent = it.value()->parent())
            parent->removeChildNode(it.value());
        delete it.value();
        it = m_insertNodes.erase(it);
    }
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
    m_inkRecordIds.clear();
    m_insertGeometry.clear();
    m_insertDirty.clear();
    m_insertRecordIds.clear();
    m_insertZOrder.clear();

    if (m_ink) {
        connect(m_ink, &InkDocument::pageChanged, this, &DocumentCanvas::onInkPageChanged);
        connect(m_ink, &InkDocument::reloaded, this, &DocumentCanvas::onInkReloaded);
        connect(m_ink, &InkDocument::insertAdded, this,
                [this](quint64 id, int) { onInsertAdded(id); });
        connect(m_ink, &InkDocument::insertChanged, this,
                [this](quint64 id, int) { onInsertChanged(id); });
        connect(m_ink, &InkDocument::insertRemoved, this,
                [this](quint64 id, int) { onInsertRemoved(id); });
        connect(m_ink, &InkDocument::insertInkChanged, this, &DocumentCanvas::onInsertInkChanged);
        for (const InkInsert &insert : m_ink->allInserts())
            m_insertZOrder.append(insert.id);
    }

    updateVisiblePages();
    update();
    emit inkChanged();
}

void DocumentCanvas::setTool(Tool tool)
{
    if (m_tool == tool)
        return;
    // «Вкладыш» - инструмент на одно касание: после установки вкладыша
    // возвращается тот, что был выбран до него.
    if (tool == Tool::Insert)
        m_toolBeforeInsert = m_tool;
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

bool DocumentCanvas::beginInkAt(const QPointF &viewPoint, qreal pressure, bool eraser,
                                qreal timeSeconds)
{
    // Перо в работе - не простой: перенос недавних штрихов подождёт.
    if (m_recentFlushTimer.isActive())
        restartRecentFlush();
    if (!m_ink || m_layout.isEmpty())
        return false;

    // Инерция и штрих несовместимы: перо коснулось - документ встаёт.
    stopFlick();
    setStylusInContact(true);

    if (eraser || m_tool == Tool::Eraser) {
        m_erasedInCurrentStroke.clear();
        // Поверхность ластика выбирается на касании и не меняется до отрыва:
        // начатый во вкладыше стирает только вкладыш, начатый на странице -
        // только страницу (design.md, D3).
        m_eraseInsert = insertAtView(viewPoint);
        eraseAt(viewPoint);
        return true;
    }

    if (m_tool == Tool::Insert) {
        m_placingInsert = true;
        placeInsertAt(viewPoint);
        return true;
    }

    // Штрих, начатый в раскрытом вкладыше, принадлежит вкладышу целиком, даже
    // если перо потом уйдёт за рамку.
    const quint64 insert = insertAtView(viewPoint);
    const int page = insert != 0 ? m_ink->insertById(insert).page
                                 : m_layout.pageAt(viewToDocument(viewPoint));
    if (page < 0)
        return false; // касание в промежутке между страницами не рисует

    Stroke prototype;
    prototype.tool = InkTool::Pen;
    prototype.color = m_penColor;
    prototype.width = m_penWidth;
    prototype.insert = insert;
    // Новые штрихи пера - с заострёнными концами; закон уходит вместе со
    // штрихом в страницу, файл и историю (ink-beauty-pass, design.md, D3).
    prototype.style = StrokeStyle::Tapered;

    m_wetPage = page;
    m_wetInsert = insert;
    m_wetOrigin = insert != 0 ? m_ink->insertById(insert).rect.topLeft() : QPointF();
    // Масштаб фиксируется на начало штриха: пока перо в контакте, жесты не
    // начинаются, поэтому внутри штриха он не меняется.
    m_wet.begin(page, prototype, m_zoom);
    ++m_wetStrokeSerial;
    // Чернила складываются в координатах поверхности - страницы или вкладыша,
    // а не экрана.
    m_wet.addPoint({surfacePoint(viewPoint), pressure, inputTimeSeconds(timeSeconds)});

    // Латентность - от метки события пера, а не от момента обработки
    // (ink-beauty-pass, D1); без метки (-1) - от момента обработки.
    if (m_frameStats)
        m_frameStats->markInput(timeSeconds);
    update();
    emit stylusActiveChanged();
    return true;
}

void DocumentCanvas::continueInkAt(const QPointF &viewPoint, qreal pressure, qreal timeSeconds)
{
    if (m_recentFlushTimer.isActive())
        restartRecentFlush();
    // Начатый штрих доводится до конца тем же инструментом: нажатие кнопки на
    // корпусе посреди линии не должно превращать её в стирание.
    if (m_placingInsert)
        return;
    if (!m_wet.isActive()) {
        if ((m_pointerIsEraser || m_tool == Tool::Eraser) && m_stylusInContact)
            eraseAt(viewPoint);
        return;
    }

    m_wet.addPoint({surfacePoint(viewPoint), pressure, inputTimeSeconds(timeSeconds)});

    // Латентность - от метки события пера, а не от момента обработки
    // (ink-beauty-pass, D1); без метки (-1) - от момента обработки.
    if (m_frameStats)
        m_frameStats->markInput(timeSeconds);
    update();
}

void DocumentCanvas::endInk()
{
    setStylusInContact(false);
    m_placingInsert = false;

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
    m_wetInsert = 0;
    m_wetOrigin = QPointF();
    m_wet.reset();

    if (m_ink && stroke.isValid()) {
        m_ink->addStroke(stroke); // пометит страницу грязной через pageChanged
        // Штрих лёг в недавние слоты; в сухой слой он переедет по простою.
        restartRecentFlush();
    } else {
        update();
    }

    emit stylusActiveChanged();
}

void DocumentCanvas::cancelInk()
{
    setStylusInContact(false);
    m_wet.reset();
    m_wetPage = -1;
    m_wetInsert = 0;
    m_wetOrigin = QPointF();
    m_placingInsert = false;
    m_erasedInCurrentStroke.clear();
    update();
    emit stylusActiveChanged();
}

void DocumentCanvas::eraseAt(const QPointF &viewPoint)
{
    if (!m_ink)
        return;

    QList<quint64> hits;
    if (m_eraseInsert != 0) {
        // Во вкладыше стирается только видимое: точка в его рамке и не закрыта
        // вкладышем поверх. Скрытое обрезкой вслепую не стирается.
        if (insertAtView(viewPoint) != m_eraseInsert)
            return;
        const QPointF local =
            viewToDocument(viewPoint) - insertDocumentRect(m_eraseInsert).topLeft();
        hits = m_ink->strokesNearInInsert(m_eraseInsert, local, m_eraserRadius);
    } else {
        // Под раскрытым вкладышем чернила страницы не видны, и исчезнуть
        // невидимое не должно.
        if (insertAtView(viewPoint) != 0)
            return;
        const QPointF documentPoint = viewToDocument(viewPoint);
        const int page = m_layout.pageAt(documentPoint);
        if (page < 0)
            return;
        hits = m_ink->strokesNear(page, viewToPage(page, viewPoint), m_eraserRadius);
    }
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
    m_inkRecordIds.clear();
    for (auto it = m_pages.cbegin(); it != m_pages.cend(); ++it) {
        m_inkDirtyPages.insert(it.key());
        ensureInkGeometry(it.key());
    }

    // Вкладыши сменились целиком: порядок перекрытия - заново по id.
    if (m_wetInsert != 0)
        cancelInk();
    m_insertGeometry.clear();
    m_insertDirty.clear();
    m_insertRecordIds.clear();
    m_insertZOrder.clear();
    if (m_ink) {
        for (const InkInsert &insert : m_ink->allInserts())
            m_insertZOrder.append(insert.id);
    }
    syncInsertGeometry();
    syncInsertModel();
    update();
}

void DocumentCanvas::ensureInkGeometry(int page)
{
    if (!m_ink)
        return;
    if (m_inkGeometry.contains(page) && !m_inkDirtyPages.contains(page))
        return;

    QList<StrokeGeometry> geometry = m_inkGeometry.take(page);
    QList<quint64> &ids = m_inkRecordIds[page];
    updateGeometryCache(geometry, ids, m_ink->strokesOnPage(page));
    if (!geometry.isEmpty())
        m_inkGeometry.insert(page, geometry);
    m_inkDirtyPages.remove(page);
}

void DocumentCanvas::updateGeometryCache(QList<StrokeGeometry> &geometry, QList<quint64> &ids,
                                         const QList<InkDocument::Record> &records) const
{
    // Записи - это закэшированные и новые в конце: отрыв пера, повтор,
    // отмена стирания. Штрих в модели неизменяем, а его тесселяция от вида не
    // зависит, поэтому геометрия прежних штрихов при тех же id та же, и
    // тесселируются только новые. Сравниваются записи, а не геометрия:
    // вырожденные штрихи в геометрию не попадают.
    const bool appended = !ids.isEmpty() && records.size() > ids.size()
                          && std::equal(ids.cbegin(), ids.cend(), records.cbegin(),
                                        [](quint64 id, const InkDocument::Record &record) {
                                            return id == record.id;
                                        });
    if (!appended) {
        geometry.clear();
        ids.clear();
    }
    const QList<InkDocument::Record> fresh = records.mid(ids.size());
    geometry.append(tessellate(fresh));
    for (const InkDocument::Record &record : fresh)
        ids.append(record.id);
}

QList<DocumentCanvas::StrokeGeometry>
DocumentCanvas::tessellate(const QList<InkDocument::Record> &records) const
{
    QList<StrokeGeometry> geometry;
    StrokeTessellator tessellator;
    m_tessellatedStrokes += int(records.size());
    for (const InkDocument::Record &record : records) {
        tessellator.setWidth(record.stroke.width);
        // Каждый штрих - своим законом толщины: нанесённые до заострения
        // остаются прежней толщины (D3).
        tessellator.setStyle(record.stroke.style);
        StrokeGeometry entry;
        entry.id = record.id;
        entry.color = record.stroke.color;
        entry.strip = tessellator.tessellate(record.stroke.points);
        if (!StrokeTessellator::isDegenerate(entry.strip))
            geometry.append(entry);
    }
    return geometry;
}

void DocumentCanvas::dropInkGeometry(int page)
{
    m_inkGeometry.remove(page);
    m_inkDirtyPages.remove(page);
    m_inkRecordIds.remove(page);
}

// --- Вкладыши -----------------------------------------------------------------

QAbstractItemModel *DocumentCanvas::insertModel() const
{
    return m_insertModel;
}

QRectF DocumentCanvas::insertDocRect(const InkInsert &insert) const
{
    // Вкладыш страницы, которой нет в документе, хранится, но не показывается.
    if (!insert.isValid() || insert.page >= m_layout.pageCount())
        return QRectF();
    return insert.rect.translated(m_layout.pageRect(insert.page).topLeft());
}

QRectF DocumentCanvas::insertDocumentRect(quint64 id) const
{
    return m_ink ? insertDocRect(m_ink->insertById(id)) : QRectF();
}

quint64 DocumentCanvas::insertAtView(const QPointF &viewPoint) const
{
    if (!m_ink)
        return 0;
    // Сверху вниз по порядку перекрытия: касание в месте перекрытия относится
    // к верхнему. Рамка может выходить за свою страницу, поэтому поиск идёт по
    // документу, а не по странице под точкой.
    const QPointF point = viewToDocument(viewPoint);
    for (auto it = m_insertZOrder.crbegin(); it != m_insertZOrder.crend(); ++it) {
        const InkInsert insert = m_ink->insertById(*it);
        if (insert.open && insertDocRect(insert).contains(point))
            return insert.id;
    }
    return 0;
}

QPointF DocumentCanvas::surfacePoint(const QPointF &viewPoint) const
{
    return viewToPage(m_wetPage, viewPoint) - m_wetOrigin;
}

void DocumentCanvas::raiseInsert(quint64 id)
{
    m_insertZOrder.removeOne(id);
    m_insertZOrder.append(id);
}

void DocumentCanvas::placeInsertAt(const QPointF &viewPoint)
{
    // Касание раскрытого вкладыша и промежутка ничего не ставит: инструмент
    // остаётся выбранным, чтобы можно было попасть ещё раз.
    if (!m_ink || insertAtView(viewPoint) != 0)
        return;
    const int page = pageAtView(viewPoint);
    if (page < 0)
        return;

    const QRectF rect(viewToPage(page, viewPoint), defaultInsertSize());
    if (m_ink->addInsert(page, rect) == 0)
        return;
    setTool(m_toolBeforeInsert);
}

void DocumentCanvas::openInsert(quint64 id)
{
    if (!m_ink || !m_ink->hasInsert(id))
        return;
    raiseInsert(id);
    m_ink->setInsertOpen(id, true);
    // Уже раскрытый вкладыш сигнала не даст, а порядок перекрытия сменился.
    syncInsertModel();
    update();
}

void DocumentCanvas::closeInsert(quint64 id)
{
    if (!m_ink || !m_ink->hasInsert(id))
        return;
    // Пустой вкладыш не оставляет значка: случайное касание инструментом не
    // должно мусорить на странице. Удаление - обычное действие истории, и
    // отмена вернёт вкладыш раскрытым (design.md, D7).
    if (m_ink->strokesInInsert(id).isEmpty()) {
        m_ink->removeInsert(id);
        return;
    }
    m_ink->setInsertOpen(id, false);
}

void DocumentCanvas::deleteInsert(quint64 id)
{
    if (m_ink)
        m_ink->removeInsert(id);
}

void DocumentCanvas::resizeInsert(quint64 id, const QPointF &viewPoint)
{
    if (!m_ink || !m_ink->hasInsert(id))
        return;
    const InkInsert insert = m_ink->insertById(id);
    // Левый верхний угол стоит на месте, правый нижний идёт за точкой, но не
    // ближе минимального размера.
    const QPointF corner = viewToPage(insert.page, viewPoint);
    const QSizeF size(qMax(kMinInsertWidth, corner.x() - insert.rect.left()),
                      qMax(kMinInsertHeight, corner.y() - insert.rect.top()));
    m_ink->setInsertRect(id, QRectF(insert.rect.topLeft(), size));
}

int DocumentCanvas::insertGeometryStrokeCount(quint64 id) const
{
    return int(m_insertGeometry.value(id).size());
}

void DocumentCanvas::onInsertAdded(quint64 id)
{
    // Созданный или возвращённый отменой вкладыш встаёт наверх.
    raiseInsert(id);
    m_insertDirty.insert(id);
    syncInsertGeometry();
    syncInsertModel();
    update();
}

void DocumentCanvas::onInsertChanged(quint64 id)
{
    Q_UNUSED(id)
    syncInsertGeometry();
    syncInsertModel();
    update();
}

void DocumentCanvas::onInsertRemoved(quint64 id)
{
    if (m_wet.isActive() && m_wetInsert == id)
        cancelInk();
    m_insertZOrder.removeOne(id);
    m_insertGeometry.remove(id);
    m_insertDirty.remove(id);
    m_insertRecordIds.remove(id);
    syncInsertModel();
    update();
}

void DocumentCanvas::onInsertInkChanged(quint64 id)
{
    // Как у страницы: геометрия пересобирается на потоке UI, иначе законченный
    // штрих появился бы во вкладыше только после ближайшей смены вида.
    m_insertDirty.insert(id);
    syncInsertGeometry();
    update();
}

void DocumentCanvas::ensureInsertGeometry(quint64 id)
{
    if (m_insertGeometry.contains(id) && !m_insertDirty.contains(id))
        return;
    // Пустой список тоже хранится: так пустой раскрытый вкладыш не
    // тесселируется заново на каждом кадре панорамы.
    QList<StrokeGeometry> &geometry = m_insertGeometry[id];
    updateGeometryCache(geometry, m_insertRecordIds[id], m_ink->strokesInInsert(id));
    m_insertDirty.remove(id);
}

// Геометрия чернил вкладышей живёт ровно у раскрытых вкладышей удерживаемых
// страниц: как у страниц, и ещё без свёрнутых - их содержимого не видно.
void DocumentCanvas::syncInsertGeometry()
{
    if (!m_ink) {
        m_insertGeometry.clear();
        m_insertDirty.clear();
        m_insertRecordIds.clear();
        return;
    }
    for (auto it = m_insertGeometry.begin(); it != m_insertGeometry.end();) {
        const InkInsert insert = m_ink->insertById(it.key());
        if (!insert.open || !m_keepPages.contains(insert.page)) {
            m_insertRecordIds.remove(it.key());
            it = m_insertGeometry.erase(it);
        } else {
            ++it;
        }
    }
    for (quint64 id : std::as_const(m_insertZOrder)) {
        const InkInsert insert = m_ink->insertById(id);
        if (insert.open && m_keepPages.contains(insert.page))
            ensureInsertGeometry(id);
    }
}

void DocumentCanvas::syncInsertModel()
{
    if (!m_insertModel)
        return;
    if ((!m_ink || m_ink->insertCount() == 0) && m_insertModel->rowCount() == 0)
        return;

    QList<InsertListModel::Row> rows;
    if (m_ink) {
        for (const InkInsert &insert : m_ink->allInserts()) {
            if (!m_keepPages.contains(insert.page))
                continue;
            const QRectF rect = insertDocRect(insert);
            if (rect.isEmpty())
                continue;
            rows.append({insert.id, insert.page, rect, insert.open,
                         int(m_insertZOrder.indexOf(insert.id))});
        }
    }
    m_insertModel->sync(rows);
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
        beginInkAt(position, pressure, m_pointerIsEraser, eventTimeSeconds(event));
        event->accept();
        return true;
    case QEvent::TabletMove:
        if (m_stylusInContact) {
            continueInkAt(position, pressure, eventTimeSeconds(event));
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
        beginInkAt(event->position(), intent.pressure, intent.eraser, eventTimeSeconds(event));
        event->accept();
        return true;
    case QEvent::MouseMove:
        if (!m_stylusInContact || !intent.contact)
            return false;
        continueInkAt(event->position(), intent.pressure, eventTimeSeconds(event));
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
