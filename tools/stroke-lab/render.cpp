#include "render.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "raster.h"

namespace lab {

namespace {

double len(const QPointF &v) { return std::hypot(v.x(), v.y()); }

QImage blank(const Sample &sample, const RenderOptions &options)
{
    const int w = int(std::ceil(sample.bounds.width() * options.dpr));
    const int h = int(std::ceil(sample.bounds.height() * options.dpr));
    QImage image(std::max(1, w), std::max(1, h), QImage::Format_RGB32);
    image.fill(options.paper);
    return image;
}

void addGeometry(CoverageBuffer &buffer, const Geometry &g, double toPixel)
{
    switch (g.kind) {
    case Geometry::Kind::TriangleStrip:
    case Geometry::Kind::Triangles: {
        std::vector<QPointF> v;
        v.reserve(g.vertices.size());
        for (const QPointF &p : g.vertices)
            v.push_back(p * toPixel);
        if (g.kind == Geometry::Kind::TriangleStrip)
            buffer.addTriangleStrip(v);
        else
            buffer.addTriangles(v);
        break;
    }
    case Geometry::Kind::Polygons: {
        std::vector<QPolygonF> rings;
        for (const QPolygonF &ring : g.polygons) {
            QPolygonF r;
            for (const QPointF &p : ring)
                r.append(p * toPixel);
            rings.push_back(r);
        }
        buffer.addPolygons(rings);
        break;
    }
    case Geometry::Kind::None:
        break;
    }
}

// Круглая кисть: диски в точках и трапеции между ними. Пиксели.
void addSweptBrush(CoverageBuffer &buffer, const std::vector<QPointF> &pts,
                   const std::vector<double> &radius)
{
    for (size_t i = 0; i < pts.size(); ++i) {
        buffer.addDisc(pts[i], radius[i]);
        if (i + 1 < pts.size()) {
            const QPointF d = pts[i + 1] - pts[i];
            const double l = len(d);
            if (l < 1e-9)
                continue;
            const QPointF n(-d.y() / l, d.x() / l);
            buffer.addConvex({pts[i] + n * radius[i], pts[i + 1] + n * radius[i + 1],
                              pts[i + 1] - n * radius[i + 1], pts[i] - n * radius[i]});
        }
    }
}

} // namespace

QImage renderApp(const Sample &sample, const std::vector<StrokeRun> &runs, double scale,
                 const RenderOptions &options)
{
    QImage image = blank(sample, options);
    CoverageBuffer buffer(image.width(), image.height(), SamplePattern::msaa4());
    const double toPixel = scale * options.dpr;
    std::vector<const StrokeRun *> custom;
    for (const StrokeRun &run : runs) {
        if (run.variant) {
            // Пробный вызов на копии: своя растеризация есть, только если
            // вариант её действительно сделал.
            custom.push_back(&run);
            continue;
        }
        addGeometry(buffer, run.geometry, toPixel);
    }
    // Варианты со своей растеризацией рисуют сами; остальные - через MSAA.
    std::vector<const StrokeRun *> own;
    for (const StrokeRun *run : custom) {
        QImage probe = image;
        RasterTarget target{&probe, QTransform::fromScale(toPixel, toPixel), options.ink};
        if (run->variant->rasterize(run->geometry, target))
            own.push_back(run);
        else
            addGeometry(buffer, run->geometry, toPixel);
    }
    buffer.compositeOnto(image, options.ink);
    for (const StrokeRun *run : own) {
        RasterTarget target{&image, QTransform::fromScale(toPixel, toPixel), options.ink};
        run->variant->rasterize(run->geometry, target);
    }
    return image;
}

QImage renderIdeal(const Sample &sample, const std::vector<StrokeRun> &runs, double scale,
                   const RenderOptions &options)
{
    QImage image = blank(sample, options);
    CoverageBuffer buffer(image.width(), image.height(), SamplePattern::grid(8));
    const double toPixel = scale * options.dpr;
    for (const StrokeRun &run : runs) {
        std::vector<QPointF> pts;
        std::vector<double> radius;
        for (const CenterPoint &c : run.centerline) {
            pts.push_back(c.pos * toPixel);
            radius.push_back(c.width * toPixel / 2.0);
        }
        addSweptBrush(buffer, pts, radius);
    }
    buffer.compositeOnto(image, options.ink);
    return image;
}

QImage renderTruth(const Sample &sample, double scale, const RenderOptions &options)
{
    QImage image = blank(sample, options);
    CoverageBuffer buffer(image.width(), image.height(), SamplePattern::grid(8));
    for (const SampleStroke &stroke : sample.strokes) {
        std::vector<QPointF> pts;
        std::vector<double> radius;
        for (size_t i = 0; i < stroke.truth.p.size(); ++i) {
            const QPointF p = stroke.truth.p[i] * options.dpr;
            if (!pts.empty() && len(p - pts.back()) < 0.4 && i + 1 < stroke.truth.p.size())
                continue;
            pts.push_back(p);
            radius.push_back(truthWidthPx(stroke.truth.pressure[i], scale, options.metrics)
                             * options.dpr / 2.0);
        }
        addSweptBrush(buffer, pts, radius);
    }
    buffer.compositeOnto(image, options.ink);
    return image;
}

QImage upscale(const QImage &image, int zoom)
{
    if (zoom <= 1)
        return image;
    return image.scaled(image.width() * zoom, image.height() * zoom, Qt::IgnoreAspectRatio,
                        Qt::FastTransformation);
}

QImage zoomCrop(const QImage &image, const QPointF &centerPx, double dpr, int w, int h, int zoom)
{
    const int pw = int(std::round(w * dpr));
    const int ph = int(std::round(h * dpr));
    int x = int(std::round(centerPx.x() * dpr - pw / 2.0));
    int y = int(std::round(centerPx.y() * dpr - ph / 2.0));
    QImage crop(pw, ph, QImage::Format_RGB32);
    crop.fill(QColor(255, 255, 255));
    QPainter painter(&crop);
    painter.drawImage(QPoint(0, 0), image, QRect(x, y, pw, ph));
    painter.end();
    return upscale(crop, zoom);
}

QPointF focusPoint(const Sample &sample)
{
    if (!sample.corners.empty())
        return sample.corners.front();
    // Наибольшая кривизна эталона (окно ±1.5 px), вдали от концов.
    QPointF best = sample.bounds.center();
    double bestK = -1;
    for (const SampleStroke &stroke : sample.strokes) {
        std::vector<QPointF> pts;
        for (const QPointF &p : stroke.truth.p)
            if (pts.empty() || len(p - pts.back()) >= 0.5)
                pts.push_back(p);
        for (size_t i = 6; i + 6 < pts.size(); ++i) {
            const QPointF a = pts[i] - pts[i - 3];
            const QPointF b = pts[i + 3] - pts[i];
            const double k = std::abs(std::atan2(a.x() * b.y() - a.y() * b.x(),
                                                 a.x() * b.x() + a.y() * b.y()));
            if (k > bestK) {
                bestK = k;
                best = pts[i];
            }
        }
    }
    return best;
}

namespace {

// Один мокрый кадр x4 вокруг кончика после отсчёта k.
QImage renderWetTile(const SampleStroke &stroke, const EventRecord &event, int k,
                     const Geometry &wet, double scale, const RenderOptions &options)
{
    constexpr int kZoom = 4;
    constexpr double kW = 80.0, kH = 60.0;
    // Где перо будет в момент фотона: L = 22 мс от отсчёта (эмуляция кадров в
    // metrics.cpp, kFrameLatencies).
    constexpr double kPhotonDelay = 0.022;
    const QPointF origin = event.tip - QPointF(kW / 2, kH / 2);
    const double toPixel = scale * options.dpr;

    QImage frame(int(std::ceil(kW * options.dpr)), int(std::ceil(kH * options.dpr)),
                 QImage::Format_RGB32);
    frame.fill(options.paper);
    CoverageBuffer buffer(frame.width(), frame.height(), SamplePattern::msaa4());
    Geometry g = wet;
    for (QPointF &p : g.vertices)
        p = p - origin / scale;
    for (QPolygonF &ring : g.polygons)
        ring.translate(-origin / scale);
    addGeometry(buffer, g, toPixel);
    buffer.compositeOnto(frame, options.ink);
    QImage big = upscale(frame, kZoom);

    QPainter painter(&big);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto map = [&](const QPointF &screen) {
        return (screen - origin) * options.dpr * kZoom;
    };
    const double tk = stroke.eventTrueTime[size_t(k)];
    // Эталон - тонкой зелёной линией до отсчёта, пурпурной - до фотона.
    QPainterPath truth, ahead;
    bool first = true, firstAhead = true;
    QPointF photon = stroke.truth.p.back();
    for (size_t i = 0; i < stroke.truth.p.size(); i += 5) {
        const double t = stroke.truth.t[i];
        if (t <= tk) {
            if (first)
                truth.moveTo(map(stroke.truth.p[i]));
            else
                truth.lineTo(map(stroke.truth.p[i]));
            first = false;
        } else if (t <= tk + kPhotonDelay) {
            if (firstAhead)
                ahead.moveTo(map(stroke.truth.p[i]));
            else
                ahead.lineTo(map(stroke.truth.p[i]));
            firstAhead = false;
            photon = stroke.truth.p[i];
        }
    }
    painter.setPen(QPen(QColor(0, 200, 90), 1.2));
    painter.drawPath(truth);
    painter.setPen(QPen(QColor(200, 0, 200), 1.2, Qt::DashLine));
    painter.drawPath(ahead);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 150, 0));
    for (int i = 0; i <= k; ++i)
        painter.drawEllipse(map(stroke.events[size_t(i)].position), 1.4, 1.4);
    painter.setBrush(QColor(220, 30, 30, 200));
    for (const QPointF &p : event.provisional)
        painter.drawEllipse(map(p), 1.6, 1.6);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(220, 30, 30), 1.5));
    painter.drawEllipse(map(event.tip), 6.0, 6.0);
    const QPointF pen = map(stroke.events[size_t(k)].position);
    painter.setPen(QPen(QColor(30, 90, 230), 1.5));
    painter.drawLine(pen - QPointF(8, 0), pen + QPointF(8, 0));
    painter.drawLine(pen - QPointF(0, 8), pen + QPointF(0, 8));
    const QPointF ph = map(photon);
    painter.setPen(QPen(QColor(200, 0, 200), 1.5));
    painter.drawLine(ph - QPointF(6, 6), ph + QPointF(6, 6));
    painter.drawLine(ph - QPointF(6, -6), ph + QPointF(6, -6));
    painter.setPen(QColor(0, 0, 0));
    painter.drawText(QPointF(6, 14),
                     QStringLiteral("event %1, lag %2 px, pen@+22ms %3 px")
                         .arg(k)
                         .arg(len(event.tip - stroke.events[size_t(k)].position), 0, 'f', 2)
                         .arg(len(event.tip - photon), 0, 'f', 1));
    painter.end();
    return big;
}

} // namespace

QImage renderWetFrame(const Sample &sample, const std::vector<StrokeRun> &runs, double scale,
                      const RenderOptions &options)
{
    const StrokeRun *run = nullptr;
    size_t strokeIndex = 0;
    for (size_t i = 0; i < runs.size(); ++i) {
        if (runs[i].wetSnapshotEvent >= 0) {
            run = &runs[i];
            strokeIndex = i;
            break;
        }
    }
    if (!run || strokeIndex >= sample.strokes.size())
        return {};
    const SampleStroke &stroke = sample.strokes[strokeIndex];
    // Лента: все записанные кадры по порядку отсчётов (или один снимок).
    std::vector<QImage> tiles;
    if (!run->wetFilmEvents.empty()) {
        for (size_t i = 0; i < run->wetFilmEvents.size(); ++i) {
            const int k = run->wetFilmEvents[i];
            tiles.push_back(renderWetTile(stroke, run->events[size_t(k)], k,
                                          run->wetFilmGeometry[i], scale, options));
        }
    } else {
        const int k = run->wetSnapshotEvent;
        tiles.push_back(renderWetTile(stroke, run->events[size_t(k)], k, run->wetGeometry, scale,
                                      options));
    }
    constexpr int kGap = 4;
    int width = 0, height = 0;
    for (const QImage &t : tiles) {
        width += t.width() + kGap;
        height = std::max(height, t.height());
    }
    QImage film(std::max(1, width - kGap), height, QImage::Format_RGB32);
    film.fill(QColor(225, 225, 228));
    QPainter painter(&film);
    int x = 0;
    for (const QImage &t : tiles) {
        painter.drawImage(x, 0, t);
        x += t.width() + kGap;
    }
    painter.end();
    return film;
}

QImage composeSheet(const std::vector<QString> &columnTitles,
                    const std::vector<QString> &rowTitles,
                    const std::vector<std::vector<QImage>> &cells, int zoom)
{
    constexpr int kGap = 6;
    constexpr int kTitle = 18;
    constexpr int kRowLabel = 150;
    std::vector<int> colW(columnTitles.size(), 60);
    std::vector<int> rowH(rowTitles.size(), 20);
    for (size_t r = 0; r < cells.size(); ++r) {
        for (size_t c = 0; c < cells[r].size() && c < colW.size(); ++c) {
            colW[c] = std::max(colW[c], cells[r][c].width() * zoom);
            rowH[r] = std::max(rowH[r], cells[r][c].height() * zoom);
        }
    }
    int width = kRowLabel + kGap;
    for (int w : colW)
        width += w + kGap;
    int height = kTitle + kGap;
    for (int h : rowH)
        height += h + kGap;

    QImage sheet(width, height, QImage::Format_RGB32);
    sheet.fill(QColor(225, 225, 228));
    QPainter painter(&sheet);
    QFont font = painter.font();
    font.setPixelSize(12);
    painter.setFont(font);
    painter.setPen(QColor(20, 20, 20));
    int x = kRowLabel + kGap;
    for (size_t c = 0; c < columnTitles.size(); ++c) {
        painter.drawText(QRect(x, 0, colW[c], kTitle), Qt::AlignLeft | Qt::AlignVCenter,
                         columnTitles[c]);
        x += colW[c] + kGap;
    }
    int y = kTitle + kGap;
    for (size_t r = 0; r < rowTitles.size(); ++r) {
        painter.drawText(QRect(4, y, kRowLabel - 4, rowH[r]), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         rowTitles[r]);
        x = kRowLabel + kGap;
        for (size_t c = 0; c < colW.size(); ++c) {
            if (r < cells.size() && c < cells[r].size() && !cells[r][c].isNull())
                painter.drawImage(QPoint(x, y), upscale(cells[r][c], zoom));
            x += colW[c] + kGap;
        }
        y += rowH[r] + kGap;
    }
    painter.end();
    return sheet;
}

} // namespace lab
