// ink-beauty-pass, D7: материал чернил на настоящем RHI. Граф сцены Qt Quick
// рисует полосу через QQuickRenderControl в текстуру - окна на экране нет, -
// и профиль покрытия поперёк штриха сверяется с CPU-зеркалом шейдеров
// (src/ink/inkcoverage.h). Зум меняет только матрицу узла: геометрия та же,
// а кромка обязана пересчитаться в шейдере. 80 узлов - чтобы узел зума стал
// корнем батча, и полосы прошли через слитый батч батч-рендера.
//
// Под QT_QPA_PLATFORM=offscreen RHI обычно нет (программный бэкенд) - тогда
// SKIP. Проверка на GPU: QT_QPA_PLATFORM=wayland ./tst_inkmaterialgpu.
//
// Там же - цветные чернила на тёмном фоне: каждый канал поперёк штриха -
// смешивание over премультиплицированного цвета (ink-input, «Цветные чернила
// на тёмной странице»).
//
// Второй тест - батчинг на настоящем RHI с холстом приложения: журнал
// заливки батч-рендера (QSG_RENDERER_DEBUG=upload) перехватывается, и
// проверяется, что событие пера и касание новым штрихом не перезаливают
// слитый батч сухих чернил страницы (design.md ink-beauty-pass, D7).
// Программный бэкенд графа сцены (tst_pagescenegraph) батчей не строит,
// поэтому это проверяется только здесь.

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <QSGSimpleRectNode>
#include <QSGTransformNode>
#include <QTest>

#include <rhi/qrhi.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <memory>

#include "canvas/documentcanvas.h"
#include "canvas/inkmaterial.h"
#include "canvas/inknodepool.h"
#include "fixtures.h"
#include "ink/inkdocument.h"
#include "ink/inkcoverage.h"
#include "ink/stroketessellator.h"
#include "pdf/pdfdocumentmodel.h"

namespace {

constexpr int kStrokes = 80;
constexpr qreal kFirstAxisY = 30.3; // ось первого штриха, точки страницы
constexpr qreal kHalfWidth = 1.0;   // перо 2 pt, полный нажим

// Горизонтальные штрихи под одним узлом зума.
class InkItem : public QQuickItem
{
public:
    InkItem() { setFlag(ItemHasContents); }
    qreal zoom = 1.0;

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override
    {
        QMatrix4x4 matrix;
        matrix.scale(float(zoom));
        if (old) {
            static_cast<QSGTransformNode *>(old)->setMatrix(matrix);
            return old;
        }
        auto *root = new QSGTransformNode;
        root->setMatrix(matrix);
        StrokeTessellator tessellator;
        tessellator.setWidth(2.0 * kHalfWidth);
        tessellator.setMinWidthFactor(0.0);
        for (int k = 0; k < kStrokes; ++k) {
            QList<InkPoint> points;
            for (int i = 0; i <= 40; ++i)
                points.append({QPointF(20 + i * 4.0, kFirstAxisY + k * 5.0), 1.0, i / 200.0});
            const QVector<AxisVertex> strip = tessellator.tessellate(points);
            auto *node = new QSGGeometryNode;
            auto *geometry = new QSGGeometry(InkMaterial::attributes(), int(strip.size()));
            geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
            std::memcpy(geometry->vertexData(), strip.constData(),
                        size_t(strip.size()) * sizeof(AxisVertex));
            node->setGeometry(geometry);
            node->setFlag(QSGNode::OwnsGeometry);
            auto *material = new InkMaterial;
            material->setColor(Qt::black);
            node->setMaterial(material);
            node->setFlag(QSGNode::OwnsMaterial);
            root->appendChildNode(node);
        }
        return root;
    }
};

// Сцена Qt Quick, которая рисуется в текстуру через QQuickRenderControl:
// окна на экране нет, а рендер - настоящий RHI.
class OffscreenScene
{
public:
    explicit OffscreenScene(const QSize &size)
        : m_size(size)
        , m_window(&m_control)
    {
        m_window.setColor(Qt::white);
        m_window.resize(size);
    }

    QQuickWindow &window() { return m_window; }

    // false - RHI нет (программный бэкенд под offscreen).
    bool initialize()
    {
        if (!m_control.initialize() || !m_control.rhi())
            return false;
        QRhi *rhi = m_control.rhi();
        qInfo("RHI %s, %s", rhi->backendName(), rhi->driverInfo().deviceName.constData());

        m_texture.reset(rhi->newTexture(QRhiTexture::RGBA8, m_size, 1,
                                        QRhiTexture::RenderTarget
                                            | QRhiTexture::UsedAsTransferSource));
        if (!m_texture->create())
            return false;
        m_depth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, m_size, 1));
        if (!m_depth->create())
            return false;
        QRhiTextureRenderTargetDescription description{QRhiColorAttachment(m_texture.get())};
        description.setDepthStencilBuffer(m_depth.get());
        m_target.reset(rhi->newTextureRenderTarget(description));
        m_pass.reset(m_target->newCompatibleRenderPassDescriptor());
        m_target->setRenderPassDescriptor(m_pass.get());
        if (!m_target->create())
            return false;
        m_window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(m_target.get()));
        return true;
    }

    // Кадр со считыванием цели в память.
    QImage frame()
    {
        QRhi *rhi = m_control.rhi();
        QCoreApplication::processEvents();
        m_control.polishItems();
        m_control.beginFrame();
        m_control.sync();
        m_control.render();
        QRhiReadbackResult readback;
        QRhiResourceUpdateBatch *batch = rhi->nextResourceUpdateBatch();
        batch->readBackTexture({m_texture.get()}, &readback);
        m_control.commandBuffer()->resourceUpdate(batch);
        m_control.endFrame();
        QImage image(reinterpret_cast<const uchar *>(readback.data.constData()),
                     readback.pixelSize.width(), readback.pixelSize.height(),
                     QImage::Format_RGBA8888_Premultiplied);
        QImage copy = image.convertToFormat(QImage::Format_RGB32);
        return rhi->isYUpInFramebuffer() ? copy.flipped(Qt::Vertical) : copy;
    }

    void frames(int count)
    {
        for (int i = 0; i < count; ++i)
            frame();
    }

    // Растр страницы и штрихи доезжают с рабочих потоков - кадрами.
    bool framesUntil(const std::function<bool()> &predicate, int timeoutMs = 15000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < timeoutMs) {
            QTest::qWait(5);
            frame();
        }
        return predicate();
    }

private:
    QSize m_size;
    QQuickRenderControl m_control;
    QQuickWindow m_window;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiRenderBuffer> m_depth;
    std::unique_ptr<QRhiTextureRenderTarget> m_target;
    std::unique_ptr<QRhiRenderPassDescriptor> m_pass;
};

// Цветные штрихи поверх тёмного прямоугольника (ink-input, «Цветные чернила
// на тёмной странице»): красный и зелёный полной непрозрачности и красный под
// узлом непрозрачности 0.5 - по горизонтальному штриху на каждый.
class DarkPageItem : public QQuickItem
{
public:
    static constexpr QRgb kBackground = 0xff101010;
    static constexpr qreal kHalfWidth = 2.0;

    struct Stroke
    {
        qreal axisY;
        QColor color;
        qreal opacity;
    };
    static constexpr int kStrokeCount = 3;
    static Stroke stroke(int i)
    {
        const Stroke strokes[kStrokeCount] = {{50.3, QColor(Qt::red), 1.0},
                                              {100.6, QColor(Qt::green), 1.0},
                                              {150.3, QColor(Qt::red), 0.5}};
        return strokes[i];
    }

    DarkPageItem() { setFlag(ItemHasContents); }

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override
    {
        if (old)
            return old;
        auto *root = new QSGNode;
        root->appendChildNode(new QSGSimpleRectNode(boundingRect(), QColor(kBackground)));
        StrokeTessellator tessellator;
        tessellator.setWidth(2.0 * kHalfWidth);
        tessellator.setMinWidthFactor(0.0);
        for (int k = 0; k < kStrokeCount; ++k) {
            const Stroke s = stroke(k);
            QList<InkPoint> points;
            for (int i = 0; i <= 40; ++i)
                points.append({QPointF(20 + i * 4.0, s.axisY), 1.0, i / 200.0});
            const QVector<AxisVertex> strip = tessellator.tessellate(points);
            QSGGeometryNode *node = InkNodePool::makeNode(s.color, InkMaterial::Layer::Dry);
            InkNodePool::upload(node, strip.constData(), int(strip.size()));
            if (s.opacity < 1.0) {
                auto *opacity = new QSGOpacityNode;
                opacity->setOpacity(s.opacity);
                opacity->appendChildNode(node);
                root->appendChildNode(opacity);
            } else {
                root->appendChildNode(node);
            }
        }
        return root;
    }
};

// Относительная яркость sRGB (WCAG 2.x) и контраст двух цветов.
qreal relativeLuminance(QRgb c)
{
    const auto linear = [](int v) {
        const qreal x = v / 255.0;
        return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(qRed(c)) + 0.7152 * linear(qGreen(c)) + 0.0722 * linear(qBlue(c));
}

qreal contrast(QRgb a, QRgb b)
{
    const qreal la = relativeLuminance(a);
    const qreal lb = relativeLuminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

// Журнал заливки батчей (QSG_RENDERER_DEBUG=upload, включается в main до
// создания приложения). Пока объект жив, отладочные сообщения рендера
// перехватываются и не попадают в вывод теста; остальное идёт дальше.
class UploadLog
{
public:
    UploadLog()
    {
        s_current = this;
        m_previous = qInstallMessageHandler(&UploadLog::handle);
    }
    ~UploadLog()
    {
        qInstallMessageHandler(m_previous);
        s_current = nullptr;
    }

    void clear() { m_lines.clear(); }

    // Сколько узлов с материалом этого типа залито в батчи с последнего
    // clear(). Строка элемента: "  - uploading element: ... GeometryNode(
    // ... materialtype= 0x... )" - только у слитых батчей, поэтому тест
    // сначала убеждается, что батч сухих чернил вообще в журнале виден.
    // Сколько узлов залито в батчи с последнего clear(), любых.
    int uploadedElements() const
    {
        int count = 0;
        for (const QString &line : m_lines)
            count += line.startsWith(QStringLiteral("  - uploading element:")) ? 1 : 0;
        return count;
    }

    int uploadedElements(const QSGMaterialType *type) const
    {
        static const QRegularExpression pattern(QStringLiteral("materialtype= 0x([0-9a-f]+)"));
        int count = 0;
        for (const QString &line : m_lines) {
            if (!line.startsWith(QStringLiteral("  - uploading element:")))
                continue;
            const QRegularExpressionMatch match = pattern.match(line);
            bool ok = false;
            if (match.hasMatch() && match.captured(1).toULongLong(&ok, 16) == quintptr(type) && ok)
                ++count;
        }
        return count;
    }

private:
    static void handle(QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        if (type == QtDebugMsg && s_current) {
            s_current->m_lines.append(message);
            return;
        }
        if (s_current && s_current->m_previous)
            s_current->m_previous(type, context, message);
    }

    static inline UploadLog *s_current = nullptr;
    QtMessageHandler m_previous = nullptr;
    QStringList m_lines;
};

// Холст приложения на документе-фикстуре в сцене OffscreenScene.
struct CanvasScene
{
    static constexpr QSize kSize{900, 700};

    PdfDocumentModel document;
    InkDocument ink;
    OffscreenScene scene{kSize};
    DocumentCanvas *canvas = nullptr;

    bool open(const QString &pdf)
    {
        if (!document.open(pdf))
            return false;
        canvas = new DocumentCanvas(scene.window().contentItem());
        canvas->setSize(QSizeF(kSize));
        canvas->setDocument(&document);
        canvas->setInk(&ink);
        canvas->setPageMode(DocumentCanvas::PageMode::Single);
        return true;
    }
};

// Насколько кадр after темнее кадра before поперёк горизонтальной оси в точке
// at: максимум по ±4 px.
int darkeningAcross(const QImage &before, const QImage &after, const QPointF &at)
{
    int darkening = 0;
    const int x = int(at.x());
    for (int y = int(at.y()) - 4; y <= int(at.y()) + 4; ++y)
        darkening = qMax(darkening, qRed(before.pixel(x, y)) - qRed(after.pixel(x, y)));
    return darkening;
}

// Плотная страница холста: штрихов столько, что вершин больше порога
// батч-рендера (1024), и узел страницы, который двигается каждый кадр,
// становится корнем батча - как плотная страница приложения. Мелкое
// поддерево без своего корня батч-рендер перезаливает на каждом сдвиге
// целиком, и проверять было бы нечего.
struct DensePage
{
    static constexpr int kDryStrokes = 12;

    explicit DensePage(UploadLog &log)
        : log(log)
    {
    }

    UploadLog &log;
    CanvasScene fixture;
    const InkMaterial dryMaterial;
    const InkMaterial wetMaterial{InkMaterial::Layer::Wet};
    const QSGMaterialType *dry = dryMaterial.type();
    // У недавних штрихов и кусков мокрого штриха тип материала общий.
    const QSGMaterialType *wet = wetMaterial.type();
    qreal time = 3.0;
    QString error;

    bool touch(const QPointF &from)
    {
        return fixture.canvas->pageAtView(from) == 0
               && fixture.canvas->beginInkAt(from, 0.6, false, time);
    }

    void move(const QPointF &from, int first, int last)
    {
        for (int i = first; i <= last; ++i) {
            time += 1.0 / 240.0;
            fixture.canvas->continueInkAt(from + QPointF(i * 2.0, 0.0), 0.6, time);
        }
    }

    // Сухие штрихи в сухом слое, батч сухого слоя виден в журнале - иначе
    // отсутствие строк ничего не значит, - и покой: растр страницы в полном
    // разрешении доехал, кадры ничего не заливают. Смена текстуры страницы
    // перезаливает и чернила под ней (слитый батч текстур соседних страниц
    // перекрывает их порядки), и это не то, что здесь проверяется.
    bool prepare()
    {
        DocumentCanvas *canvas = fixture.canvas;
        OffscreenScene &scene = fixture.scene;
        canvas->setPenWidth(3.0);
        canvas->goToPage(0);
        if (!scene.framesUntil([canvas] { return !canvas->textureSizeOf(0).isEmpty(); })) {
            error = QStringLiteral("страница не отрисовалась");
            return false;
        }
        log.clear();
        for (int k = 0; k < kDryStrokes; ++k) {
            const QPointF from(300, 150 + k * 20.0);
            if (!touch(from)) {
                error = QStringLiteral("касание мимо страницы");
                return false;
            }
            move(from, 1, 60);
            canvas->endInk();
        }
        canvas->flushRecentInk();
        if (!scene.framesUntil(
                [canvas] { return canvas->inkMaterialStats(0).dryNodes == kDryStrokes; })) {
            error = QStringLiteral("сухой слой не собрался");
            return false;
        }
        scene.frames(3);
        if (log.uploadedElements(dry) == 0) {
            error = QStringLiteral("батч сухих чернил в журнале заливки не виден");
            return false;
        }
        // Растр в полном разрешении считается на рабочем потоке и может
        // прийти позже первого тихого кадра: покой - полсекунды тихих кадров.
        int quiet = 0;
        for (int i = 0; i < 600 && quiet < 50; ++i) {
            QTest::qWait(10);
            log.clear();
            scene.frame();
            quiet = log.uploadedElements() == 0 ? quiet + 1 : 0;
        }
        if (quiet >= 50)
            return true;
        error = QStringLiteral("кадры не успокоились");
        return false;
    }
};

// Корень с 80 полосами (корень батча после первого сдвига) и пул из одного
// пустого слота под ним; заливка слота - по флагу, без других изменений.
class PoolItem : public QQuickItem
{
public:
    static constexpr qreal kSlotAxisY = 150.0;

    PoolItem() { setFlag(ItemHasContents); }
    bool touchMatrix = false;
    bool fillSlot = false;

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override
    {
        if (old) {
            auto *root = static_cast<QSGTransformNode *>(old);
            if (touchMatrix) {
                root->setMatrix(QMatrix4x4());
                touchMatrix = false;
            }
            if (fillSlot) {
                StrokeTessellator tessellator;
                tessellator.setWidth(4.0);
                tessellator.setMinWidthFactor(0.0);
                QList<InkPoint> points;
                for (int i = 0; i <= 40; ++i)
                    points.append({QPointF(20 + i * 4.0, kSlotAxisY), 1.0, i / 200.0});
                const QVector<AxisVertex> strip = tessellator.tessellate(points);
                m_pool->fill(0, strip.constData(), int(strip.size()), Qt::black);
                fillSlot = false;
            }
            return old;
        }
        auto *root = new QSGTransformNode;
        StrokeTessellator tessellator;
        tessellator.setWidth(1.0);
        for (int k = 0; k < kStrokes; ++k) {
            QList<InkPoint> points;
            for (int i = 0; i <= 10; ++i)
                points.append({QPointF(220 + i * 4.0, 10 + k * 2.0), 1.0, i / 200.0});
            const QVector<AxisVertex> strip = tessellator.tessellate(points);
            QSGGeometryNode *node = InkNodePool::makeNode(Qt::gray, InkMaterial::Layer::Dry);
            InkNodePool::upload(node, strip.constData(), int(strip.size()));
            root->appendChildNode(node);
        }
        m_pool = std::make_unique<InkNodePool>(InkMaterial::Layer::Wet, 0, 1);
        root->appendChildNode(m_pool->node());
        return root;
    }

private:
    std::unique_ptr<InkNodePool> m_pool;
};

} // namespace

class TestInkMaterialGpu : public QObject
{
    Q_OBJECT

private slots:
    void edgeCoverageMatchesMirrorAtEveryZoom()
    {
        // Журнал заливки этому тесту не нужен - только глушится.
        const UploadLog quiet;
        const QSize size(400, 200);
        OffscreenScene scene(size);
        auto *item = new InkItem;
        item->setParentItem(scene.window().contentItem());
        item->setSize(QSizeF(size));
        if (!scene.initialize())
            QSKIP("RHI недоступен на этой платформе (программный бэкенд)");

        for (const qreal zoom : {1.0, 2.0, 1.5, 3.0}) {
            item->zoom = zoom;
            item->update();
            const QImage image = scene.frame();
            // Device px в точке страницы: зум и dpr цели (у текстуры - 1).
            const qreal k = zoom;
            const int x = int(100 * k);
            const qreal axis = kFirstAxisY * k;
            qreal worst = 0.0;
            qreal massGpu = 0.0;
            qreal massMirror = 0.0;
            for (int y = int(axis - 2 * k); y <= int(axis + 2 * k); ++y) {
                const qreal gpu = 1.0 - qRed(image.pixel(x, y)) / 255.0;
                const qreal mirror = ink::coverage(y + 0.5 - axis, kHalfWidth * k);
                worst = qMax(worst, qAbs(gpu - mirror));
                massGpu += gpu;
                massMirror += mirror;
            }
            QVERIFY2(worst <= 2.0 / 255.0,
                     qPrintable(QStringLiteral("зум %1: ошибка покрытия %2, масса %3 против %4")
                                    .arg(zoom)
                                    .arg(worst)
                                    .arg(massGpu)
                                    .arg(massMirror)));
        }
    }

    // Красные и зелёные чернила на тёмной странице - своего цвета. Каждый
    // канал каждого пикселя поперёк штриха - смешивание over
    // премультиплицированного цвета с покрытием зеркала: c·a·cov + bg·(1 -
    // a·cov). Смешивание Min/Max или непремультиплицированный цвет дали бы
    // другое: Max на кромке красного - зелёный и синий канал фона целиком
    // вместо половины, Min - чернила, погасшие до фона.
    void coloredInkKeepsItsColorOnDarkPage()
    {
        const UploadLog quiet;
        const QSize size(200, 200);
        OffscreenScene scene(size);
        auto *item = new DarkPageItem;
        item->setParentItem(scene.window().contentItem());
        item->setSize(QSizeF(size));
        if (!scene.initialize())
            QSKIP("RHI недоступен на этой платформе (программный бэкенд)");

        const QImage image = scene.frame();
        const QRgb background = DarkPageItem::kBackground;
        QCOMPARE(image.pixel(100, 10), background);
        const int x = 100;
        for (int k = 0; k < DarkPageItem::kStrokeCount; ++k) {
            const DarkPageItem::Stroke stroke = DarkPageItem::stroke(k);
            const qreal hw = DarkPageItem::kHalfWidth;
            int worst = 0;
            for (int y = int(stroke.axisY - hw - 2); y <= int(stroke.axisY + hw + 2); ++y) {
                const qreal alpha =
                    stroke.opacity * ink::coverage(y + 0.5 - stroke.axisY, hw);
                const auto expected = [alpha](qreal ink, int bg) {
                    return int(std::lround((ink * alpha + bg / 255.0 * (1.0 - alpha)) * 255.0));
                };
                const QRgb got = image.pixel(x, y);
                worst = qMax(worst, qAbs(qRed(got) - expected(stroke.color.redF(), qRed(background))));
                worst = qMax(worst,
                             qAbs(qGreen(got) - expected(stroke.color.greenF(), qGreen(background))));
                worst = qMax(worst,
                             qAbs(qBlue(got) - expected(stroke.color.blueF(), qBlue(background))));
            }
            QVERIFY2(worst <= 3,
                     qPrintable(QStringLiteral("штрих %1 (%2, непрозрачность %3): канал разошёлся "
                                               "со смешиванием over на %4/255")
                                    .arg(k)
                                    .arg(stroke.color.name())
                                    .arg(stroke.opacity)
                                    .arg(worst)));

            // Середина штриха - свой цвет: свой канал много больше прочих и
            // контраст с фоном ясно больше единицы.
            const QRgb center = image.pixel(x, int(stroke.axisY));
            const int own = stroke.color.red() > 0 ? qRed(center) : qGreen(center);
            const int other = stroke.color.red() > 0 ? qMax(qGreen(center), qBlue(center))
                                                     : qMax(qRed(center), qBlue(center));
            QVERIFY2(own >= 4 * other && contrast(center, background) > 1.5,
                     qPrintable(QStringLiteral("штрих %1: середина %2, контраст с фоном %3")
                                    .arg(k)
                                    .arg(QColor(center).name())
                                    .arg(contrast(center, background))));
            if (stroke.opacity == 1.0)
                QVERIFY(contrast(center, background) > 4.0);
        }
    }

    // Мокрый слой не трогает батч сухих чернил: ни событие пера на уже
    // занятом куске, ни касание новым штрихом. Добавление узла под корнем
    // батча пересобирает все батчи корня, и слитый батч сухих чернил
    // плотной страницы перезаливался бы на каждом касании.
    void wetInkDoesNotReuploadDryBatch()
    {
        UploadLog log;
        DensePage dense(log);
        QVERIFY(dense.fixture.open(fixtures::large()));
        if (!dense.fixture.scene.initialize())
            QSKIP("RHI недоступен на этой платформе (программный бэкенд)");
        QVERIFY2(dense.prepare(), qPrintable(dense.error));
        OffscreenScene &scene = dense.fixture.scene;
        const QImage before = scene.frame();

        // Касание вторым штрихом: залит кусок мокрого штриха, сухой батч - нет.
        const QPointF second(300, 450);
        log.clear();
        QVERIFY(dense.touch(second));
        dense.move(second, 1, 6);
        scene.frame();
        QVERIFY2(log.uploadedElements(dense.wet) > 0, "мокрый штрих не залит - он не нарисован");
        QVERIFY2(log.uploadedElements(dense.dry) == 0,
                 qPrintable(QStringLiteral("касание перезалило батч сухих чернил: %1 узлов")
                                .arg(log.uploadedElements(dense.dry))));

        // Событие пера на уже занятом куске: только кусок.
        log.clear();
        dense.move(second, 7, 40);
        const QImage image = scene.frame();
        QVERIFY(log.uploadedElements(dense.wet) > 0);
        QVERIFY2(log.uploadedElements(dense.dry) == 0,
                 qPrintable(QStringLiteral("событие пера перезалило батч сухих чернил: %1 узлов")
                                .arg(log.uploadedElements(dense.dry))));
        // И штрих действительно нарисован: поперёк оси кадр темнее, чем до
        // касания.
        const int darkening = darkeningAcross(before, image, second + QPointF(40, 0));
        QVERIFY2(darkening >= 64,
                 qPrintable(QStringLiteral("мокрый штрих не виден: темнее на %1").arg(darkening)));

        dense.fixture.canvas->endInk();
        scene.frames(2);
    }

    // Отрыв пера на плотной странице кладёт штрих в слот недавних штрихов и
    // сухой батч не перезаливает; перенос в сухой слой - один раз, после
    // паузы, и картинка при нём не меняется (ink-beauty-pass, рецензия
    // группы 12: кадр отрыва на странице в 26 МБ - около 12 мс).
    void liftUsesRecentSlotAndIdleMovesItOnce()
    {
        UploadLog log;
        DensePage dense(log);
        QVERIFY(dense.fixture.open(fixtures::large()));
        if (!dense.fixture.scene.initialize())
            QSKIP("RHI недоступен на этой платформе (программный бэкенд)");
        QVERIFY2(dense.prepare(), qPrintable(dense.error));
        OffscreenScene &scene = dense.fixture.scene;
        DocumentCanvas *canvas = dense.fixture.canvas;
        const QImage before = scene.frame();

        // Два штриха с отрывом: на отрыве и после - сухой батч не залит.
        log.clear();
        for (const qreal y : {450.0, 480.0}) {
            const QPointF from(300, y);
            QVERIFY(dense.touch(from));
            dense.move(from, 1, 40);
            scene.frame();
            canvas->endInk();
            scene.frames(2);
        }
        QCOMPARE(canvas->inkMaterialStats(0).recentNodes, 2);
        QVERIFY2(log.uploadedElements(dense.dry) == 0,
                 qPrintable(QStringLiteral("отрыв перезалил батч сухих чернил: %1 узлов")
                                .arg(log.uploadedElements(dense.dry))));
        QVERIFY(log.uploadedElements(dense.wet) > 0);
        const QImage recent = scene.frame();
        QVERIFY(darkeningAcross(before, recent, QPointF(340, 480)) >= 64);

        // Пауза: перенос в сухой слой - ровно в одном кадре.
        int framesWithDry = 0;
        QElapsedTimer timer;
        timer.start();
        while (canvas->inkMaterialStats(0).recentNodes > 0 && timer.elapsed() < 3000) {
            QTest::qWait(10);
            log.clear();
            scene.frame();
            framesWithDry += log.uploadedElements(dense.dry) > 0 ? 1 : 0;
        }
        QCOMPARE(canvas->inkMaterialStats(0).recentNodes, 0);
        QCOMPARE(canvas->inkMaterialStats(0).dryNodes, DensePage::kDryStrokes + 2);
        for (int i = 0; i < 5; ++i) {
            log.clear();
            scene.frame();
            framesWithDry += log.uploadedElements(dense.dry) > 0 ? 1 : 0;
        }
        QCOMPARE(framesWithDry, 1);

        // Тот же шейдер, цвет и порядок: картинка та же.
        const QImage moved = scene.frame();
        int worst = 0;
        for (int y = 0; y < moved.height(); ++y) {
            for (int x = 0; x < moved.width(); ++x) {
                const QRgb a = recent.pixel(x, y);
                const QRgb b = moved.pixel(x, y);
                worst = qMax(worst, qMax(qAbs(qRed(a) - qRed(b)),
                                         qMax(qAbs(qGreen(a) - qGreen(b)),
                                              qAbs(qBlue(a) - qBlue(b)))));
            }
        }
        QVERIFY2(worst <= 2,
                 qPrintable(QStringLiteral("перенос изменил картинку на %1/255").arg(worst)));
    }

    // Длинный штрих на плотной странице занимает шесть кусков мокрого слоя -
    // больше, чем было в пуле 0.10.0 (4). Ни одно событие пера не
    // перезаливает сухой батч, в том числе первые заполнения кусков. Прежде
    // пятый кусок рос пул - добавлял узлы под корнем батча страницы, и её
    // батч в 28 МБ перезаливался посреди штриха, около 20 мс рендера
    // (ink-beauty-pass, прокси-замеры группы 14). Затем перенос недавних
    // штрихов заранее растит пул вдвое против самого длинного штриха.
    void longStrokeNeverReuploadsDryBatch()
    {
        UploadLog log;
        DensePage dense(log);
        QVERIFY(dense.fixture.open(fixtures::large()));
        if (!dense.fixture.scene.initialize())
            QSKIP("RHI недоступен на этой платформе (программный бэкенд)");
        QVERIFY2(dense.prepare(), qPrintable(dense.error));
        OffscreenScene &scene = dense.fixture.scene;
        DocumentCanvas *canvas = dense.fixture.canvas;

        const QPointF from(300, 470);
        QVERIFY(dense.touch(from));
        int sample = 0;
        int dryEvents = 0;
        int maxChunks = 0;
        constexpr int kChunks = 6;
        while (canvas->inkMaterialStats(0).wetNodes < kChunks && sample < 4000) {
            log.clear();
            for (int k = 0; k < 4; ++k) {
                ++sample;
                dense.time += 1.0 / 240.0;
                canvas->continueInkAt(
                    from + QPointF(sample * 0.25, 25.0 * std::sin(sample / 4.0)), 0.6, dense.time);
            }
            scene.frame();
            QVERIFY(log.uploadedElements(dense.wet) > 0);
            if (log.uploadedElements(dense.dry) > 0) {
                ++dryEvents;
                qWarning("перезалит сухой батч на %d-м отсчёте, кусков %d", sample,
                         canvas->inkMaterialStats(0).wetNodes);
            }
            maxChunks = qMax(maxChunks, canvas->inkMaterialStats(0).wetNodes);
        }
        QCOMPARE(maxChunks, kChunks);
        QCOMPARE(dryEvents, 0);
        const int poolBefore = canvas->inkMaterialStats(0).wetPoolNodes;
        canvas->endInk();
        scene.frames(2);

        // Перенос по простою растит пул заранее: вдвое против 6 кусков.
        canvas->flushRecentInk();
        scene.frames(2);
        QCOMPARE(canvas->inkMaterialStats(0).recentNodes, 0);
        QVERIFY(canvas->inkMaterialStats(0).wetPoolNodes >= 2 * kChunks);
        QVERIFY(canvas->inkMaterialStats(0).wetPoolNodes >= poolBefore);
    }

    // Пустой слот пула оживает со штрихом. Пустой элемент батча не имеет, а
    // одна DirtyGeometry у элемента без батча перестройку батчей не
    // заказывает: без DirtyMaterial в InkNodePool::fill слот остаётся
    // невидимым. Корень батча здесь неподвижен, и других движущихся узлов нет
    // - перестройку не закажет никто другой.
    void revivedPoolSlotIsDrawn()
    {
        const UploadLog quiet;
        const QSize size(400, 200);
        OffscreenScene scene(size);
        auto *item = new PoolItem;
        item->setParentItem(scene.window().contentItem());
        item->setSize(QSizeF(size));
        if (!scene.initialize())
            QSKIP("RHI недоступен на этой платформе (программный бэкенд)");

        // Первый кадр строит узлы, второй сдвигает корень - он становится
        // корнем батча, - дальше покой.
        scene.frame();
        item->touchMatrix = true;
        item->update();
        scene.frames(3);
        const QImage empty = scene.frame();

        item->fillSlot = true;
        item->update();
        const QImage filled = scene.frame();
        const int darkening =
            darkeningAcross(empty, filled, QPointF(100, PoolItem::kSlotAxisY));
        QVERIFY2(darkening >= 128,
                 qPrintable(QStringLiteral("оживлённый слот не виден: темнее на %1").arg(darkening)));
    }
};

// Журнал заливки батч-рендер читает из окружения один раз, при первом кадре,
// поэтому переменные ставятся до создания приложения. QT_FORCE_STDERR_LOGGING
// - чтобы отладочный вывод рендера не уходил в журнал системы.
int main(int argc, char *argv[])
{
    qputenv("QSG_RENDERER_DEBUG", "upload");
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QGuiApplication app(argc, argv);
    TestInkMaterialGpu test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_inkmaterialgpu.moc"
