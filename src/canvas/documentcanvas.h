#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QImage>
#include <QPair>

#include <functional>
#include <QPointF>
#include <QQmlEngine>
#include <QElapsedTimer>
#include <QQuickItem>
#include <QSet>
#include <QTimer>

#include "canvas/documentlayout.h"
#include "ink/inkdocument.h"
#include "ink/inktypes.h"
#include "ink/wetstroke.h"
#include "pdf/pdfdocumentmodel.h"
#include "perf/framestats.h"

class InsertListModel;
class InsertSceneNode;
class PageRenderService;
class PageSceneNode;
struct InkLayerNodes;

// Холст документа. Страницы живут в графе сцены как узлы с текстурой; зум и
// панорама - это матрица трансформации корневого узла, а не перерисовка
// содержимого (design.md, D5). Поэтому масштабирование не стоит ни одного
// растрового прохода и не может уронить кадр.
//
// Система координат документа - точки PDF, та же, что у DocumentLayout.
// Пересчёт в пиксели элемента: view = doc * zoom + origin.
class DocumentCanvas : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PdfDocumentModel *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(PageMode pageMode READ pageMode WRITE setPageMode NOTIFY pageModeChanged)
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY viewChanged)
    Q_PROPERTY(qreal minZoom READ minZoom CONSTANT)
    Q_PROPERTY(qreal maxZoom READ maxZoom CONSTANT)
    Q_PROPERTY(int currentPage READ currentPage NOTIFY currentPageChanged)
    Q_PROPERTY(qreal originX READ originX NOTIFY viewChanged)
    Q_PROPERTY(qreal originY READ originY NOTIFY viewChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY documentChanged)
    Q_PROPERTY(int liveTextureCount READ liveTextureCount NOTIFY texturesChanged)
    Q_PROPERTY(int retainedPageCount READ retainedPageCount NOTIFY texturesChanged)
    Q_PROPERTY(int visiblePageCount READ visiblePageCount NOTIFY texturesChanged)
    Q_PROPERTY(int placeholderPageCount READ placeholderPageCount NOTIFY texturesChanged)
    Q_PROPERTY(FrameStats *frameStats READ frameStats WRITE setFrameStats NOTIFY frameStatsChanged)
    Q_PROPERTY(bool flicking READ isFlicking NOTIFY flickingChanged)
    Q_PROPERTY(InkDocument *ink READ ink WRITE setInk NOTIFY inkChanged)
    Q_PROPERTY(Tool tool READ tool WRITE setTool NOTIFY toolChanged)
    // Инструмент, которым рисуют прямо сейчас: перевёрнутый конец пера делает
    // его ластиком, не трогая выбор пользователя.
    Q_PROPERTY(Tool activeTool READ activeTool NOTIFY toolChanged)
    Q_PROPERTY(qreal penWidth READ penWidth WRITE setPenWidth NOTIFY penChanged)
    Q_PROPERTY(qreal minPenWidthValue READ minPenWidth CONSTANT)
    Q_PROPERTY(qreal maxPenWidthValue READ maxPenWidth CONSTANT)
    Q_PROPERTY(QColor penColor READ penColor WRITE setPenColor NOTIFY penChanged)
    Q_PROPERTY(qreal eraserRadius READ eraserRadius WRITE setEraserRadius NOTIFY penChanged)
    // Пока стилус в контакте, жесты не начинаются: приоритет у штриха.
    Q_PROPERTY(bool stylusActive READ isStylusActive NOTIFY stylusActiveChanged)
    Q_PROPERTY(bool drawing READ isDrawing NOTIFY stylusActiveChanged)
    Q_PROPERTY(int inkGeometryPageCount READ inkGeometryPageCount NOTIFY texturesChanged)
    // Вкладыши зоны удержания для элементов управления в QML.
    Q_PROPERTY(QAbstractItemModel *insertModel READ insertModel CONSTANT)

public:
    enum class Tool {
        Pen,
        Eraser,
        // На одно касание: ставит вкладыш и возвращает прежний инструмент.
        Insert,
    };
    Q_ENUM(Tool)

    enum class PageMode {
        Single = int(DocumentLayout::Mode::Single),
        Spread = int(DocumentLayout::Mode::Spread),
    };
    Q_ENUM(PageMode)

    explicit DocumentCanvas(QQuickItem *parent = nullptr);
    ~DocumentCanvas() override;

    PdfDocumentModel *document() const { return m_document; }
    void setDocument(PdfDocumentModel *document);

    PageMode pageMode() const { return PageMode(m_layout.mode()); }
    void setPageMode(PageMode mode);

    qreal zoom() const { return m_zoom; }
    void setZoom(qreal zoom);
    qreal minZoom() const { return kMinZoom; }
    qreal maxZoom() const { return kMaxZoom; }

    int currentPage() const { return m_currentPage; }
    qreal originX() const { return m_origin.x(); }
    qreal originY() const { return m_origin.y(); }

    // Восстанавливает сохранённое состояние просмотра. Без сохранённого
    // состояния просто вписывает первую страницу.
    Q_INVOKABLE void restoreViewState(int page, qreal zoom, int pageMode, qreal originX,
                                      qreal originY, bool valid);
    int pageCount() const { return m_layout.pageCount(); }
    // Страницы, у которых текстура уже есть.
    int liveTextureCount() const;
    // Страницы, которые держатся в памяти: видимые плюс зона удержания вокруг.
    int retainedPageCount() const { return int(m_pages.size()); }
    // Память под растрами удерживаемых страниц: и под теми, что уже в
    // текстурах, и под теми, что ждут загрузки. Страницы, снятые с зоны
    // удержания, сюда не входят: их текстуры живут ещё несколько кадров, пока
    // узлы снимаются с графа сцены, но удержанием это уже не является и
    // вытеснением не управляется.
    qint64 retainedRasterBytes() const;
    // Потолок памяти под растры. Меняется только тестами: с заниженным
    // бюджетом вытеснение проверяется, не занимая 600 МБ. Пользователю никаких
    // настроек памяти не предлагается - в приложении бюджет всегда
    // kRasterMemoryBudget.
    qint64 rasterMemoryBudget() const { return m_rasterBudget; }
    void setRasterMemoryBudget(qint64 bytes);
    int visiblePageCount() const { return int(m_visiblePages.size()); }
    // Видимые страницы, которые сейчас показаны заполнителем, а не растром.
    int placeholderPageCount() const;

    // Размер текстуры страницы в пикселях; пустой размер - текстуры ещё нет.
    QSize textureSizeOf(int page) const;
    // Сколько штрихов страницы уже переведены в готовую геометрию.
    Q_INVOKABLE int inkGeometryStrokeCount(int page) const;
    // Готовая полоса штриха страницы из кэша геометрии; пустая - штриха там
    // нет. По ней тесты видят, каким законом толщины штрих нарисован.
    QVector<StrokeTessellator::Vertex> inkGeometryStrip(int page, quint64 id) const;
    // Как чернила страницы стоят относительно её растра в графе сцены.
    // «Поверх» - утверждение сразу о двух слоях, поэтому отсутствие любого из
    // них - это отдельный ответ, а не «не поверх». Мокрый штрих при этом обязан
    // стоять поверх сухих чернил.
    enum class InkLayering {
        Incomplete = -1, // в графе сцены нет либо растра, либо чернил
        BelowRaster = 0, // чернила закрыты растром страницы
        AboveRaster = 1, // чернила видны
    };
    Q_ENUM(InkLayering)
    Q_INVOKABLE InkLayering inkLayering(int page) const;
    // Сколько узлов страниц ждёт снятия с графа сцены.
    Q_INVOKABLE int pendingNodeRemovalCount() const;
    // Докуда узлы мокрого штриха в графе сцены покрывают его полосу, в
    // вершинах; -1 - узла нет. После кадра обязано совпадать с длиной полосы:
    // кусок со старой геометрией за её концом дал бы число больше.
    Q_INVOKABLE int wetInkCoveredVertexCount(int page) const;
    int wetStripVertexCount() const { return int(m_wet.strip().size()); }

    FrameStats *frameStats() const { return m_frameStats; }
    void setFrameStats(FrameStats *stats);

    const DocumentLayout &layout() const { return m_layout; }

    InkDocument *ink() const { return m_ink; }
    void setInk(InkDocument *ink);

    Tool tool() const { return m_tool; }
    void setTool(Tool tool);
    Tool activeTool() const { return m_pointerIsEraser ? Tool::Eraser : m_tool; }

    qreal penWidth() const { return m_penWidth; }
    void setPenWidth(qreal width);
    // Диапазон толщины в точках страницы. Нижняя граница выбрана так, чтобы
    // мелкие пометки на увеличенном масштабе читались, а не сливались.
    static constexpr qreal minPenWidth() { return 0.4; }
    static constexpr qreal maxPenWidth() { return 12.0; }
    QColor penColor() const { return m_penColor; }
    void setPenColor(const QColor &color);
    qreal eraserRadius() const { return m_eraserRadius; }
    void setEraserRadius(qreal radius);

    bool isStylusActive() const { return m_stylusInContact; }
    Q_INVOKABLE bool isDrawing() const { return m_wet.isActive(); }
    int inkGeometryPageCount() const { return int(m_inkGeometry.size()); }

    // Что означает указатель в данном состоянии. Вынесено из обработчика
    // событий отдельно, потому что именно здесь живёт вся специфика устройства:
    // перо на целевой машине приходит мышиным событием, давление берётся из
    // точки, ластик включается перевёрнутым концом или кнопкой на корпусе.
    struct PointerIntent
    {
        bool stylus = false;   // это перо, а не мышь
        bool eraser = false;   // включён ластик: конец пера или кнопка
        bool contact = false;  // перо касается поверхности
        qreal pressure = -1.0; // -1 означает «давление неизвестно»
    };

    static PointerIntent intentFor(const QPointingDevice *device, Qt::MouseButtons buttons,
                                   qreal pointPressure);

    // Ввод, разложенный по смыслу, а не по устройству: тесты подают его
    // напрямую, событийные обработчики - через разбор QPointerEvent.
    //
    // timeSeconds - время отсчёта пера. Обработчики событий подают метку
    // времени самого события: пачка событий, разобранная за один проход
    // цикла, иначе получила бы почти одно время, и модель штриха увидела бы
    // скачок скорости, которого не было. Отрицательное значение - «сейчас»:
    // так зовут сценарный ввод и тесты, которым время отсчёта не важно.
    Q_INVOKABLE bool beginInkAt(const QPointF &viewPoint, qreal pressure, bool eraser,
                                qreal timeSeconds = -1.0);
    Q_INVOKABLE void continueInkAt(const QPointF &viewPoint, qreal pressure,
                                   qreal timeSeconds = -1.0);
    Q_INVOKABLE void endInk();
    Q_INVOKABLE void cancelInk();

    // Вкладыши (design.md, D3-D6, D9). Раскрытие поднимает вкладыш наверх;
    // сворачивание пустого вкладыша удаляет его обычным действием истории.
    QAbstractItemModel *insertModel() const;
    Q_INVOKABLE void openInsert(quint64 id);
    Q_INVOKABLE void closeInsert(quint64 id);
    Q_INVOKABLE void deleteInsert(quint64 id);
    // Правый нижний угол рамки - в точку элемента, левый верхний на месте.
    Q_INVOKABLE void resizeInsert(quint64 id, const QPointF &viewPoint);
    // Верхний раскрытый вкладыш под точкой элемента; 0 - такого нет.
    Q_INVOKABLE quint64 insertAtView(const QPointF &viewPoint) const;
    // Рамка вкладыша в координатах документа.
    Q_INVOKABLE QRectF insertDocumentRect(quint64 id) const;
    // Порядок перекрытия снизу вверх.
    QList<quint64> insertZOrder() const { return m_insertZOrder; }
    static QSizeF defaultInsertSize() { return {kDefaultInsertWidth, kDefaultInsertHeight}; }
    static QSizeF minInsertSize() { return {kMinInsertWidth, kMinInsertHeight}; }

    // Сколько штрихов вкладыша уже переведены в готовую геометрию.
    Q_INVOKABLE int insertGeometryStrokeCount(quint64 id) const;
    // Где узел вкладыша стоит в графе сцены. «Поверх страниц» - слой
    // вкладышей идёт после слоя страниц, а чернила вкладыша - под обрезкой.
    enum class InsertLayering {
        Missing = -1,   // узла вкладыша в графе сцены нет
        BelowPages = 0, // слой вкладышей не последний
        AbovePages = 1,
        Unclipped = 2,  // чернила вкладыша вне узла обрезки
    };
    Q_ENUM(InsertLayering)
    Q_INVOKABLE InsertLayering insertLayering(quint64 id) const;
    Q_INVOKABLE int wetInsertCoveredVertexCount(quint64 id) const;

    // Масштабирование вокруг точки элемента: точка документа под ней остаётся
    // на месте - требование сценария pinch из canvas-view.
    Q_INVOKABLE void zoomAt(const QPointF &viewPoint, qreal newZoom);
    Q_INVOKABLE void zoomBy(const QPointF &viewPoint, qreal factor);
    Q_INVOKABLE void panBy(const QPointF &deltaPixels);

    // Инерция после броска: документ продолжает движение и плавно встаёт.
    // Скорость - в пикселях в секунду, как её отдаёт centroid.velocity.
    Q_INVOKABLE void flick(const QPointF &velocityPixelsPerSecond);
    Q_INVOKABLE void stopFlick();
    bool isFlicking() const { return m_flickVelocity != QPointF(); }

    // Показать страницу целиком, вписав её в окно.
    Q_INVOKABLE void goToPage(int page);
    Q_INVOKABLE void fitPage(int page);

    Q_INVOKABLE QPointF viewToDocument(const QPointF &viewPoint) const;
    Q_INVOKABLE QPointF documentToView(const QPointF &documentPoint) const;
    // Точка в координатах страницы: чернила хранятся именно в них.
    Q_INVOKABLE QPointF viewToPage(int page, const QPointF &viewPoint) const;
    Q_INVOKABLE int pageAtView(const QPointF &viewPoint) const;

    QRectF visibleDocumentRect() const;

signals:
    void documentChanged();
    void flickingChanged();
    void inkChanged();
    void toolChanged();
    void penChanged();
    void stylusActiveChanged();
    void pageModeChanged();
    void viewChanged();
    void currentPageChanged();
    void texturesChanged();
    void frameStatsChanged();

protected:
    bool event(QEvent *event) override;
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void releaseResources() override;

private:
    static constexpr qreal kMinZoom = 0.25;
    static constexpr qreal kMaxZoom = 8.0;
    // Зона удержания текстур отсчитывается строками раскладки вокруг видимых,
    // а не высотами окна: видимая область в координатах документа сжимается с
    // ростом масштаба, и зона, считаемая от неё, на большом зуме схлопывалась бы
    // до одной страницы - соседи выгружались бы ровно тогда, когда они нужнее
    // всего, на обратном раззуме.
    static constexpr int kRetentionRows = 6;
    // Строки, которые не вытесняются даже при исчерпанном бюджете памяти:
    // соседние с видимыми. Ниже этой границы вытеснение не опускается, иначе
    // очередь ещё не освобождённых текстур могла бы выгрести зону подчистую.
    static constexpr int kMinRetentionRows = 1;
    // Потолок памяти под растры удерживаемых страниц. Страница A4 на 800%
    // упирается в kMaxTextureEdge и весит 4096*4096*4 = 67 МБ, так что без
    // потолка расширенная зона на предельном масштабе съела бы под гигабайт.
    static constexpr qint64 kRasterMemoryBudget = qint64(600) * 1024 * 1024;
    // Рамка нового вкладыша и минимальная: две строки рукописи и место под
    // кнопки. В точках страницы (design.md, D3).
    static constexpr qreal kDefaultInsertWidth = 200.0;
    static constexpr qreal kDefaultInsertHeight = 140.0;
    static constexpr qreal kMinInsertWidth = 48.0;
    static constexpr qreal kMinInsertHeight = 36.0;

    struct PageEntry
    {
        QImage image;        // готовый растр, ждущий превращения в текстуру
        QSize textureSize;   // размер текстуры, которая уже в графе сцены
        QSize requestedSize; // размер, под который сделан запрос
        bool imageDirty = false;

        // Память записи: текстура плюс ещё не сданный в неё растр. Размер по
        // умолчанию - (-1, -1), поэтому пустой он или нет, проверяется явно.
        qint64 bytes() const
        {
            const qint64 texture = textureSize.isEmpty()
                ? 0
                : qint64(textureSize.width()) * textureSize.height() * 4;
            return texture + image.sizeInBytes();
        }
    };

    void rebuildLayout();
    void updateVisiblePages();
    // Строки зоны удержания: от первой видимой минус margin до последней
    // видимой плюс margin.
    QPair<int, int> retentionRowRange(int margin) const;
    // Снимает растр со страницы, оставляя её узел графу сцены на снятие.
    void dropPageRaster(int page);
    // Вытесняет дальние страницы, пока растры не уложатся в бюджет памяти.
    void evictOverBudget();
    void requestRenders(bool immediate);
    void onPageRendered(int page, const QSize &pixelSize, const QImage &image);
    void invalidateTextures();
    void stepFlick();

    bool handleTabletEvent(QTabletEvent *event);
    bool handleMouseEvent(QMouseEvent *event);
    bool handleNativeGesture(QNativeGestureEvent *event);
    void setStylusInContact(bool inContact);
    void setPointerIsEraser(bool eraser);
    void eraseAt(const QPointF &viewPoint);

    void onInkPageChanged(int page);
    void onInkReloaded();
    void ensureInkGeometry(int page);
    void dropInkGeometry(int page);
    void placeInsertAt(const QPointF &viewPoint);
    void raiseInsert(quint64 id);
    QRectF insertDocRect(const InkInsert &insert) const;
    // Точка элемента в координатах поверхности мокрого штриха.
    QPointF surfacePoint(const QPointF &viewPoint) const;
    void onInsertAdded(quint64 id);
    void onInsertChanged(quint64 id);
    void onInsertRemoved(quint64 id);
    void onInsertInkChanged(quint64 id);
    void ensureInsertGeometry(quint64 id);
    void syncInsertGeometry();
    void syncInsertModel();
    void updateInsertNodes(int &inkRebuilds, bool &deferred);
    void clampOrigin();
    void updateCurrentPage();
    QSize targetPixelSize(int page) const;

    PdfDocumentModel *m_document = nullptr;
    PageRenderService *m_renderer = nullptr;
    FrameStats *m_frameStats = nullptr;
    DocumentLayout m_layout;

    qreal m_zoom = 1.0;
    QPointF m_origin;
    int m_currentPage = 0;

    QPointF m_flickVelocity;
    QElapsedTimer m_flickClock;
    QTimer m_flickTimer;

    // Пинч на тачпаде. Масштаб жеста копится отдельно от m_zoom и выставляется
    // от начала жеста: zoom = startZoom * scale, где scale = 1 + сумма value().
    // Тачпад шлёт приращения, и их сумма - ровно масштаб жеста, поэтому пинч
    // «туда и обратно» возвращает исходный масштаб. Умножение m_zoom на
    // (1 + value) так не умеет (design.md, D3).
    bool m_gestureActive = false;
    qreal m_gestureStartZoom = 1.0;
    qreal m_gestureScale = 1.0;

    // Тесселированные штрихи страницы. Кэш живёт ровно на тех страницах, что
    // держатся в зоне удержания: документ на 200 страниц не должен хранить
    // геометрию всех чернил сразу.
    struct StrokeGeometry
    {
        quint64 id = 0;
        QColor color;
        QVector<StrokeTessellator::Vertex> strip;
    };

    InkDocument *m_ink = nullptr;
    Tool m_tool = Tool::Pen;
    bool m_pointerIsEraser = false;
    bool m_stylusInContact = false;
    qreal m_penWidth = 1.0;
    // Совпадает с первым цветом палитры в UI, чтобы при старте
    // выбранный цвет был виден как выбранный.
    QColor m_penColor = QColor(0x1c, 0x2a, 0x5a);
    qreal m_eraserRadius = 6.0;

    WetStroke m_wet;
    int m_wetPage = -1;
    QList<quint64> m_erasedInCurrentStroke;

    QHash<int, QList<StrokeGeometry>> m_inkGeometry;
    QSet<int> m_inkDirtyPages;

    QList<StrokeGeometry> tessellate(const QList<InkDocument::Record> &records) const;

    // Куда включить узел чернил поверхности: у страницы - на место слоя, у
    // вкладыша - под узел обрезки. Страница и вкладыш заливают чернила одним
    // кодом (design.md, D4).
    using AttachInkNode = std::function<void(QSGNode *child, bool wet)>;
    bool updateDryInkNodes(InkLayerNodes &layer, const QList<StrokeGeometry> &geometry,
                           bool budgetExhausted, const AttachInkNode &attach);
    void updateWetInkNodes(InkLayerNodes &layer, bool active, const AttachInkNode &attach);

    // Вкладыши. Инструмент до «Вкладыша» возвращается после установки.
    Tool m_toolBeforeInsert = Tool::Pen;
    // Касание инструментом «Вкладыш»: до отрыва пера оно ничего не рисует и
    // не стирает, даже если инструмент уже вернулся к ластику.
    bool m_placingInsert = false;
    // Поверхность мокрого штриха: вкладыш (0 - страница) и начало его
    // локальной системы в координатах страницы.
    quint64 m_wetInsert = 0;
    QPointF m_wetOrigin;
    // Поверхность ластика, выбранная на касании: вкладыш или 0 - страница.
    quint64 m_eraseInsert = 0;
    // Порядок перекрытия снизу вверх: создание и раскрытие переносят в конец.
    QList<quint64> m_insertZOrder;
    // Геометрия чернил раскрытых вкладышей удерживаемых страниц.
    QHash<quint64, QList<StrokeGeometry>> m_insertGeometry;
    QSet<quint64> m_insertDirty;
    // Страницы зоны удержания: по ним живут геометрия, узлы и модель вкладышей.
    QSet<int> m_keepPages;
    InsertListModel *m_insertModel = nullptr;

    // Постоянные слои корня и узлы вкладышей. Трогаются только из
    // updatePaintNode.
    QSGNode *m_pagesRoot = nullptr;
    QSGNode *m_insertsRoot = nullptr;
    QHash<quint64, InsertSceneNode *> m_insertNodes;

    QHash<int, PageEntry> m_pages;
    qint64 m_rasterBudget = kRasterMemoryBudget;
    QList<int> m_visiblePages;
    // Страницы, чьи узлы надо снять с графа сцены. Узлы снимаются не сразу, а
    // по одному за кадр, и пока узел не снят, его текстура жива - поэтому
    // вместе со страницей здесь лежит и её учётная запись: страница, успевшая
    // вернуться, забирает текстуру назад, а не рисует растр заново.
    QHash<int, PageEntry> m_droppedPages;

    // Узлы страниц. Трогаются только из updatePaintNode, то есть на потоке
    // рендера при остановленном потоке UI.
    QHash<int, PageSceneNode *> m_nodes;

    // Перерендер под новый масштаб откладывается: во время непрерывного зума
    // запрашивать каждый промежуточный масштаб бессмысленно и дорого.
    QTimer m_lodTimer;
};
