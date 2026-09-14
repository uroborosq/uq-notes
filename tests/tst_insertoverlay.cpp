// Задачи 6.3 и 6.4 add-ink-inserts: элементы управления вкладышами в Main.qml.
// Сценарий «Касание значка не рисует» из specs/ink-inserts/spec.md и запись
// вкладышей при сохранении из интерфейса.
//
// Тест поднимает настоящее окно приложения (offscreen, программный рендер) и
// жмёт мышью по элементам управления: только так проверяется, что нажатие на
// значок забирает QML, а не холст под ним. Мышь для холста - то же, что перо.

#include <QFile>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QTest>

#include "canvas/documentcanvas.h"
#include "fixtures.h"
#include "ink/inkdocument.h"
#include "storage/notestore.h"

namespace {

void sendMouse(QQuickWindow *window, QEvent::Type type, const QPointF &scenePoint,
               Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, scenePoint, scenePoint, window->mapToGlobal(scenePoint), button,
                      buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &event);
}

void click(QQuickWindow *window, const QPointF &scenePoint)
{
    sendMouse(window, QEvent::MouseButtonPress, scenePoint, Qt::LeftButton, Qt::LeftButton);
    sendMouse(window, QEvent::MouseButtonRelease, scenePoint, Qt::LeftButton, Qt::NoButton);
}

void drag(QQuickWindow *window, const QPointF &from, const QPointF &to, int steps)
{
    sendMouse(window, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= steps; ++i) {
        sendMouse(window, QEvent::MouseMove, from + (to - from) * (qreal(i) / steps),
                  Qt::NoButton, Qt::LeftButton);
    }
    sendMouse(window, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

// Делегаты Repeater не всегда получают QObject-родителя, поэтому поиск идёт по
// дереву элементов, а не через findChild.
QQuickItem *findItem(QQuickItem *root, const QString &objectName)
{
    if (!root)
        return nullptr;
    if (root->objectName() == objectName)
        return root;
    const QList<QQuickItem *> children = root->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *found = findItem(child, objectName))
            return found;
    }
    return nullptr;
}

QPointF centerOf(QQuickItem *item)
{
    return item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0));
}

} // namespace

class TestInsertOverlay : public QObject
{
    Q_OBJECT

private slots:
    void controlsDriveInsertsWithoutInkingThePage()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pdf = dir.filePath(QStringLiteral("book.pdf"));
        QVERIFY(QFile::copy(fixtures::textbook(), pdf));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("initialFile"), pdf);
        engine.loadFromModule("UqNotes", "Main");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *canvas = window->findChild<DocumentCanvas *>(QStringLiteral("canvas"));
        QVERIFY(canvas);
        QTRY_VERIFY(canvas->width() > 0 && canvas->pageCount() > 0);
        InkDocument *ink = canvas->ink();
        QVERIFY(ink);
        canvas->goToPage(0);
        QCoreApplication::processEvents();

        const auto scene = [canvas](const QPointF &viewPoint) {
            return canvas->mapToScene(viewPoint);
        };
        const QPointF at = canvas->documentToView(canvas->layout().pageRect(0).topLeft()
                                                  + QPointF(100, 150));
        QCOMPARE(canvas->pageAtView(at), 0);

        // Касание страницы инструментом «Вкладыш».
        canvas->setTool(DocumentCanvas::Tool::Insert);
        click(window, scene(at));
        QCOMPARE(ink->insertCount(), 1);
        const quint64 id = ink->allInserts().first().id;
        QCOMPARE(canvas->tool(), DocumentCanvas::Tool::Pen);

        // Письмо во вкладыше.
        drag(window, scene(at + QPointF(20, 20)), scene(at + QPointF(90, 40)), 12);
        QCOMPARE(ink->strokesInInsert(id).size(), 1);
        QVERIFY(ink->allStrokes().isEmpty());

        // Ручка размера тянет правый нижний угол и штриха не оставляет.
        QCoreApplication::processEvents();
        QQuickItem *resize = findItem(window->contentItem(), QStringLiteral("insertResize"));
        QVERIFY(resize);
        QVERIFY(resize->isVisible());
        const QRectF before = ink->insertById(id).rect;
        const QPointF handle = centerOf(resize);
        drag(window, handle, handle + QPointF(80, 50), 8);
        const QRectF after = ink->insertById(id).rect;
        QCOMPARE(after.topLeft(), before.topLeft());
        QVERIFY2(qAbs(after.width() - (before.width() + 80 / canvas->zoom())) < 1.0,
                 qPrintable(QStringLiteral("ширина %1 вместо %2")
                                .arg(after.width())
                                .arg(before.width() + 80 / canvas->zoom())));
        QVERIFY(qAbs(after.height() - (before.height() + 50 / canvas->zoom())) < 1.0);
        QCOMPARE(ink->strokeCount(), 1);

        // «Свернуть».
        QQuickItem *close = findItem(window->contentItem(), QStringLiteral("insertClose"));
        QVERIFY(close);
        QVERIFY(close->isVisible());
        click(window, centerOf(close));
        QVERIFY(!ink->insertById(id).open);
        QCOMPARE(ink->strokeCount(), 1);

        // Задача 6.4: сохранение из интерфейса дописывает вкладыши в заметку.
        QVERIFY(QMetaObject::invokeMethod(window, "persist"));
        {
            InkDocument saved;
            NoteStore store;
            QVERIFY(store.openNote(NoteStore::notePathFor(pdf)));
            QVERIFY(store.load(&saved));
            QVERIFY(saved.hasInsert(id));
            QVERIFY(!saved.insertById(id).open);
            QCOMPARE(saved.insertById(id).rect, after);
            QCOMPARE(saved.strokesInInsert(id).size(), 1);
        }

        // Сценарий «Касание значка не рисует»: значок стоит над страницей,
        // касание его раскрывает, а на странице не появляется точки.
        QCoreApplication::processEvents();
        QQuickItem *anchor = findItem(window->contentItem(), QStringLiteral("insertAnchor"));
        QVERIFY(anchor);
        QVERIFY(anchor->isVisible());
        const QPointF anchorPoint = centerOf(anchor);
        QCOMPARE(canvas->pageAtView(canvas->mapFromScene(anchorPoint)), 0);
        QCOMPARE(canvas->tool(), DocumentCanvas::Tool::Pen);
        click(window, anchorPoint);
        QVERIFY(ink->insertById(id).open);
        QCOMPARE(ink->strokeCount(), 1);
        QVERIFY(ink->allStrokes().isEmpty());
        QVERIFY(!canvas->isStylusActive());

        // «Удалить» и отмена.
        QCoreApplication::processEvents();
        QQuickItem *remove = findItem(window->contentItem(), QStringLiteral("insertDelete"));
        QVERIFY(remove);
        QVERIFY(remove->isVisible());
        click(window, centerOf(remove));
        QCOMPARE(ink->insertCount(), 0);
        QVERIFY(ink->allStrokes().isEmpty());
        ink->undo();
        QCOMPARE(ink->insertCount(), 1);
        QCOMPARE(ink->strokesInInsert(id).size(), 1);
    }

    // Элементы управления стоят на рамке вкладыша при восстановленном виде и
    // после смены размера окна: сдвиг вида меняется раскладкой, а не жестом,
    // и привязки обязаны это увидеть.
    void controlsFollowViewAfterLayout()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pdf = dir.filePath(QStringLiteral("book.pdf"));
        QVERIFY(QFile::copy(fixtures::textbook(), pdf));

        quint64 open = 0;
        quint64 closed = 0;
        {
            InkDocument ink;
            NoteStore store;
            QVERIFY(store.create(NoteStore::notePathFor(pdf), pdf));
            store.attach(&ink);
            open = ink.addInsert(0, QRectF(80, 120, 220, 150));
            closed = ink.addInsert(0, QRectF(380, 420, 200, 140));
            for (const quint64 insert : {open, closed}) {
                Stroke stroke;
                stroke.insert = insert;
                stroke.width = 1.6;
                for (int i = 0; i <= 20; ++i)
                    stroke.points.append({QPointF(10 + i * 5.0, 40), 0.6, i / 200.0});
                QVERIFY(ink.addStroke(stroke) != 0);
            }
            ink.setInsertOpen(closed, false);
            store.rememberView(0, 0.5, 0, 0, 20);
            QVERIFY(store.flush());
        }

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("initialFile"), pdf);
        engine.loadFromModule("UqNotes", "Main");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *canvas = window->findChild<DocumentCanvas *>(QStringLiteral("canvas"));
        QVERIFY(canvas);
        QTRY_VERIFY(canvas->width() > 0 && canvas->ink() && canvas->ink()->insertCount() == 2);
        QTest::qWait(100);
        QVERIFY2(qAbs(canvas->zoom() - 0.5) < 1e-6, qPrintable(QString::number(canvas->zoom())));

        const auto checkPlacement = [window, canvas, open, closed]() -> QString {
            QQuickItem *anchor = findVisibleItem(window->contentItem(), QStringLiteral("insertAnchor"));
            QQuickItem *resize = findVisibleItem(window->contentItem(), QStringLiteral("insertResize"));
            if (!anchor || !resize)
                return QStringLiteral("нет видимого значка или ручки");
            const QPointF expectedAnchor = canvas->mapToScene(
                canvas->documentToView(canvas->insertDocumentRect(closed).topLeft()));
            const QPointF expectedHandle = canvas->mapToScene(
                canvas->documentToView(canvas->insertDocumentRect(open).bottomRight()));
            const QPointF a = centerOf(anchor);
            const QPointF h = centerOf(resize);
            if (QLineF(a, expectedAnchor).length() > 1.0 || QLineF(h, expectedHandle).length() > 1.0) {
                return QStringLiteral("значок (%1, %2) вместо (%3, %4), ручка (%5, %6) вместо (%7, %8)")
                    .arg(a.x()).arg(a.y()).arg(expectedAnchor.x()).arg(expectedAnchor.y())
                    .arg(h.x()).arg(h.y()).arg(expectedHandle.x()).arg(expectedHandle.y());
            }
            return QString();
        };

        QString problem = checkPlacement();
        QVERIFY2(problem.isEmpty(), qPrintable(problem));

        window->setWidth(window->width() - 300);
        QTRY_VERIFY2((problem = checkPlacement()).isEmpty(), qPrintable(problem));

        // Сценарий «Значок меняет размер вместе с масштабом»: 56 точек
        // страницы на масштаб, но не мельче 28 и не крупнее 112 пикселей.
        const auto anchorWidth = [window]() {
            QQuickItem *anchor =
                findVisibleItem(window->contentItem(), QStringLiteral("insertAnchor"));
            return anchor ? anchor->width() : -1.0;
        };
        const QPointF center(canvas->width() / 2.0, canvas->height() / 2.0);
        canvas->zoomAt(center, 1.0);
        QCOMPARE(anchorWidth(), 56.0);
        canvas->zoomAt(center, 1.5);
        QCOMPARE(anchorWidth(), 84.0);
        canvas->zoomAt(center, 0.25);
        QCOMPARE(anchorWidth(), 28.0);
        canvas->zoomAt(center, 8.0);
        QCOMPARE(anchorWidth(), 112.0);
        // И при любом размере значок центрирован на точке установки.
        QVERIFY2((problem = checkPlacement()).isEmpty(), qPrintable(problem));
    }

private:
    static QQuickItem *findVisibleItem(QQuickItem *root, const QString &objectName)
    {
        if (!root)
            return nullptr;
        if (root->objectName() == objectName && root->isVisible())
            return root;
        const QList<QQuickItem *> children = root->childItems();
        for (QQuickItem *child : children) {
            if (QQuickItem *found = findVisibleItem(child, objectName))
                return found;
        }
        return nullptr;
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
    TestInsertOverlay test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_insertoverlay.moc"
