// Задача 2.1 move-outline-panel-right: сценарии «Панель открыта» и «Панель
// скрыта» требования «Расположение панели оглавления» из
// specs/outline-navigation/spec.md.
//
// Тест поднимает настоящее окно приложения (offscreen): положение панели - это
// раскладка Main.qml, и проверить его можно только на собранном окне.

#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include "canvas/documentcanvas.h"
#include "fixtures.h"

namespace {

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

QRectF sceneRect(QQuickItem *item)
{
    return QRectF(item->mapToScene(QPointF(0, 0)), QSizeF(item->width(), item->height()));
}

QString describe(QQuickItem *canvas, QQuickItem *panel)
{
    const QRectF c = sceneRect(canvas);
    const QRectF p = sceneRect(panel);
    return QStringLiteral("холст [%1, %2], панель [%3, %4]")
        .arg(c.left()).arg(c.right()).arg(p.left()).arg(p.right());
}

} // namespace

class TestOutlinePanel : public QObject
{
    Q_OBJECT

private slots:
    void panelStaysAtRightEdge()
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
        QQuickItem *panel = findItem(window->contentItem(), QStringLiteral("outlinePanel"));
        QVERIFY(panel);

        const qreal windowWidth = window->contentItem()->width();

        // «Панель открыта»: панель у правого края, документ слева от неё.
        QTRY_VERIFY(panel->isVisible() && panel->width() > 0);
        QTRY_COMPARE(sceneRect(panel).right(), windowWidth);
        QTRY_VERIFY2(sceneRect(canvas).left() == 0.0
                         && sceneRect(canvas).right() == sceneRect(panel).left(),
                     qPrintable(describe(canvas, panel)));

        // «Панель скрыта»: документ во всю ширину окна.
        QVERIFY(window->setProperty("sidebarVisible", false));
        QTRY_VERIFY(!panel->isVisible());
        QTRY_COMPARE(canvas->width(), windowWidth);
        QCOMPARE(sceneRect(canvas).left(), 0.0);

        // Повторный показ возвращает панель к правому краю.
        QVERIFY(window->setProperty("sidebarVisible", true));
        QTRY_VERIFY(panel->isVisible());
        QTRY_COMPARE(sceneRect(panel).right(), windowWidth);
        QTRY_VERIFY2(sceneRect(canvas).left() == 0.0
                         && sceneRect(canvas).right() == sceneRect(panel).left(),
                     qPrintable(describe(canvas, panel)));
    }
};

QTEST_MAIN(TestOutlinePanel)
#include "tst_outlinepanel.moc"
