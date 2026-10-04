// Порядок слоёв страницы в графе сцены: чернила поверх растра при любом
// порядке появления растра и чернил. Сценарии из specs/canvas-view/spec.md.
//
// Единственный тест холста, которому нужно настоящее окно: остальные проверяют
// наблюдаемое состояние и обходятся без графа сцены. Окно поднимается
// offscreen на программном бэкенде отрисовки, а проверяется порядок слоёв, а не
// пиксели: полосы чернил этот бэкенд не растеризует вовсе, и в снимке окна их
// нет, даже когда они заведомо видны.

#include <QElapsedTimer>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>

#include <cmath>

#include "canvas/documentcanvas.h"
#include "fixtures.h"
#include "ink/inkdocument.h"
#include "pdf/pdfdocumentmodel.h"

namespace {

constexpr int kWindowWidth = 900;
constexpr int kWindowHeight = 700;
// Страница, достаточно далёкая, чтобы исходная покинула зону удержания.
constexpr int kFarPage = 150;

// Холст в окне: всё вместе, потому что порознь ни граф сцены, ни кадры не
// случаются.
struct Canvas
{
    PdfDocumentModel document;
    InkDocument ink;
    QQuickWindow window;
    DocumentCanvas *canvas = nullptr;

    bool open(const QString &pdf)
    {
        if (!document.open(pdf))
            return false;
        window.resize(kWindowWidth, kWindowHeight);
        window.show();
        canvas = new DocumentCanvas(window.contentItem());
        canvas->setSize(QSizeF(kWindowWidth, kWindowHeight));
        canvas->setDocument(&document);
        canvas->setInk(&ink);
        canvas->setPageMode(DocumentCanvas::PageMode::Single);
        return true;
    }
};

// Кадр графа сцены: grabWindow синхронно прогоняет и синхронизацию узлов, и
// отрисовку, то есть делает ровно то, что делает обычный кадр.
void renderFrames(QQuickWindow *window, int count = 1)
{
    for (int i = 0; i < count; ++i) {
        QCoreApplication::processEvents();
        window->grabWindow();
    }
}

// Растр страницы считает рабочий поток, поэтому ждать его приходится кадрами.
template <typename Predicate>
bool renderUntil(QQuickWindow *window, Predicate predicate, int timeoutMs = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QTest::qWait(5);
        renderFrames(window);
    }
    return predicate();
}

// Ведёт пером по прямой в координатах элемента.
void drawLine(DocumentCanvas *canvas, const QPointF &from, const QPointF &to)
{
    canvas->beginInkAt(from, 0.6, false);
    for (int i = 1; i <= 24; ++i)
        canvas->continueInkAt(from + (to - from) * (qreal(i) / 24.0), 0.6);
    canvas->endInk();
}

} // namespace

class TestPageSceneGraph : public QObject
{
    Q_OBJECT

private slots:
    // Сценарий «Возврат к странице по оглавлению»: написанное не пропадает,
    // если уйти по оглавлению далеко и вернуться.
    void inkStaysAboveRasterAfterOutlineJump()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));

        const QPointF at(450, 350);
        const int page = canvas->pageAtView(at);
        QCOMPARE(page, 0);
        drawLine(canvas, QPointF(350, 300), QPointF(550, 320));
        QCOMPARE(fixture.ink.strokeCount(), 1);

        renderFrames(&fixture.window, 3);
        QCOMPARE(canvas->inkLayering(page), DocumentCanvas::InkLayering::AboveRaster);

        // Уходим так далеко, что страница покидает зону удержания и её узел
        // снимается с графа сцены.
        canvas->goToPage(kFarPage);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return canvas->pendingNodeRemovalCount() == 0; }));
        QCOMPARE(canvas->inkLayering(page), DocumentCanvas::InkLayering::Incomplete);

        // И возвращаемся: чернила обязаны быть видны с того самого кадра, в
        // котором страница снова показана целиком, без единого действия
        // пользователя. Ждать, пока порядок слоёв починится сам, нельзя: это и
        // есть дефект.
        canvas->goToPage(page);
        // Первый кадр рисуется сразу, как в приложении: узел страницы успевает
        // получить чернила и не успевает получить растр - в этом порядке
        // дефект и жил.
        renderFrames(&fixture.window, 1);
        QCOMPARE(canvas->inkLayering(page), DocumentCanvas::InkLayering::Incomplete);
        QVERIFY(canvas->inkGeometryStrokeCount(page) > 0);

        QVERIFY(renderUntil(&fixture.window, [canvas, page] {
            return canvas->inkLayering(page) != DocumentCanvas::InkLayering::Incomplete;
        }));
        QCOMPARE(canvas->inkLayering(page), DocumentCanvas::InkLayering::AboveRaster);
        QCOMPARE(fixture.ink.strokeCount(), 1);
    }

    // Сценарии «Открытие сохранённой заметки» и «Растр страницы приходит после
    // чернил»: чернила попадают в граф сцены раньше растра, и растр обязан
    // встать под них.
    void loadedInkStaysAboveLateRaster()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);

        // Чернила из файла заметки: приходят до того, как страница отрисована.
        Stroke stroke;
        stroke.page = 0;
        stroke.width = 2.0;
        for (int i = 0; i <= 20; ++i)
            stroke.points.append({QPointF(60 + i * 8.0, 140), 0.5, i / 200.0});
        fixture.ink.loadStrokes({{1, stroke}}, 2);

        QCOMPARE(canvas->inkGeometryStrokeCount(0), 1);
        QVERIFY2(canvas->textureSizeOf(0).isEmpty(), "растр не должен успеть прийти раньше");

        // Кадр с заполнителем и чернилами, но ещё без растра.
        renderFrames(&fixture.window, 1);

        QVERIFY(renderUntil(&fixture.window, [canvas] {
            return canvas->inkLayering(0) != DocumentCanvas::InkLayering::Incomplete;
        }));
        QCOMPARE(canvas->inkLayering(0), DocumentCanvas::InkLayering::AboveRaster);
    }

    // Сценарий «Мокрый штрих поверх готовой страницы»: след пера виден поверх
    // растра и поверх ранее нанесённых чернил.
    void wetStrokeStaysAboveRasterAndDryInk()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));

        drawLine(canvas, QPointF(350, 300), QPointF(550, 300));
        renderFrames(&fixture.window, 3);

        // Штрих в процессе: мокрый след живёт отдельным слоем.
        canvas->beginInkAt(QPointF(350, 400), 0.6, false);
        for (int i = 1; i <= 24; ++i)
            canvas->continueInkAt(QPointF(350 + i * 8.0, 400), 0.6);
        renderFrames(&fixture.window, 2);
        QVERIFY(canvas->isDrawing());
        QCOMPARE(canvas->inkLayering(0), DocumentCanvas::InkLayering::AboveRaster);

        canvas->endInk();
        renderFrames(&fixture.window, 3);
        QCOMPARE(canvas->inkLayering(0), DocumentCanvas::InkLayering::AboveRaster);
    }

    // rnote-grade-stroke-fidelity, задача 5.3: полоса мокрого штриха
    // укорачивается, когда пересобирается провизорный хвост, и узлы графа сцены
    // обязаны покрывать ровно её - в том числе через границу кусков в 512
    // вершин.
    void wetStrokeNodesFollowShrinkingStrip()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));

        // Волна с чередованием разгона и остановок на месте: на разгоне хвост
        // длинный, на остановке модель догоняет перо, и хвост сжимается.
        const QPointF start(260, 300);
        QVERIFY(canvas->pageAtView(start) == 0);
        qreal x = start.x();
        qreal time = 3.0;
        QVERIFY(canvas->beginInkAt(start, 0.6, false, time));

        int peak = 0;
        int shrinks = 0;
        int previous = 0;
        for (int i = 1; i <= 400; ++i) {
            time += 1.0 / 240.0;
            if ((i / 20) % 2 == 0)
                x += 3.0;
            canvas->continueInkAt(QPointF(x, start.y() + 40.0 * std::sin((x - start.x()) / 30.0)),
                                  0.6, time);
            renderFrames(&fixture.window);

            const int strip = canvas->wetStripVertexCount();
            if (strip < 4)
                continue;
            QCOMPARE(canvas->wetInkCoveredVertexCount(0), strip);
            peak = qMax(peak, strip);
            if (strip < previous)
                ++shrinks;
            previous = strip;
        }

        QVERIFY2(shrinks > 0, "эталон должен укорачивать полосу, иначе тест ничего не проверил");
        QVERIFY2(peak > 512, qPrintable(QStringLiteral("полоса %1 не дошла до второго куска")
                                            .arg(peak)));
        canvas->endInk();
    }

    // Сценарий «Промах по оглавлению»: ушли и тут же вернулись - страница
    // показывается готовой, без промежуточного заполнителя.
    void quickReturnKeepsPageReady()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));
        drawLine(canvas, QPointF(350, 300), QPointF(550, 320));
        renderFrames(&fixture.window, 3);
        QCOMPARE(canvas->inkLayering(0), DocumentCanvas::InkLayering::AboveRaster);

        // Промах и возврат - в одном обработчике, кадра между ними нет.
        canvas->goToPage(kFarPage);
        canvas->goToPage(0);

        for (int frame = 0; frame < 8; ++frame) {
            renderFrames(&fixture.window, 1);
            QVERIFY2(!canvas->textureSizeOf(0).isEmpty(),
                     "страница осталась без растра после возврата");
            QCOMPARE(canvas->placeholderPageCount(), 0);
            QVERIFY2(canvas->inkLayering(0) == DocumentCanvas::InkLayering::AboveRaster,
                     "узел страницы пересобрали, и чернила ушли под растр");
        }
    }

    // Сценарий «Раззум после работы на большом масштабе»: страницы вокруг
    // показываются своим содержимым с первого же кадра после раззума. Проверять
    // это можно только с настоящим графом сцены: пока узел не снят, его
    // текстура жива, и без кадров страница «возвращается» даже тогда, когда в
    // приложении она была бы уже выгружена.
    void zoomOutAfterDeepZoomShowsNeighboursAtOnce()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::textbook()));
        DocumentCanvas *canvas = fixture.canvas;

        // Читаем как обычно: в кадре несколько страниц, и все отрисованы.
        canvas->setZoom(0.5);
        canvas->goToPage(4);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return canvas->placeholderPageCount() == 0; }));
        const int visibleBefore = canvas->visiblePageCount();
        QVERIFY(visibleBefore > 1);

        // Работаем с одной страницей на большом масштабе: соседи уходят из
        // кадра, и очередь снятия узлов разбирается до конца.
        canvas->setZoom(8.0);
        QVERIFY(renderUntil(&fixture.window, [canvas] {
            return canvas->pendingNodeRemovalCount() == 0
                && canvas->placeholderPageCount() == 0;
        }));
        QCOMPARE(canvas->visiblePageCount(), 1);

        // Быстрый раззум: страницы вокруг обязаны быть на месте в том же кадре,
        // а не появляться по мере отрисовки.
        canvas->setZoom(0.5);
        renderFrames(&fixture.window, 1);
        QCOMPARE(canvas->visiblePageCount(), visibleBefore);
        QCOMPARE(canvas->placeholderPageCount(), 0);
    }

    // Сценарий «Чернила соседних страниц переживают зум»: страница с чернилами
    // ушла из кадра из-за зума и вернулась при раззуме - чернила на месте.
    void inkOfNeighbourPageSurvivesDeepZoom()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::textbook()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->setZoom(0.5);
        canvas->goToPage(4);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return canvas->placeholderPageCount() == 0; }));

        // Пишем на верхней из видимых страниц.
        const QPointF at(450, 80);
        const int inkedPage = canvas->pageAtView(at);
        QVERIFY(inkedPage >= 0);
        drawLine(canvas, at, at + QPointF(120, 20));
        QCOMPARE(fixture.ink.strokeCount(), 1);
        renderFrames(&fixture.window, 3);
        QCOMPARE(canvas->inkLayering(inkedPage), DocumentCanvas::InkLayering::AboveRaster);

        // Уходим в большой масштаб на соседней странице: страница с чернилами
        // выходит из кадра, но не из зоны удержания.
        canvas->goToPage(inkedPage + 1);
        canvas->setZoom(8.0);
        QVERIFY(renderUntil(&fixture.window, [canvas] {
            return canvas->pendingNodeRemovalCount() == 0
                && canvas->placeholderPageCount() == 0;
        }));
        // Геометрия чернил не пересобиралась: она пережила зум вместе с зоной.
        QVERIFY2(canvas->inkGeometryStrokeCount(inkedPage) > 0,
                 "ink geometry dropped while zoomed in");

        // Возвращаемся к прежнему масштабу: чернила видны поверх растра сразу.
        canvas->setZoom(0.5);
        canvas->goToPage(inkedPage);
        renderFrames(&fixture.window, 1);
        QVERIFY(canvas->inkGeometryStrokeCount(inkedPage) > 0);
        QVERIFY(renderUntil(&fixture.window, [canvas, inkedPage] {
            return canvas->inkLayering(inkedPage) != DocumentCanvas::InkLayering::Incomplete;
        }));
        QCOMPARE(canvas->inkLayering(inkedPage), DocumentCanvas::InkLayering::AboveRaster);
        QCOMPARE(fixture.ink.strokeCount(), 1);
    }

    // Инвариант зоны удержания: узлы живут ровно у удерживаемых страниц, а
    // очередь снятия разбирается до конца. Сами чернила при этом целы.
    void nodesLiveOnlyForRetainedPages()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));
        drawLine(canvas, QPointF(350, 300), QPointF(550, 320));
        renderFrames(&fixture.window, 3);

        canvas->goToPage(kFarPage);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return canvas->pendingNodeRemovalCount() == 0; }));

        QCOMPARE(canvas->pendingNodeRemovalCount(), 0);
        QCOMPARE(canvas->inkLayering(0), DocumentCanvas::InkLayering::Incomplete);
        QCOMPARE(canvas->inkGeometryStrokeCount(0), 0);
        QVERIFY(canvas->textureSizeOf(0).isEmpty());
        // Вытесняется геометрия, а не данные.
        QCOMPARE(fixture.ink.strokeCount(), 1);
    }

    // Задача 4.6 add-ink-inserts: узел вкладыша стоит в слое поверх всех
    // страниц, чернила - под обрезкой, мокрый штрих вкладыша покрыт узлами
    // целиком и не рисуется на странице.
    void insertNodeStaysAboveAllPages()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));

        const QPointF at(380, 280);
        QCOMPARE(canvas->pageAtView(at), 0);
        canvas->setTool(DocumentCanvas::Tool::Insert);
        canvas->beginInkAt(at, 0.6, false);
        canvas->endInk();
        QCOMPARE(fixture.ink.insertCount(), 1);
        const quint64 id = fixture.ink.allInserts().first().id;

        drawLine(canvas, QPointF(400, 300), QPointF(480, 320));
        QCOMPARE(fixture.ink.strokesInInsert(id).size(), 1);
        renderFrames(&fixture.window, 3);
        QCOMPARE(canvas->insertLayering(id), DocumentCanvas::InsertLayering::AbovePages);

        // Мокрый штрих во вкладыше.
        const QPointF from(400, 340);
        canvas->beginInkAt(from, 0.6, false);
        for (int i = 1; i <= 24; ++i)
            canvas->continueInkAt(from + QPointF(4.0 * i, 1.5 * i), 0.6);
        renderFrames(&fixture.window, 1);
        QVERIFY(canvas->wetStripVertexCount() >= 4);
        QCOMPARE(canvas->wetInsertCoveredVertexCount(id), canvas->wetStripVertexCount());
        QCOMPARE(canvas->wetInkCoveredVertexCount(0), -1);
        QCOMPARE(canvas->insertLayering(id), DocumentCanvas::InsertLayering::AbovePages);
        canvas->endInk();
        renderFrames(&fixture.window, 2);
        QCOMPARE(canvas->wetInsertCoveredVertexCount(id), -1);
        QCOMPARE(fixture.ink.strokesInInsert(id).size(), 2);

        // Соседние страницы, попавшие в граф сцены позже вкладыша, его не
        // перекрывают.
        canvas->panBy(QPointF(0, -300));
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return canvas->placeholderPageCount() == 0; }));
        QCOMPARE(canvas->insertLayering(id), DocumentCanvas::InsertLayering::AbovePages);

        // Свёрнутый вкладыш узла не имеет.
        canvas->closeInsert(id);
        renderFrames(&fixture.window, 1);
        QCOMPARE(canvas->insertLayering(id), DocumentCanvas::InsertLayering::Missing);
        canvas->openInsert(id);
        renderFrames(&fixture.window, 1);
        QCOMPARE(canvas->insertLayering(id), DocumentCanvas::InsertLayering::AbovePages);
    }

    // ink-beauty-pass, D7: сухие и мокрые чернила нарисованы материалом
    // чернил, у мокрого слоя свой тип (иначе он сливается в батч с сухими),
    // куски мокрого штриха не сливаются друг с другом.
    void inkNodesUseInkMaterial()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));
        drawLine(canvas, QPointF(350, 300), QPointF(550, 300));
        // Законченный штрих - в недавних слотах; в сухой слой его переносит простой.
        canvas->flushRecentInk();
        renderFrames(&fixture.window, 3);

        // Длинный мокрый штрих - больше одного куска по 512 вершин.
        qreal time = 3.0;
        QCOMPARE(canvas->pageAtView(QPointF(300, 420)), 0);
        QVERIFY(canvas->beginInkAt(QPointF(300, 420), 0.6, false, time));
        for (int i = 1; i <= 400; ++i) {
            time += 1.0 / 240.0;
            canvas->continueInkAt(QPointF(300 + i * 0.9, 420 + 30.0 * std::sin(i / 5.0)), 0.6,
                                  time);
        }
        renderFrames(&fixture.window, 2);
        QVERIFY2(canvas->wetStripVertexCount() > 512,
                 qPrintable(QStringLiteral("полоса %1 не дошла до второго куска")
                                .arg(canvas->wetStripVertexCount())));

        const DocumentCanvas::InkMaterialStats stats = canvas->inkMaterialStats(0);
        QVERIFY(stats.dryNodes > 0);
        QVERIFY(stats.wetNodes > 1);
        QCOMPARE(stats.foreignNodes, 0);
        QVERIFY2(stats.dryAndWetTypesDiffer, "мокрый слой с типом сухого сольётся с ним в батч");
        QVERIFY2(stats.wetChunksSeparate, "куски мокрого штриха сливаются в один батч");

        canvas->endInk();
        renderFrames(&fixture.window, 2);
        QCOMPARE(canvas->inkMaterialStats(0).wetNodes, 0);
        QCOMPARE(canvas->inkMaterialStats(0).foreignNodes, 0);
    }

    // Узлы мокрого слоя - пул, живущий вместе с узлом страницы: касание пером
    // и следующий штрих узлов не добавляют. Добавление узла под корнем батча
    // перезалило бы слитый батч сухих чернил страницы (design.md
    // ink-beauty-pass, D7). Опустевший после отрыва кусок обязан ожить со
    // следующим штрихом.
    void wetLayerReusesNodePool()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));
        drawLine(canvas, QPointF(350, 300), QPointF(550, 300));
        renderFrames(&fixture.window, 3);

        // Пул уже есть до касания, пустой.
        const int pool = canvas->inkMaterialStats(0).wetPoolNodes;
        QVERIFY(pool > 1);
        QCOMPARE(canvas->inkMaterialStats(0).wetNodes, 0);
        QCOMPARE(canvas->wetInkCoveredVertexCount(0), -1);

        qreal time = 3.0;
        const auto stroke = [&](qreal y, int samples) {
            QVERIFY(canvas->beginInkAt(QPointF(300, y), 0.6, false, time));
            for (int i = 1; i <= samples; ++i) {
                time += 1.0 / 240.0;
                canvas->continueInkAt(QPointF(300 + i * 0.9, y + 30.0 * std::sin(i / 5.0)), 0.6,
                                      time);
            }
            renderFrames(&fixture.window, 1);
        };

        // Касание и короткий штрих - в пределах пула.
        stroke(420, 40);
        QVERIFY(canvas->wetStripVertexCount() >= 4);
        QCOMPARE(canvas->inkMaterialStats(0).wetNodes, 1);
        QCOMPARE(canvas->inkMaterialStats(0).wetPoolNodes, pool);
        QCOMPARE(canvas->wetInkCoveredVertexCount(0), canvas->wetStripVertexCount());
        canvas->endInk();
        renderFrames(&fixture.window, 2);
        QCOMPARE(canvas->inkMaterialStats(0).wetNodes, 0);
        QCOMPARE(canvas->inkMaterialStats(0).wetPoolNodes, pool);

        // Длинный штрих переполняет пул: он растёт один раз.
        stroke(480, 400);
        const int strip = canvas->wetStripVertexCount();
        const int grown = canvas->inkMaterialStats(0).wetPoolNodes;
        QVERIFY(grown > pool);
        QCOMPARE(canvas->wetInkCoveredVertexCount(0), strip);
        canvas->endInk();
        renderFrames(&fixture.window, 2);
        QCOMPARE(canvas->inkMaterialStats(0).wetNodes, 0);

        // Такой же следующий штрих рисуется опустевшими кусками того же пула.
        stroke(560, 400);
        const DocumentCanvas::InkMaterialStats second = canvas->inkMaterialStats(0);
        QVERIFY(second.wetNodes > 1);
        QCOMPARE(second.wetPoolNodes, grown);
        QCOMPARE(second.foreignNodes, 0);
        QVERIFY(second.wetChunksSeparate);
        QCOMPARE(canvas->wetInkCoveredVertexCount(0), canvas->wetStripVertexCount());

        // Отрыв и касание без кадра между ними: новый штрих не наследует
        // куски прошлого.
        canvas->endInk();
        stroke(640, 20);
        QVERIFY(canvas->wetStripVertexCount() >= 4);
        QCOMPARE(canvas->inkMaterialStats(0).wetNodes, 1);
        QCOMPARE(canvas->wetInkCoveredVertexCount(0), canvas->wetStripVertexCount());
        canvas->endInk();
        renderFrames(&fixture.window, 2);
        QCOMPARE(canvas->inkMaterialStats(0).wetPoolNodes, grown);
        QCOMPARE(canvas->wetInkCoveredVertexCount(0), -1);
    }

    // Отрыв пера кладёт штрих в слот недавних штрихов: узлы не добавляются,
    // сухой слой не трогается (добавление узла под корнем батча перезалило бы
    // слитый батч сухих чернил страницы). В сухой слой недавние переезжают
    // разом, когда перо и вид в покое (ink-beauty-pass, рецензия группы 12).
    void liftFillsRecentSlotAndIdleMovesItToDry()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));
        drawLine(canvas, QPointF(350, 300), QPointF(550, 300));
        canvas->flushRecentInk();
        renderFrames(&fixture.window, 3);
        const DocumentCanvas::InkMaterialStats before = canvas->inkMaterialStats(0);
        QCOMPARE(before.dryNodes, 1);
        QCOMPARE(before.recentNodes, 0);
        QVERIFY(before.recentPoolNodes > 1);
        const int rebuilds = canvas->dryInkNodeRebuildCount();

        drawLine(canvas, QPointF(350, 340), QPointF(550, 340));
        renderFrames(&fixture.window, 2);
        drawLine(canvas, QPointF(350, 380), QPointF(550, 380));
        renderFrames(&fixture.window, 2);
        DocumentCanvas::InkMaterialStats lifted = canvas->inkMaterialStats(0);
        QCOMPARE(lifted.dryNodes, 1);
        QCOMPARE(lifted.recentNodes, 2);
        QCOMPARE(lifted.recentPoolNodes, before.recentPoolNodes);
        QCOMPARE(lifted.wetPoolNodes, before.wetPoolNodes);
        QCOMPARE(lifted.foreignNodes, 0);
        QVERIFY(lifted.dryAndWetTypesDiffer);
        QCOMPARE(canvas->dryInkNodeRebuildCount(), rebuilds);
        QCOMPARE(canvas->inkLayering(0), DocumentCanvas::InkLayering::AboveRaster);
        QVERIFY(canvas->recentInkFlushScheduled());

        // Простой: недавние - в сухом слое, слоты пусты, узлов пула столько же.
        QVERIFY(renderUntil(&fixture.window, [canvas] {
            return canvas->inkMaterialStats(0).recentNodes == 0;
        }, 3000));
        lifted = canvas->inkMaterialStats(0);
        QCOMPARE(lifted.dryNodes, 3);
        QCOMPARE(lifted.recentPoolNodes, before.recentPoolNodes);
        QCOMPARE(canvas->dryInkNodeRebuildCount(), rebuilds + 1);
        QVERIFY(!canvas->recentInkFlushScheduled());
        QCOMPARE(canvas->inkLayering(0), DocumentCanvas::InkLayering::AboveRaster);
    }

    // Стирание, отмена и повтор недавнего штриха до переноса: стёртый штрих
    // исчезает, ни один не рисуется дважды - показано ровно то, что в
    // геометрии страницы.
    void recentStrokesFollowEraseUndoAndRedo()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));
        drawLine(canvas, QPointF(350, 300), QPointF(550, 300));
        canvas->flushRecentInk();
        renderFrames(&fixture.window, 2);

        const auto shown = [canvas] {
            const DocumentCanvas::InkMaterialStats stats = canvas->inkMaterialStats(0);
            return stats.dryNodes + stats.recentNodes;
        };
        drawLine(canvas, QPointF(350, 340), QPointF(550, 340));
        renderFrames(&fixture.window, 1);
        drawLine(canvas, QPointF(350, 380), QPointF(550, 380));
        renderFrames(&fixture.window, 1);
        QCOMPARE(canvas->inkMaterialStats(0).recentNodes, 2);
        QCOMPARE(shown(), 3);

        // Ластик по недавнему штриху.
        canvas->setTool(DocumentCanvas::Tool::Eraser);
        drawLine(canvas, QPointF(450, 330), QPointF(450, 350));
        canvas->setTool(DocumentCanvas::Tool::Pen);
        renderFrames(&fixture.window, 1);
        QCOMPARE(fixture.ink.strokeCount(), 2);
        QCOMPARE(canvas->inkGeometryStrokeCount(0), 2);
        QCOMPARE(shown(), 2);

        // Отмена стирания возвращает штрих, отмена его - снова убирает.
        fixture.ink.undo();
        renderFrames(&fixture.window, 1);
        QCOMPARE(shown(), 3);
        drawLine(canvas, QPointF(350, 420), QPointF(550, 420));
        renderFrames(&fixture.window, 1);
        QCOMPARE(shown(), 4);
        fixture.ink.undo();
        renderFrames(&fixture.window, 1);
        QCOMPARE(canvas->inkGeometryStrokeCount(0), 3);
        QCOMPARE(shown(), 3);
        fixture.ink.redo();
        renderFrames(&fixture.window, 1);
        QCOMPARE(shown(), 4);
        QVERIFY(canvas->inkMaterialStats(0).recentNodes > 0);

        // Перенос по простою после всего этого - то же число штрихов.
        canvas->flushRecentInk();
        renderFrames(&fixture.window, 1);
        QCOMPARE(canvas->inkMaterialStats(0).recentNodes, 0);
        QCOMPARE(canvas->inkMaterialStats(0).dryNodes, 4);
        QCOMPARE(canvas->inkLayering(0), DocumentCanvas::InkLayering::AboveRaster);
    }

    // Отмена единственного штриха страницы, пока он в недавних слотах:
    // страница пуста, слот тоже - сухого слоя, вместе с которым он чистился
    // бы, здесь нет вовсе.
    void undoOfOnlyRecentStrokeLeavesPageEmpty()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));
        drawLine(canvas, QPointF(350, 300), QPointF(550, 300));
        renderFrames(&fixture.window, 1);
        QCOMPARE(canvas->inkMaterialStats(0).recentNodes, 1);
        QCOMPARE(canvas->inkMaterialStats(0).dryNodes, 0);

        fixture.ink.undo();
        renderFrames(&fixture.window, 1);
        QCOMPARE(canvas->inkGeometryStrokeCount(0), 0);
        QCOMPARE(canvas->inkMaterialStats(0).recentNodes, 0);
        QCOMPARE(canvas->inkMaterialStats(0).dryNodes, 0);
        QCOMPARE(canvas->inkLayering(0), DocumentCanvas::InkLayering::Incomplete);
    }

    // Слотов не хватило - всё недавнее переезжает в сухой слой сразу, ни один
    // штрих не теряется и не двоится.
    void recentOverflowMovesToDryAtOnce()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));
        renderFrames(&fixture.window, 1);
        const int poolSize = canvas->inkMaterialStats(0).recentPoolNodes;
        QVERIFY(poolSize > 0);
        for (int i = 0; i <= poolSize; ++i) {
            const qreal y = 120 + i * 6.0;
            drawLine(canvas, QPointF(300, y), QPointF(500, y));
            renderFrames(&fixture.window, 1);
            const DocumentCanvas::InkMaterialStats stats = canvas->inkMaterialStats(0);
            QCOMPARE(stats.dryNodes + stats.recentNodes, i + 1);
            QCOMPARE(stats.recentPoolNodes, poolSize);
        }
        const DocumentCanvas::InkMaterialStats stats = canvas->inkMaterialStats(0);
        QCOMPARE(stats.dryNodes, poolSize + 1);
        QCOMPARE(stats.recentNodes, 0);
    }

    // Сценарий «Изменение масштаба» (ink-input, «Сглаживание кромки чернил»):
    // кромка пересчитывается в шейдере, геометрия штрихов не пересобирается.
    void zoomDoesNotRebuildInkGeometry()
    {
        Canvas fixture;
        QVERIFY(fixture.open(fixtures::large()));
        DocumentCanvas *canvas = fixture.canvas;

        canvas->goToPage(0);
        QVERIFY(renderUntil(&fixture.window,
                            [canvas] { return !canvas->textureSizeOf(0).isEmpty(); }));
        drawLine(canvas, QPointF(350, 300), QPointF(550, 320));
        // Законченный штрих - в недавних слотах; в сухой слой его переносит простой.
        canvas->flushRecentInk();
        renderFrames(&fixture.window, 3);
        QVERIFY(canvas->inkMaterialStats(0).dryNodes > 0);

        const int rebuilds = canvas->dryInkNodeRebuildCount();
        for (const qreal zoom : {2.0, 0.6, 1.4, 3.0}) {
            canvas->setZoom(zoom);
            renderFrames(&fixture.window, 1);
        }
        QCOMPARE(canvas->dryInkNodeRebuildCount(), rebuilds);
        QVERIFY(canvas->inkMaterialStats(0).dryNodes > 0);
    }
};

// Графический бэкенд выбирается до создания приложения, поэтому QTEST_MAIN
// здесь не годится.
int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);

    QGuiApplication app(argc, argv);
    TestPageSceneGraph test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_pagescenegraph.moc"
