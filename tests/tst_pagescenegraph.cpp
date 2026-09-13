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
