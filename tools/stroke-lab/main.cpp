// stroke-lab: офлайн-стенд для сравнения конвейеров рукописных чернил.
// README.md рядом - как запускать и как добавить вариант.

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTextStream>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>

#include "corpus.h"
#include "lab.h"
#include "metrics.h"
#include "render.h"

namespace lab {
std::vector<VariantInfo> &registry()
{
    static std::vector<VariantInfo> list;
    return list;
}
} // namespace lab

namespace {

using namespace lab;

QString scaleTag(double scale)
{
    return QStringLiteral("s%1").arg(scale);
}

bool matches(const std::string &id, const QStringList &patterns)
{
    if (patterns.isEmpty())
        return true;
    for (const QString &p : patterns) {
        const QString pattern = p.trimmed();
        if (pattern.endsWith('*')) {
            if (QString::fromStdString(id).startsWith(pattern.chopped(1)))
                return true;
        } else if (QString::fromStdString(id) == pattern) {
            return true;
        }
    }
    return false;
}

// Один прогон варианта по одному сэмплу при одном масштабе.
std::vector<StrokeRun> runVariant(const VariantInfo &info, const Sample &sample, double scale,
                                  double width, const QColor &color, int reps)
{
    std::vector<StrokeRun> runs;
    // Снимок мокрого кадра - у самого длинного штриха, на 60% его отсчётов.
    size_t snapshotStroke = 0;
    for (size_t i = 0; i < sample.strokes.size(); ++i)
        if (sample.strokes[i].events.size() > sample.strokes[snapshotStroke].events.size())
            snapshotStroke = i;

    for (size_t si = 0; si < sample.strokes.size(); ++si) {
        const SampleStroke &stroke = sample.strokes[si];
        const size_t n = stroke.events.size();
        std::vector<double> best(n, 1e300);
        double bestFinish = 1e300;
        StrokeRun run;
        for (int rep = 0; rep < reps; ++rep) {
            const bool last = rep + 1 == reps;
            std::unique_ptr<Variant> variant = info.factory();
            StrokeSetup setup;
            setup.scale = scale;
            setup.width = width;
            setup.color = color;
            variant->begin(setup);

            std::vector<CenterPoint> seen;
            int mutations = 0;
            const int snapshotEvent =
                si == snapshotStroke ? int(std::floor(n * 0.6)) : -1;
            if (last)
                run.events.assign(n, EventRecord());

            for (size_t k = 0; k < n; ++k) {
                const InkPoint &e = stroke.events[k];
                const InkPoint raw{e.position / scale, e.pressure, e.timeSeconds};
                const auto t0 = std::chrono::steady_clock::now();
                variant->addEvent(raw);
                const auto t1 = std::chrono::steady_clock::now();
                best[k] = std::min(
                    best[k], std::chrono::duration<double, std::micro>(t1 - t0).count());
                if (!last)
                    continue;

                WetState ws;
                const int from = std::max(0, int(seen.size()) - 4);
                variant->wet(from, ws);
                // Окончательные точки не имеют права меняться.
                if (ws.settledCount < int(seen.size())) {
                    mutations += int(seen.size()) - ws.settledCount;
                    seen.resize(size_t(std::max(0, ws.settledCount)));
                }
                for (size_t j = 0; j < ws.settledFrom.size(); ++j) {
                    const size_t idx = size_t(from) + j;
                    if (idx < seen.size()) {
                        const QPointF d = seen[idx].pos - ws.settledFrom[j].pos;
                        if (std::hypot(d.x(), d.y()) > 1e-9)
                            ++mutations;
                    } else {
                        seen.push_back(ws.settledFrom[j]);
                    }
                }
                EventRecord &record = run.events[k];
                QPointF tip;
                if (!ws.provisional.empty())
                    tip = ws.provisional.back().pos;
                else if (!seen.empty())
                    tip = seen.back().pos;
                else
                    tip = raw.position;
                record.tip = tip * scale;
                for (const CenterPoint &c : ws.provisional)
                    record.provisional.push_back(c.pos * scale);
                record.wetDirtyVertices = ws.vertexCount - ws.settledVertexCount;
                if (int(k) == snapshotEvent) {
                    run.wetSnapshotEvent = snapshotEvent;
                    run.wetGeometry = variant->wetGeometry();
                }
            }
            const auto f0 = std::chrono::steady_clock::now();
            variant->finish();
            const auto f1 = std::chrono::steady_clock::now();
            bestFinish = std::min(bestFinish,
                                  std::chrono::duration<double, std::micro>(f1 - f0).count());
            if (last) {
                run.centerline = variant->centerline();
                run.geometry = variant->geometry();
                run.finalVertices = run.geometry.vertexCount();
                for (size_t j = 0; j < seen.size() && j < run.centerline.size(); ++j) {
                    const QPointF d = seen[j].pos - run.centerline[j].pos;
                    if (std::hypot(d.x(), d.y()) > 1e-9)
                        ++mutations;
                }
                if (seen.size() > run.centerline.size())
                    mutations += int(seen.size() - run.centerline.size());
                run.settledMutations = mutations;
                run.variant = std::move(variant);
            }
        }
        for (size_t k = 0; k < n; ++k)
            run.events[k].cpuMicros = best[k];
        run.finishMicros = bestFinish;
        runs.push_back(std::move(run));
    }
    return runs;
}

bool save(const QImage &image, const QString &path)
{
    if (image.isNull())
        return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!image.save(path)) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
        return false;
    }
    return true;
}

void writeJson(const QString &path, const QJsonObject &object)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

// Ключевые метрики для сводной таблицы: (ключ, агрегат, подпись).
struct Column
{
    const char *key;
    enum Agg { Mean, Max, Sum } agg;
    const char *title;
};

const Column kColumns[] = {
    {"centerline_dev_mean_px", Column::Mean, "dev mean"},
    {"centerline_dev_max_px", Column::Max, "dev max"},
    {"truth_coverage_max_px", Column::Mean, "cover max(avg)"},
    {"facet_count_2deg", Column::Sum, "facets>2°"},
    {"facet_max_deg", Column::Max, "facet max°"},
    {"curvature_tv_excess_per100px", Column::Mean, "curvTV excess"},
    {"curvature_tv_ratio", Column::Mean, "curvTV ratio"},
    {"corner_dist_mean_px", Column::Mean, "corner mean"},
    {"corner_dist_max_px", Column::Max, "corner max"},
    {"shape_hausdorff_pct", Column::Mean, "small H%"},
    {"closed_area_ratio", Column::Mean, "o area"},
    {"width_hf_rms_px", Column::Mean, "w hf"},
    {"wet_lag_px_mean", Column::Mean, "lag px"},
    {"wet_lag_px_p95", Column::Mean, "lag p95"},
    {"wet_lag_truth_px_mean", Column::Mean, "lag gt px"},
    {"wet_lag_ms_mean", Column::Mean, "lag ms"},
    {"wet_vs_final_mean_px", Column::Mean, "wet≠final"},
    {"settled_mutations", Column::Sum, "mut"},
    {"cpu_us_per_event_mean", Column::Mean, "µs/ev"},
    {"cpu_us_per_event_p99", Column::Max, "µs p99"},
    {"final_vertices", Column::Sum, "verts"},
};

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("stroke-lab"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Офлайн-стенд сравнения конвейеров рукописных чернил uq-notes"));
    parser.addHelpOption();
    QCommandLineOption variantsOpt(QStringLiteral("variants"),
                                   QStringLiteral("Варианты через запятую (по умолчанию все)"),
                                   QStringLiteral("list"));
    QCommandLineOption outOpt(QStringLiteral("out"), QStringLiteral("Каталог результатов"),
                              QStringLiteral("dir"), QStringLiteral("stroke-lab-out"));
    QCommandLineOption samplesOpt(
        QStringLiteral("samples"),
        QStringLiteral("Сэмплы через запятую, допускается суффикс * (small_*)"),
        QStringLiteral("list"));
    QCommandLineOption scalesOpt(QStringLiteral("scales"), QStringLiteral("Масштабы (зум)"),
                                 QStringLiteral("list"), QStringLiteral("1,2.5"));
    QCommandLineOption widthOpt(QStringLiteral("width"),
                                QStringLiteral("Толщина пера, точки страницы (в приложении по умолчанию 1.0)"),
                                QStringLiteral("pt"), QStringLiteral("1.0"));
    QCommandLineOption dprOpt(QStringLiteral("dpr"), QStringLiteral("devicePixelRatio экрана"),
                              QStringLiteral("ratio"), QStringLiteral("1"));
    QCommandLineOption repsOpt(QStringLiteral("reps"),
                               QStringLiteral("Повторов для замера CPU (берётся минимум)"),
                               QStringLiteral("n"), QStringLiteral("3"));
    QCommandLineOption noPngOpt(QStringLiteral("no-png"), QStringLiteral("Только метрики"));
    QCommandLineOption listOpt(QStringLiteral("list"), QStringLiteral("Список вариантов и сэмплов"));
    parser.addOptions({variantsOpt, outOpt, samplesOpt, scalesOpt, widthOpt, dprOpt, repsOpt,
                       noPngOpt, listOpt});
    parser.process(app);

    const std::vector<Sample> corpus = buildCorpus();
    auto &variants = registry();
    std::sort(variants.begin(), variants.end(),
              [](const VariantInfo &a, const VariantInfo &b) {
                  // baseline всегда первым - с ним сравнивают.
                  if ((a.name == "baseline") != (b.name == "baseline"))
                      return a.name == "baseline";
                  return a.name < b.name;
              });

    if (parser.isSet(listOpt)) {
        std::printf("Варианты:\n");
        for (const VariantInfo &v : variants)
            std::printf("  %-24s %s\n", v.name.c_str(), v.description.c_str());
        std::printf("Сэмплы:\n");
        for (const Sample &s : corpus) {
            size_t events = 0;
            for (const auto &st : s.strokes)
                events += st.events.size();
            std::printf("  %-22s %-50s strokes=%zu events=%zu\n", s.id.c_str(), s.title.c_str(),
                        s.strokes.size(), events);
        }
        return 0;
    }

    std::vector<const VariantInfo *> chosen;
    const QStringList wanted = parser.value(variantsOpt).split(',', Qt::SkipEmptyParts);
    for (const VariantInfo &v : variants)
        if (wanted.isEmpty() || wanted.contains(QString::fromStdString(v.name)))
            chosen.push_back(&v);
    for (const QString &w : wanted) {
        if (std::none_of(variants.begin(), variants.end(),
                         [&](const VariantInfo &v) { return QString::fromStdString(v.name) == w; })) {
            std::fprintf(stderr, "нет варианта %s (см. --list)\n", qPrintable(w));
            return 2;
        }
    }
    const QStringList samplePatterns = parser.value(samplesOpt).split(',', Qt::SkipEmptyParts);
    std::vector<const Sample *> samples;
    for (const Sample &s : corpus)
        if (matches(s.id, samplePatterns))
            samples.push_back(&s);
    std::vector<double> scales;
    for (const QString &s : parser.value(scalesOpt).split(',', Qt::SkipEmptyParts))
        scales.push_back(s.toDouble());
    const double width = parser.value(widthOpt).toDouble();
    const int reps = std::max(1, parser.value(repsOpt).toInt());
    const bool png = !parser.isSet(noPngOpt);
    const QString out = QDir(parser.value(outOpt)).absolutePath();
    QDir().mkpath(out);

    RenderOptions render;
    render.dpr = parser.value(dprOpt).toDouble();
    render.metrics.width = width;

    // results[variant][scale][sample] = metrics
    std::map<std::string, std::map<double, std::map<std::string, QJsonObject>>> results;
    // Картинки для сводных листов: [scale][sample][column].
    std::map<double, std::map<std::string, std::vector<QImage>>> sheetCells, cropCells;
    std::vector<QString> sheetColumns{QStringLiteral("truth (ideal AA)")};
    for (const VariantInfo *v : chosen) {
        sheetColumns.push_back(QString::fromStdString(v->name) + QStringLiteral(" (app)"));
        sheetColumns.push_back(QString::fromStdString(v->name) + QStringLiteral(" (ideal raster)"));
    }

    for (double scale : scales) {
        for (const Sample *sample : samples) {
            const QString tag = QString::fromStdString(sample->id) + '_' + scaleTag(scale);
            QImage truth;
            if (png) {
                truth = renderTruth(*sample, scale, render);
                save(truth, out + QStringLiteral("/truth/") + tag + QStringLiteral(".png"));
                sheetCells[scale][sample->id].push_back(truth);
                cropCells[scale][sample->id].push_back(
                    zoomCrop(truth, focusPoint(*sample), render.dpr, 32, 24, 8));
            }
            for (const VariantInfo *v : chosen) {
                std::vector<StrokeRun> runs = runVariant(*v, *sample, scale, width, render.ink, reps);
                QJsonObject m = computeMetrics(*sample, runs, scale, render.metrics);
                m["sample"] = QString::fromStdString(sample->id);
                m["title"] = QString::fromStdString(sample->title);
                m["scale"] = scale;
                results[v->name][scale][sample->id] = m;
                if (!png)
                    continue;
                const QString dir = out + '/' + QString::fromStdString(v->name);
                const QImage appImage = renderApp(*sample, runs, scale, render);
                const QImage ideal = renderIdeal(*sample, runs, scale, render);
                save(appImage, dir + QStringLiteral("/png/") + tag + QStringLiteral(".png"));
                save(ideal, dir + QStringLiteral("/png/") + tag + QStringLiteral("_ideal.png"));
                save(renderWetFrame(*sample, runs, scale, render),
                     dir + QStringLiteral("/wet/") + tag + QStringLiteral(".png"));
                const QImage crop = zoomCrop(appImage, focusPoint(*sample), render.dpr, 32, 24, 8);
                const QImage cropIdeal = zoomCrop(ideal, focusPoint(*sample), render.dpr, 32, 24, 8);
                save(crop, dir + QStringLiteral("/crops/") + tag + QStringLiteral(".png"));
                sheetCells[scale][sample->id].push_back(appImage);
                sheetCells[scale][sample->id].push_back(ideal);
                cropCells[scale][sample->id].push_back(crop);
                cropCells[scale][sample->id].push_back(cropIdeal);
            }
            std::fprintf(stderr, "  %s done\n", qPrintable(tag));
        }
    }

    // JSON по вариантам.
    for (const VariantInfo *v : chosen) {
        QJsonObject root;
        root["variant"] = QString::fromStdString(v->name);
        root["description"] = QString::fromStdString(v->description);
        root["width_pt"] = width;
        root["dpr"] = render.dpr;
        QJsonArray list;
        for (double scale : scales)
            for (const Sample *s : samples)
                list.append(results[v->name][scale][s->id]);
        root["samples"] = list;
        writeJson(out + '/' + QString::fromStdString(v->name) + QStringLiteral("/metrics.json"), root);
    }

    // Сводка: по варианту и масштабу - агрегаты ключевых метрик.
    QJsonObject summary;
    QString table;
    QTextStream md(&table);
    for (double scale : scales) {
        md << "\n### scale " << scale << "\n\n| variant |";
        for (const Column &c : kColumns)
            md << ' ' << c.title << " |";
        md << "\n|---|";
        for (size_t i = 0; i < std::size(kColumns); ++i)
            md << "---|";
        md << '\n';
        for (const VariantInfo *v : chosen) {
            QJsonObject agg;
            md << "| " << QString::fromStdString(v->name) << " |";
            for (const Column &c : kColumns) {
                double acc = c.agg == Column::Max ? -1e300 : 0.0;
                int count = 0;
                for (const Sample *s : samples) {
                    const QJsonValue value = results[v->name][scale][s->id].value(c.key);
                    if (!value.isDouble())
                        continue;
                    const double x = value.toDouble();
                    if (c.agg == Column::Max)
                        acc = std::max(acc, x);
                    else
                        acc += x;
                    ++count;
                }
                if (count == 0) {
                    md << " - |";
                    continue;
                }
                if (c.agg == Column::Mean)
                    acc /= count;
                agg[QString::fromLatin1(c.key) + (c.agg == Column::Mean ? "@mean" : c.agg == Column::Max ? "@max" : "@sum")] = acc;
                md << ' ' << QString::number(acc, 'g', 4) << " |";
            }
            md << '\n';
            QJsonObject perScale = summary.value(QString::fromStdString(v->name)).toObject();
            perScale[scaleTag(scale)] = agg;
            summary[QString::fromStdString(v->name)] = perScale;
        }
    }
    writeJson(out + QStringLiteral("/summary.json"), summary);
    {
        QFile f(out + QStringLiteral("/summary.md"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            f.write(table.toUtf8());
    }
    std::printf("%s\n", table.toUtf8().constData());

    if (png) {
        std::vector<QString> cropColumns{QStringLiteral("truth")};
        for (const VariantInfo *v : chosen) {
            cropColumns.push_back(QString::fromStdString(v->name));
            cropColumns.push_back(QString::fromStdString(v->name) + QStringLiteral(" ideal"));
        }
        for (double scale : scales) {
            std::vector<QString> rows;
            std::vector<std::vector<QImage>> cells, crops;
            for (const Sample *s : samples) {
                rows.push_back(QString::fromStdString(s->id));
                cells.push_back(sheetCells[scale][s->id]);
                crops.push_back(cropCells[scale][s->id]);
                // Сравнение по одному сэмплу: сверху целиком, снизу x8.
                const int zoom = s->bounds.width() < 140 ? 3 : 2;
                QImage top = composeSheet(sheetColumns, {QString::fromStdString(s->title)},
                                          {sheetCells[scale][s->id]}, zoom);
                QImage bottom = composeSheet(cropColumns, {QStringLiteral("x8 crop")},
                                             {cropCells[scale][s->id]}, 1);
                QImage both(std::max(top.width(), bottom.width()), top.height() + bottom.height(),
                            QImage::Format_RGB32);
                both.fill(QColor(225, 225, 228));
                QPainter p(&both);
                p.drawImage(0, 0, top);
                p.drawImage(0, top.height(), bottom);
                p.end();
                save(both, out + QStringLiteral("/compare/") + QString::fromStdString(s->id) + '_'
                               + scaleTag(scale) + QStringLiteral(".png"));
            }
            save(composeSheet(sheetColumns, rows, cells, 2),
                 out + QStringLiteral("/contact_") + scaleTag(scale) + QStringLiteral(".png"));
            save(composeSheet(cropColumns, rows, crops, 1),
                 out + QStringLiteral("/crops_") + scaleTag(scale) + QStringLiteral(".png"));
        }
    }
    std::fprintf(stderr, "results: %s\n", qPrintable(out));
    return 0;
}
