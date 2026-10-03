// aa-gpu-bench: настоящий GPU (QRhi, OpenGL/EGL) для эксперимента со сглаживанием.
// (Vulkan не годится: в системе нет vulkan-headers, без них QVulkanInstance не
// объявлен.)
//
//  1. Проверка эмуляции: сэмпл корпуса рисуется на GPU (a) полосой baseline в
//     MSAA 4x с resolve, как приложение, и (b) полосой aa-fringe шейдером
//     gpu/ink.{vert,frag} без MSAA со смешиванием premultiplied over. Те же
//     вершины растеризуются эмуляцией лаборатории; печатается расхождение.
//  2. Цена кадра: «плотная страница» (копии сэмплов корпуса на весь кадр),
//     время GPU по меткам времени QRhi (EnableTimestamps,
//     lastCompletedGpuTime) для msaa4-flat, fringe-1x и flat-1x.
//
// Не Qt Quick: батчинг и QSGMaterial здесь не проверяются, только растеризация
// и стоимость шейдера/MSAA на этом GPU.
//
//   QT_QPA_PLATFORM=wayland ./build-lab/aa-gpu-bench --out DIR [--frames 300]

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "aaraster.h"
#include "corpus.h"
#include "lab.h"
#include "raster.h"
#include "variants/aa_fringe.h"

namespace lab {
std::vector<VariantInfo> &registry()
{
    static std::vector<VariantInfo> list;
    return list;
}
} // namespace lab

namespace {

using namespace lab;

const QColor kInk(0x1c, 0x2a, 0x5a);

struct Uniforms
{
    float mvp[16];
    float color[4];
    float pxPerUnit;
    float minHwPx;
    float fringePx;
    float pad;
};

QShader loadShader(const QString &name)
{
    QFile f(QStringLiteral(":/shaders/") + name + QStringLiteral(".qsb"));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("no shader %s", qPrintable(name));
    return QShader::fromSerialized(f.readAll());
}

std::unique_ptr<Variant> runStrokes(const char *name, const SampleStroke &stroke, double scale,
                                    double width)
{
    for (const VariantInfo &v : registry()) {
        if (v.name != name)
            continue;
        std::unique_ptr<Variant> variant = v.factory();
        StrokeSetup setup;
        setup.scale = scale;
        setup.width = width;
        setup.color = kInk;
        variant->begin(setup);
        for (const InkPoint &e : stroke.events)
            variant->addEvent(InkPoint{e.position / scale, e.pressure, e.timeSeconds});
        variant->finish();
        return variant;
    }
    qFatal("no variant %s", name);
}

// Геометрия сцены: треугольники (не полоса - так ближе к батчу Qt Quick).
struct Scene
{
    std::vector<float> flat;              // x, y страницы
    std::vector<FringeGpuVertex> fringe;  // по 3 на треугольник
    int segments = 0;
};

void appendSample(Scene &scene, const Sample &sample, double scale, double width,
                  const QPointF &offsetPage)
{
    for (const SampleStroke &stroke : sample.strokes) {
        auto base = runStrokes("baseline", stroke, scale, width);
        const Geometry g = base->geometry();
        for (size_t i = 0; i + 2 < g.vertices.size(); ++i)
            for (int k = 0; k < 3; ++k) {
                scene.flat.push_back(float(g.vertices[i + k].x() + offsetPage.x()));
                scene.flat.push_back(float(g.vertices[i + k].y() + offsetPage.y()));
            }
        scene.segments += int(base->centerline().size());
        auto fringe = runStrokes("aa-fringe", stroke, scale, width);
        std::vector<FringeGpuVertex> strip;
        fringeGpuStrip(*fringe, strip);
        for (size_t i = 0; i + 2 < strip.size(); ++i) {
            const auto same = [](const FringeGpuVertex &a, const FringeGpuVertex &b) {
                return std::memcmp(&a, &b, sizeof a) == 0;
            };
            if (same(strip[i], strip[i + 1]) || same(strip[i + 1], strip[i + 2])
                || same(strip[i], strip[i + 2]))
                continue;
            for (int k = 0; k < 3; ++k) {
                FringeGpuVertex v = strip[i + size_t(k)];
                v.cx += float(offsetPage.x());
                v.cy += float(offsetPage.y());
                scene.fringe.push_back(v);
            }
        }
    }
}

enum class Mode { Msaa4Flat, Fringe, Flat1x };
const char *modeName(Mode m)
{
    switch (m) {
    case Mode::Msaa4Flat: return "msaa4-flat";
    case Mode::Fringe: return "fringe-1x";
    case Mode::Flat1x: return "flat-1x";
    }
    return "?";
}

class Renderer
{
public:
    explicit Renderer(QRhi *rhi) : m_rhi(rhi) {}

    // Рисует сцену size пикселей с pageToPixel = scale; возвращает картинку и
    // (если frames > 1) времена GPU, мс.
    QImage render(const Scene &scene, Mode mode, const QSize &size, double toPixel, int frames,
                  std::vector<double> *gpuMs, std::vector<double> *cpuMs)
    {
        const bool msaa = mode == Mode::Msaa4Flat;
        const bool fringe = mode == Mode::Fringe;
        std::unique_ptr<QRhiTexture> tex(m_rhi->newTexture(
            QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        tex->create();
        std::unique_ptr<QRhiRenderBuffer> ms;
        QRhiColorAttachment att;
        if (msaa) {
            ms.reset(m_rhi->newRenderBuffer(QRhiRenderBuffer::Color, size, 4, {}, QRhiTexture::RGBA8));
            ms->create();
            att.setRenderBuffer(ms.get());
            att.setResolveTexture(tex.get());
        } else {
            att.setTexture(tex.get());
        }
        QRhiTextureRenderTargetDescription rtDesc(att);
        std::unique_ptr<QRhiTextureRenderTarget> rt(m_rhi->newTextureRenderTarget(rtDesc));
        std::unique_ptr<QRhiRenderPassDescriptor> rp(rt->newCompatibleRenderPassDescriptor());
        rt->setRenderPassDescriptor(rp.get());
        rt->create();

        const void *data = fringe ? static_cast<const void *>(scene.fringe.data())
                                  : static_cast<const void *>(scene.flat.data());
        const quint32 bytes = fringe ? quint32(scene.fringe.size() * sizeof(FringeGpuVertex))
                                     : quint32(scene.flat.size() * sizeof(float));
        const int vertexCount = fringe ? int(scene.fringe.size()) : int(scene.flat.size() / 2);
        std::unique_ptr<QRhiBuffer> vbuf(
            m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, std::max(bytes, 16u)));
        vbuf->create();
        std::unique_ptr<QRhiBuffer> ubuf(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Uniforms)));
        ubuf->create();
        std::unique_ptr<QRhiShaderResourceBindings> srb(m_rhi->newShaderResourceBindings());
        srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            ubuf.get())});
        srb->create();

        std::unique_ptr<QRhiGraphicsPipeline> ps(m_rhi->newGraphicsPipeline());
        if (fringe) {
            ps->setShaderStages({{QRhiShaderStage::Vertex, loadShader("ink.vert")},
                                 {QRhiShaderStage::Fragment, loadShader("ink.frag")}});
            QRhiVertexInputLayout layout;
            layout.setBindings({{sizeof(FringeGpuVertex)}});
            layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0},
                                  {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
                                  {0, 2, QRhiVertexInputAttribute::Float, 4 * sizeof(float)},
                                  {0, 3, QRhiVertexInputAttribute::Float, 5 * sizeof(float)}});
            ps->setVertexInputLayout(layout);
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::One;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.srcAlpha = QRhiGraphicsPipeline::One;
            blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            ps->setTargetBlends({blend});
        } else {
            ps->setShaderStages({{QRhiShaderStage::Vertex, loadShader("flat.vert")},
                                 {QRhiShaderStage::Fragment, loadShader("flat.frag")}});
            QRhiVertexInputLayout layout;
            layout.setBindings({{2 * sizeof(float)}});
            layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
            ps->setVertexInputLayout(layout);
        }
        ps->setSampleCount(msaa ? 4 : 1);
        ps->setShaderResourceBindings(srb.get());
        ps->setRenderPassDescriptor(rp.get());
        ps->create();

        QMatrix4x4 mvp = m_rhi->clipSpaceCorrMatrix();
        mvp.ortho(0, size.width(), size.height(), 0, -1, 1);
        mvp.scale(float(toPixel), float(toPixel));
        Uniforms u{};
        std::memcpy(u.mvp, mvp.constData(), sizeof u.mvp);
        u.color[0] = float(kInk.redF());
        u.color[1] = float(kInk.greenF());
        u.color[2] = float(kInk.blueF());
        u.color[3] = 1.0f;
        u.pxPerUnit = float(toPixel);
        u.minHwPx = 0.15f;
        u.fringePx = 1.0f;

        QImage result;
        for (int frame = 0; frame < frames; ++frame) {
            QElapsedTimer timer;
            timer.start();
            QRhiCommandBuffer *cb = nullptr;
            if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
                qFatal("beginOffscreenFrame failed");
            QRhiResourceUpdateBatch *u0 = m_rhi->nextResourceUpdateBatch();
            if (frame == 0)
                u0->uploadStaticBuffer(vbuf.get(), data);
            u0->updateDynamicBuffer(ubuf.get(), 0, sizeof u, &u);
            cb->beginPass(rt.get(), QColor(255, 255, 255), {1.0f, 0}, u0);
            cb->setGraphicsPipeline(ps.get());
            cb->setViewport({0, 0, float(size.width()), float(size.height())});
            cb->setShaderResources();
            const QRhiCommandBuffer::VertexInput vin(vbuf.get(), 0);
            cb->setVertexInput(0, 1, &vin);
            cb->draw(quint32(vertexCount));
            QRhiReadbackResult rb;
            QRhiResourceUpdateBatch *u1 = nullptr;
            if (frame + 1 == frames) {
                u1 = m_rhi->nextResourceUpdateBatch();
                u1->readBackTexture({tex.get()}, &rb);
            }
            cb->endPass(u1);
            m_rhi->endOffscreenFrame();
            if (cpuMs)
                cpuMs->push_back(timer.nsecsElapsed() / 1e6);
            if (gpuMs)
                gpuMs->push_back(cb->lastCompletedGpuTime() * 1000.0);
            if (frame + 1 == frames) {
                QImage img(reinterpret_cast<const uchar *>(rb.data.constData()), rb.pixelSize.width(),
                           rb.pixelSize.height(), QImage::Format_RGBA8888);
                result = img.convertToFormat(QImage::Format_RGB32);
                if (m_rhi->isYUpInFramebuffer())
                    result = result.flipped(Qt::Vertical);
            }
        }
        return result;
    }

private:
    QRhi *m_rhi;
};

double diffRms(const QImage &a, const QImage &b, double *maxOut, double *fracOver8)
{
    double sq = 0, mx = 0;
    long n = 0, over = 0, inked = 0;
    for (int y = 0; y < std::min(a.height(), b.height()); ++y)
        for (int x = 0; x < std::min(a.width(), b.width()); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            const double d = (std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q))
                              + std::abs(qBlue(p) - qBlue(q)))
                             / 3.0;
            if (qRed(p) < 254 || qRed(q) < 254) {
                ++inked;
                sq += d * d;
                ++n;
                if (d > 8)
                    ++over;
            }
            mx = std::max(mx, d);
        }
    if (maxOut)
        *maxOut = mx;
    if (fracOver8)
        *fracOver8 = inked ? double(over) / inked : 0;
    return n ? std::sqrt(sq / n) : 0;
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
double pct(std::vector<double> v, double p)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * v.size()))];
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", qEnvironmentVariableIsSet("WAYLAND_DISPLAY") ? "wayland" : "xcb");
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption outOpt("out", "dir", "dir", "aa-gpu-out");
    QCommandLineOption framesOpt("frames", "frames for timing", "n", "300");
    QCommandLineOption widthOpt("width", "pen width pt", "pt", "1.0");
    QCommandLineOption dprOpt("dpr", "dpr", "r", "1.05");
    parser.addOptions({outOpt, framesOpt, widthOpt, dprOpt});
    parser.process(app);
    const QString out = parser.value(outOpt);
    QDir().mkpath(out);
    const int frames = std::max(2, parser.value(framesOpt).toInt());
    const double width = parser.value(widthOpt).toDouble();
    const double dpr = parser.value(dprOpt).toDouble();

    std::unique_ptr<QOffscreenSurface> surface(QRhiGles2InitParams::newFallbackSurface());
    QRhiGles2InitParams params;
    params.fallbackSurface = surface.get();
    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::OpenGLES2, &params, QRhi::EnableTimestamps));
    if (!rhi)
        qFatal("QRhi OpenGL failed");
    std::printf("GPU: %s, driver %s\n", rhi->driverInfo().deviceName.constData(),
                rhi->backendName());
    Renderer renderer(rhi.get());
    const std::vector<Sample> corpus = buildCorpus();

    // 1. Проверка эмуляции.
    std::printf("\n## GPU vs эмуляция stroke-lab (RMS по чернильным пикселям, уровни 0..255)\n");
    std::printf("| sample | scale | msaa4 GPU↔emu rms/max/%%>8 | msaa4 GPU↔emu(шаблон отражён по y) | fringe GPU↔emu rms/max/%%>8 | GPU fringe↔GPU msaa rms |\n|---|---|---|---|---|---|\n");
    for (const char *id : {"word_hello", "word_minimum_small", "small_o_8", "corner_M", "ref_spiral"}) {
        const Sample *sample = nullptr;
        for (const Sample &s : corpus)
            if (s.id == id)
                sample = &s;
        if (!sample)
            continue;
        for (double scale : {1.0, 2.3}) {
            const double toPixel = scale * dpr;
            const QSize size(int(std::ceil(sample->bounds.width() * dpr)),
                             int(std::ceil(sample->bounds.height() * dpr)));
            Scene scene;
            appendSample(scene, *sample, scale, width, QPointF());
            const QImage gpuMsaa = renderer.render(scene, Mode::Msaa4Flat, size, toPixel, 1, nullptr, nullptr);
            const QImage gpuFringe = renderer.render(scene, Mode::Fringe, size, toPixel, 1, nullptr, nullptr);
            // Эмуляция: те же штрихи.
            QImage emuMsaa(size, QImage::Format_RGB32), emuFringe(size, QImage::Format_RGB32);
            emuMsaa.fill(Qt::white);
            emuFringe.fill(Qt::white);
            CoverageBuffer buffer(size.width(), size.height(), SamplePattern::msaa4());
            // Тот же шаблон, отражённый по y: в GL начало координат кадра
            // внизу, а лаборатория считает сверху.
            SamplePattern mirrored = SamplePattern::msaa4();
            for (QPointF &o : mirrored.offsets)
                o.setY(1.0 - o.y());
            CoverageBuffer bufferMirrored(size.width(), size.height(), mirrored);
            QImage emuMsaaMirrored(size, QImage::Format_RGB32);
            emuMsaaMirrored.fill(Qt::white);
            for (const SampleStroke &stroke : sample->strokes) {
                auto base = runStrokes("baseline", stroke, scale, width);
                std::vector<QPointF> v;
                for (const QPointF &p : base->geometry().vertices)
                    v.push_back(p * toPixel);
                buffer.addTriangleStrip(v);
                bufferMirrored.addTriangleStrip(v);
                auto fringe = runStrokes("aa-fringe", stroke, scale, width);
                RasterTarget target{&emuFringe, QTransform::fromScale(toPixel, toPixel), kInk};
                fringe->rasterize(fringe->geometry(), target);
            }
            buffer.compositeOnto(emuMsaa, kInk);
            bufferMirrored.compositeOnto(emuMsaaMirrored, kInk);
            const QString tag = QString::fromLatin1(id) + QStringLiteral("_s") + QString::number(scale);
            gpuMsaa.save(out + "/gpu_msaa4_" + tag + ".png");
            gpuFringe.save(out + "/gpu_fringe_" + tag + ".png");
            emuMsaa.save(out + "/emu_msaa4_" + tag + ".png");
            emuFringe.save(out + "/emu_fringe_" + tag + ".png");
            double m1, m2, f1, f2;
            const double r1 = diffRms(gpuMsaa, emuMsaa, &m1, &f1);
            const double r2 = diffRms(gpuFringe, emuFringe, &m2, &f2);
            const double r3 = diffRms(gpuFringe, gpuMsaa, nullptr, nullptr);
            double m4, f4;
            const double r4 = diffRms(gpuMsaa, emuMsaaMirrored, &m4, &f4);
            std::printf("| %s | %.1f | %.2f / %.0f / %.2f%% | %.2f / %.0f / %.2f%% | %.2f / %.0f / %.2f%% | %.1f |\n",
                        id, scale, r1, m1, 100 * f1, r4, m4, 100 * f4, r2, m2, 100 * f2, r3);
        }
    }

    // 2. Цена кадра: плотная страница 1920x1080.
    const QSize screen(1920, 1080);
    for (double scale : {1.0, 2.3}) {
        const double toPixel = scale * dpr;
        Scene scene;
        double x = 0, y = 0, rowH = 0;
        int copies = 0;
        // Слова и буквы корпуса подряд, как строки текста, пока не заполнят экран.
        while (y < screen.height()) {
            for (const Sample &s : corpus) {
                if (!(s.hasTag("word") || s.hasTag("small")))
                    continue;
                const double w = s.bounds.width() * dpr * 0.8;
                const double h = s.bounds.height() * dpr * 0.6;
                if (x + w > screen.width()) {
                    x = 0;
                    y += rowH;
                    rowH = 0;
                }
                if (y >= screen.height())
                    break;
                appendSample(scene, s, scale, width, QPointF(x, y) / toPixel);
                x += w;
                rowH = std::max(rowH, h);
                ++copies;
            }
        }
        std::printf("\n## Плотная страница 1920x1080, scale %.1f, перо %.1f pt: %d копий сэмплов, %d точек осевой, "
                    "вершин: flat %zu (8 Б), fringe %zu (24 Б); VBO flat %.2f МБ, fringe %.2f МБ\n",
                    scale, width, copies, scene.segments, scene.flat.size() / 2, scene.fringe.size(),
                    scene.flat.size() * 4 / 1048576.0, scene.fringe.size() * sizeof(FringeGpuVertex) / 1048576.0);
        std::printf("| mode | GPU ms median | GPU p95 | CPU frame ms median |\n|---|---|---|---|\n");
        for (Mode mode : {Mode::Msaa4Flat, Mode::Fringe, Mode::Flat1x}) {
            std::vector<double> gpu, cpu;
            const QImage img = renderer.render(scene, mode, screen, toPixel, frames, &gpu, &cpu);
            img.save(out + "/page_" + modeName(mode) + "_s" + QString::number(scale) + ".png");
            gpu.erase(gpu.begin(), gpu.begin() + std::min<size_t>(10, gpu.size()));
            cpu.erase(cpu.begin(), cpu.begin() + std::min<size_t>(10, cpu.size()));
            std::printf("| %s | %.3f | %.3f | %.3f |\n", modeName(mode), median(gpu), pct(gpu, 0.95),
                        median(cpu));
        }
    }
    return 0;
}
