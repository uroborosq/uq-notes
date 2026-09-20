// Задачи 2.1, 3.1-3.3 fix-idle-repaint-loop: требования specs/idle-power/spec.md.
//
// Структурная половина проверки покоя. Само требование - «в простое кадров
// нет» - проверяется в tst_idleframes, и только на настоящей платформе: под
// QT_QPA_PLATFORM=offscreen, с которым идут тесты этого проекта, цикл рендера
// крутится вхолостую всегда, и ни один сигнал окна там не отличает покой от
// петли (design.md, D2).
//
// Зато от платформы не зависит то, из-за чего петля возникала: живая невидимая
// машинерия. Элемент, который показывается в редком режиме, но создан и
// пересчитывается всегда, рано или поздно начинает держать сцену грязной -
// так было и со сводкой кадров, и с индикатором экспорта. Здесь проверяется,
// что таких элементов в дереве нет, пока их режим не наступил, и что
// инструментация кадров молчит, пока замер не запрошен.
//
// Тест намеренно ничего не знает про Loader: он ищет элемент по objectName и
// требует, чтобы его не было. Любая реализация, которая этого добивается,
// тест проходит.

#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <memory>

#include "canvas/documentcanvas.h"
#include "export/noteexporter.h"
#include "fixtures.h"
#include "ink/inkdocument.h"
#include "pdf/pdfdocumentmodel.h"
#include "perf/framestats.h"

namespace {

// Делегаты и содержимое загрузчиков не всегда получают QObject-родителя,
// поэтому поиск идёт по дереву элементов, а не через findChild - как в
// tst_penonlymode.
QQuickItem *findNamed(QQuickItem *root, const QString &objectName)
{
    if (!root)
        return nullptr;
    if (root->objectName() == objectName)
        return root;
    const QList<QQuickItem *> children = root->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *found = findNamed(child, objectName))
            return found;
    }
    return nullptr;
}

} // namespace

class TestIdlePower : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
    DocumentCanvas *m_canvas = nullptr;
    FrameStats *m_stats = nullptr;

    void loadWindow()
    {
        const QString pdf = m_dir->filePath(QStringLiteral("book.pdf"));
        QVERIFY(QFile::copy(fixtures::textbook(), pdf));

        m_engine = std::make_unique<QQmlApplicationEngine>();
        m_engine->rootContext()->setContextProperty(QStringLiteral("initialFile"), pdf);
        m_engine->loadFromModule("UqNotes", "Main");
        QVERIFY(!m_engine->rootObjects().isEmpty());
        m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().constFirst());
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));

        m_canvas = m_window->findChild<DocumentCanvas *>(QStringLiteral("canvas"));
        QVERIFY(m_canvas);
        m_stats = m_window->findChild<FrameStats *>();
        QVERIFY(m_stats);
        QTRY_VERIFY(m_canvas->width() > 0 && m_canvas->pageCount() > 0);
    }

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        loadWindow();
    }

    void cleanup()
    {
        m_stats = nullptr;
        m_canvas = nullptr;
        m_window = nullptr;
        m_engine.reset();
        m_dir.reset();
    }

    // Требование «Отсутствие кадров в простое»: приложение не держит окно в
    // состоянии, требующем перерисовки, из-за непоказанного элемента. Проверка
    // со стороны причины: непоказанного элемента вовсе нет.
    void hiddenMachineryIsNotBuilt_data()
    {
        QTest::addColumn<QString>("objectName");
        QTest::addColumn<QString>("what");
        QTest::newRow("сводка кадров")
            << QStringLiteral("frameStatsHud") << QStringLiteral("замер не запрошен");
        QTest::newRow("индикатор экспорта")
            << QStringLiteral("exportProgressPanel") << QStringLiteral("экспорт не идёт");
    }

    void hiddenMachineryIsNotBuilt()
    {
        QFETCH(QString, objectName);
        QFETCH(QString, what);

        QQuickItem *item = findNamed(m_window->contentItem(), objectName);
        if (item) {
            QFAIL(qPrintable(
                QStringLiteral("«%1» создан в дереве, хотя %2. Непоказанный элемент "
                               "продолжает пересчитывать привязки и способен держать "
                               "сцену грязной - создавать его до времени нельзя.")
                    .arg(objectName, what)));
        }
    }

    // Обратная сторона того же требования: не создавать заранее - не значит
    // не показать вовсе. В своём режиме панель обязана появиться, а привязки
    // внутри неё - разрешиться (ссылки на exporter из sourceComponent).
    void exportPanelAppearsWhileExporting()
    {
        auto *exporter = m_window->findChild<NoteExporter *>();
        QVERIFY(exporter);
        auto *ink = m_window->findChild<InkDocument *>();
        QVERIFY(ink);
        auto *document = m_window->findChild<PdfDocumentModel *>();
        QVERIFY(document);

        const QString output = m_dir->filePath(QStringLiteral("export.pdf"));
        QVERIFY(exporter->exportToPdf(document, ink, output));

        QQuickItem *panel = nullptr;
        QTRY_VERIFY((panel = findNamed(m_window->contentItem(),
                                       QStringLiteral("exportProgressPanel"))));
        QCOMPARE(panel->width(), 360.0);
        QCOMPARE(panel->height(), 110.0);

        // Полоса нарисована своими руками (design.md, D5), поэтому её
        // заполнение - обычная привязка, и проверять надо именно её: ширина
        // заполнения должна следовать доле готовности.
        QQuickItem *fill = findNamed(m_window->contentItem(),
                                     QStringLiteral("exportProgressFill"));
        QVERIFY(fill);
        QQuickItem *groove = fill->parentItem();
        QVERIFY(groove);
        QTRY_COMPARE(fill->width(), groove->width() * exporter->progress());
        QVERIFY(fill->width() >= 0.0);
        QVERIFY(fill->width() <= groove->width());

        // По завершении панель снова исчезает, а не просто прячется.
        QTRY_VERIFY(!exporter->isBusy());
        QTRY_VERIFY(!findNamed(m_window->contentItem(), QStringLiteral("exportProgressPanel")));
    }

    // Требование «Служебная инструментация не стоит работы, пока не запрошена».
    void frameStatsIsSilentWhenNotRequested()
    {
        QVERIFY2(!m_stats->isLogging(),
                 "тест должен идти без UQ_FRAME_LOG, иначе он проверяет другой режим");

        QSignalSpy spy(m_stats, &FrameStats::statsChanged);
        QVERIFY(spy.isValid());
        QTest::qWait(1000);
        QCOMPARE(spy.count(), 0);
    }
};

QTEST_MAIN(TestIdlePower)
#include "tst_idlepower.moc"
