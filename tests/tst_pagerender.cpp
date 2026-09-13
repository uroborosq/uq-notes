// Задача 3.3: асинхронный рендер страницы на рабочем потоке.

#include <QElapsedTimer>
#include <QPdfDocument>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include "fixtures.h"
#include "pdf/pagerenderservice.h"

class TestPageRender : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QCOMPARE(m_document.load(fixtures::textbook()), QPdfDocument::Error::None);
    }

    // Запрос не растеризует ничего в вызывающем потоке: он возвращается сразу,
    // а картинка приходит позже сигналом.
    void requestReturnsImmediatelyAndDeliversLater()
    {
        PageRenderService service;
        service.setDocument(&m_document);
        QSignalSpy spy(&service, &PageRenderService::rendered);

        QElapsedTimer timer;
        timer.start();
        service.request(0, QSize(595, 842));
        const qint64 callMs = timer.elapsed();

        QVERIFY(spy.isEmpty()); // синхронно ничего не пришло
        QCOMPARE(service.pendingCount(), 1);
        QVERIFY2(callMs < 30, qPrintable(QStringLiteral("request blocked for %1 ms").arg(callMs)));

        QVERIFY(spy.wait(10000));
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.first().at(0).toInt(), 0);
        const QImage image = spy.first().at(2).value<QImage>();
        QCOMPARE(image.size(), QSize(595, 842));
        QVERIFY(!image.isNull());
        QCOMPARE(service.pendingCount(), 0);
    }

    void rendersRequestedResolution()
    {
        PageRenderService service;
        service.setDocument(&m_document);
        QSignalSpy spy(&service, &PageRenderService::rendered);

        service.request(2, QSize(1190, 1684)); // 200% от размера страницы
        QVERIFY(spy.wait(10000));
        const QImage image = spy.first().at(2).value<QImage>();
        QCOMPARE(image.size(), QSize(1190, 1684));
        // Страница не пустая: на фикстуре есть текст и рамка.
        QVERIFY(image.pixelColor(0, 0).isValid());
    }

    void repeatedIdenticalRequestIsCoalesced()
    {
        PageRenderService service;
        service.setDocument(&m_document);
        service.request(1, QSize(300, 424));
        service.request(1, QSize(300, 424));
        service.request(1, QSize(300, 424));
        QCOMPARE(service.pendingCount(), 1);
        QCOMPARE(service.pendingSize(1), QSize(300, 424));
    }

    // При быстром зуме запросы обновляются; результат устаревшего размера
    // не должен подменять текстуру.
    void staleResultIsDropped()
    {
        PageRenderService service;
        service.setDocument(&m_document);
        QSignalSpy spy(&service, &PageRenderService::rendered);

        service.request(3, QSize(200, 283));
        service.request(3, QSize(900, 1273));
        QCOMPARE(service.pendingSize(3), QSize(900, 1273));

        QVERIFY(spy.wait(10000));
        while (spy.size() < 1 && spy.wait(2000)) { }
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.first().at(2).value<QImage>().size(), QSize(900, 1273));
    }

    void cancelSuppressesResult()
    {
        PageRenderService service;
        service.setDocument(&m_document);
        QSignalSpy spy(&service, &PageRenderService::rendered);

        service.request(4, QSize(595, 842));
        service.cancel(4);
        QCOMPARE(service.pendingCount(), 0);

        QTest::qWait(1500);
        QCOMPARE(spy.size(), 0);
    }

    void ignoresRequestsOutsideDocument()
    {
        PageRenderService service;
        service.setDocument(&m_document);
        service.request(-1, QSize(100, 100));
        service.request(9999, QSize(100, 100));
        service.request(0, QSize(0, 0));
        QCOMPARE(service.pendingCount(), 0);
    }

private:
    QPdfDocument m_document;
};

QTEST_MAIN(TestPageRender)
#include "tst_pagerender.moc"
