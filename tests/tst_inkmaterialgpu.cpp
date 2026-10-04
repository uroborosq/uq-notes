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
// Второй тест - батчинг на настоящем RHI с холстом приложения: журнал
// заливки батч-рендера (QSG_RENDERER_DEBUG=upload) перехватывается, и
// проверяется, что событие пера и касание новым штрихом не перезаливают
// слитый батч сухих чернил страницы (design.md ink-beauty-pass, D7).
// Программный бэкенд графа сцены (tst_pagescenegraph) батчей не строит,
// поэтому это проверяется только здесь.

#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSGGeometryNode>
#include <QSGTransformNode>
#include <QTest>

#include <rhi/qrhi.h>

#include <cstring>
#include <functional>
#include <memory>

#include "canvas/documentcanvas.h"
#include "canvas/inkmaterial.h"
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

    // Мокрый слой не трогает батч сухих чернил: ни событие пера на уже
    // занятом куске, ни касание новым штрихом. Добавление узла под корнем
    // батча пересобирает все батчи корня, и слитый батч сухих чернил
    // плотной страницы перезаливался бы на каждом касании.
    void wetInkDoesNotReuploadDryBatch()
    {
        UploadLog log;
        CanvasScene fixture;
        QVERIFY(fixture.open(fixtures::large()));
        if (!fixture.scene.initialize())
            QSKIP("RHI недоступен на этой платформе (программный бэкенд)");
        OffscreenScene &scene = fixture.scene;
        DocumentCanvas *canvas = fixture.canvas;

        const InkMaterial dryMaterial;
        const InkMaterial wetMaterial(InkMaterial::Layer::Wet);
        const QSGMaterialType *dry = dryMaterial.type();
        const QSGMaterialType *wet = wetMaterial.type();

        canvas->setPenWidth(3.0);
        canvas->goToPage(0);
        QVERIFY(scene.framesUntil([canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));

        qreal time = 3.0;
        const auto touch = [&](const QPointF &from) {
            QCOMPARE(canvas->pageAtView(from), 0);
            QVERIFY(canvas->beginInkAt(from, 0.6, false, time));
        };
        const auto move = [&](const QPointF &from, int first, int last) {
            for (int i = first; i <= last; ++i) {
                time += 1.0 / 240.0;
                canvas->continueInkAt(from + QPointF(i * 2.0, 0.0), 0.6, time);
            }
        };

        // Сухие чернила: штрихов на странице столько, что вершин в ней больше
        // порога батч-рендера (1024), и узел страницы, который двигается
        // каждый кадр, становится корнем батча - как плотная страница
        // приложения. Мелкое поддерево без своего корня батч-рендер
        // перезаливает на каждом сдвиге целиком, и проверять было бы нечего.
        // Батч сухого слоя залит и виден в журнале - иначе отсутствие строк
        // ничего не значит.
        constexpr int kDryStrokes = 12;
        log.clear();
        for (int k = 0; k < kDryStrokes; ++k) {
            const QPointF from(300, 150 + k * 20.0);
            touch(from);
            move(from, 1, 60);
            canvas->endInk();
        }
        QVERIFY(scene.framesUntil(
            [canvas] { return canvas->inkMaterialStats(0).dryNodes == kDryStrokes; }));
        scene.frames(3);
        QVERIFY2(log.uploadedElements(dry) > 0, "батч сухих чернил в журнале заливки не виден");
        // Покой: растр страницы в полном разрешении доехал, кадр ничего не
        // заливает. Смена текстуры страницы перезаливает и чернила под ней
        // (слитый батч текстур соседних страниц перекрывает их порядки), и
        // это не то, что здесь проверяется.
        const auto idle = [&] {
            for (int i = 0; i < 200; ++i) {
                QTest::qWait(5);
                log.clear();
                scene.frame();
                if (log.uploadedElements() == 0)
                    return true;
            }
            return false;
        };
        QVERIFY(idle());
        const QImage before = scene.frame();

        // Касание вторым штрихом: залит кусок мокрого штриха, сухой батч - нет.
        // Что ожившему пустому куску нужен DirtyMaterial, здесь не ловится:
        // соседние страницы без своего корня батча двигаются каждый кадр и
        // сами заказывают перестройку батчей, так что кусок попадает в батч
        // и без него.
        const QPointF second(300, 450);
        log.clear();
        touch(second);
        move(second, 1, 6);
        scene.frame();
        QVERIFY2(log.uploadedElements(wet) > 0, "мокрый штрих не залит - он не нарисован");
        QVERIFY2(log.uploadedElements(dry) == 0,
                 qPrintable(QStringLiteral("касание перезалило батч сухих чернил: %1 узлов")
                                .arg(log.uploadedElements(dry))));

        // Событие пера на уже занятом куске: только кусок.
        log.clear();
        move(second, 7, 40);
        const QImage image = scene.frame();
        QVERIFY(log.uploadedElements(wet) > 0);
        QVERIFY2(log.uploadedElements(dry) == 0,
                 qPrintable(QStringLiteral("событие пера перезалило батч сухих чернил: %1 узлов")
                                .arg(log.uploadedElements(dry))));
        // И штрих действительно нарисован: поперёк оси кадр темнее, чем до
        // касания. Ожившему куску без батча этого не видать.
        int darkening = 0;
        const int x = int(second.x()) + 40;
        for (int y = int(second.y()) - 4; y <= int(second.y()) + 4; ++y)
            darkening = qMax(darkening, qRed(before.pixel(x, y)) - qRed(image.pixel(x, y)));
        QVERIFY2(darkening >= 64,
                 qPrintable(QStringLiteral("мокрый штрих не виден: темнее на %1").arg(darkening)));

        canvas->endInk();
        scene.frames(2);
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
