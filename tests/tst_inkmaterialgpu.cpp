// ink-beauty-pass, D7: материал чернил на настоящем RHI. Граф сцены Qt Quick
// рисует полосу через QQuickRenderControl в текстуру - окна на экране нет, -
// и профиль покрытия поперёк штриха сверяется с CPU-зеркалом шейдеров
// (src/ink/inkcoverage.h). Зум меняет только матрицу узла: геометрия та же,
// а кромка обязана пересчитаться в шейдере. 80 узлов - чтобы узел зума стал
// корнем батча, и полосы прошли через слитый батч батч-рендера.
//
// Под QT_QPA_PLATFORM=offscreen RHI обычно нет (программный бэкенд) - тогда
// SKIP. Проверка на GPU: QT_QPA_PLATFORM=wayland ./tst_inkmaterialgpu.

#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGTransformNode>
#include <QTest>

#include <rhi/qrhi.h>

#include <cstring>
#include <memory>

#include "canvas/inkmaterial.h"
#include "ink/inkcoverage.h"
#include "ink/stroketessellator.h"

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

} // namespace

class TestInkMaterialGpu : public QObject
{
    Q_OBJECT

private slots:
    void edgeCoverageMatchesMirrorAtEveryZoom()
    {
        const QSize size(400, 200);
        QQuickRenderControl control;
        QQuickWindow window(&control);
        window.setColor(Qt::white);
        window.resize(size);
        auto *item = new InkItem;
        item->setParentItem(window.contentItem());
        item->setSize(QSizeF(size));

        if (!control.initialize() || !control.rhi())
            QSKIP("RHI недоступен на этой платформе (программный бэкенд)");
        QRhi *rhi = control.rhi();
        qInfo("RHI %s, %s", rhi->backendName(), rhi->driverInfo().deviceName.constData());

        std::unique_ptr<QRhiTexture> texture(rhi->newTexture(
            QRhiTexture::RGBA8, size, 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        QVERIFY(texture->create());
        std::unique_ptr<QRhiRenderBuffer> depth(
            rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 1));
        QVERIFY(depth->create());
        QRhiTextureRenderTargetDescription description{QRhiColorAttachment(texture.get())};
        description.setDepthStencilBuffer(depth.get());
        std::unique_ptr<QRhiTextureRenderTarget> target(
            rhi->newTextureRenderTarget(description));
        std::unique_ptr<QRhiRenderPassDescriptor> pass(
            target->newCompatibleRenderPassDescriptor());
        target->setRenderPassDescriptor(pass.get());
        QVERIFY(target->create());
        window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(target.get()));

        const auto frame = [&] {
            control.polishItems();
            control.beginFrame();
            control.sync();
            control.render();
            QRhiReadbackResult readback;
            QRhiResourceUpdateBatch *batch = rhi->nextResourceUpdateBatch();
            batch->readBackTexture({texture.get()}, &readback);
            control.commandBuffer()->resourceUpdate(batch);
            control.endFrame();
            QImage image(reinterpret_cast<const uchar *>(readback.data.constData()),
                         readback.pixelSize.width(), readback.pixelSize.height(),
                         QImage::Format_RGBA8888_Premultiplied);
            QImage copy = image.convertToFormat(QImage::Format_RGB32);
            return rhi->isYUpInFramebuffer() ? copy.flipped(Qt::Vertical) : copy;
        };

        for (const qreal zoom : {1.0, 2.0, 1.5, 3.0}) {
            item->zoom = zoom;
            item->update();
            const QImage image = frame();
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
};

QTEST_MAIN(TestInkMaterialGpu)
#include "tst_inkmaterialgpu.moc"
