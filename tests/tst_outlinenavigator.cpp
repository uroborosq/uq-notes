// Задачи 6.1-6.4: дерево оглавления из закладок PDF, переход по пункту,
// отражение текущей позиции и состояние «оглавление отсутствует».
// Сценарии из specs/outline-navigation/spec.md.

#include <QPdfBookmarkModel>
#include <QSignalSpy>
#include <QTest>

#include "fixtures.h"
#include "pdf/outlinenavigator.h"
#include "pdf/pdfdocumentmodel.h"

class TestOutlineNavigator : public QObject
{
    Q_OBJECT

private:
    static int countRecursive(QAbstractItemModel *model, const QModelIndex &parent = {})
    {
        int total = 0;
        for (int row = 0; row < model->rowCount(parent); ++row) {
            ++total;
            total += countRecursive(model, model->index(row, 0, parent));
        }
        return total;
    }

private slots:
    // Сценарий «Документ с многоуровневым оглавлением»: то же дерево, та же
    // вложенность, те же заголовки и порядок.
    void buildsTreeMatchingDocumentOutline()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::textbook()));

        OutlineNavigator outline;
        outline.setDocument(&document);

        QVERIFY(outline.hasOutline());
        QAbstractItemModel *model = outline.model();
        QCOMPARE(model->rowCount(QModelIndex()), 8);
        QCOMPARE(countRecursive(model), 15);
        QCOMPARE(outline.entryCount(), 15);

        // Порядок верхнего уровня.
        const QStringList expectedTop = {"Title page", "Contents",   "1 Introduction",
                                         "2 Method",   "3 Results",  "4 Conclusion",
                                         "Index",      "Broken link (page 999)"};
        for (int row = 0; row < expectedTop.size(); ++row)
            QCOMPARE(outline.titleOf(model->index(row, 0, QModelIndex())), expectedTop[row]);

        // Три уровня вложенности сохранены.
        const QModelIndex introduction = model->index(2, 0, QModelIndex());
        QCOMPARE(model->rowCount(introduction), 2);
        const QModelIndex background = model->index(0, 0, introduction);
        QCOMPARE(outline.titleOf(background), QStringLiteral("1.1 Background"));
        QCOMPARE(model->rowCount(background), 1);
        QCOMPARE(outline.titleOf(model->index(0, 0, background)),
                 QStringLiteral("1.1.1 Deep detail"));
    }

    // Сценарий «Клик по пункту»: пункт указывает на свою страницу.
    void reportsTargetPageForEachEntry()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::textbook()));
        OutlineNavigator outline;
        outline.setDocument(&document);

        QAbstractItemModel *model = outline.model();
        QCOMPARE(outline.pageOf(model->index(2, 0, QModelIndex())), 2); // 1 Introduction
        const QModelIndex introduction = model->index(2, 0, QModelIndex());
        QCOMPARE(outline.pageOf(model->index(0, 0, introduction)), 3); // 1.1 Background
        QCOMPARE(outline.pageOf(model->index(5, 0, QModelIndex())), 12); // 4 Conclusion

        QVERIFY(outline.isReachable(model->index(2, 0, QModelIndex())));
    }

    // Сценарий «Битая ссылка»: пункт указывает на несуществующую страницу.
    void detectsBrokenLink()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::textbook()));
        OutlineNavigator outline;
        outline.setDocument(&document);

        const QModelIndex broken = outline.model()->index(7, 0, QModelIndex());
        QCOMPARE(outline.titleOf(broken), QStringLiteral("Broken link (page 999)"));
        QCOMPARE(outline.pageOf(broken), 999);
        QVERIFY(!outline.isReachable(broken));
        QVERIFY(999 >= document.pageCount());
    }

    // Сценарий «Прокрутка документа»: подсвечивается пункт текущего раздела.
    void tracksCurrentSectionWhileScrolling()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::textbook()));
        OutlineNavigator outline;
        outline.setDocument(&document);

        outline.setCurrentPage(0);
        QCOMPARE(outline.titleOf(outline.currentIndex()), QStringLiteral("Title page"));

        outline.setCurrentPage(2);
        QCOMPARE(outline.titleOf(outline.currentIndex()), QStringLiteral("1 Introduction"));

        // Внутри подраздела подсвечивается сам подраздел, а не его родитель.
        outline.setCurrentPage(4);
        QCOMPARE(outline.titleOf(outline.currentIndex()), QStringLiteral("1.1.1 Deep detail"));

        outline.setCurrentPage(10);
        QCOMPARE(outline.titleOf(outline.currentIndex()), QStringLiteral("3.1 Tables"));

        // Родительские узлы доступны для раскрытия по цепочке parent().
        QModelIndex parent = outline.currentIndex().parent();
        QCOMPARE(outline.titleOf(parent), QStringLiteral("3 Results"));
        QVERIFY(!parent.parent().isValid());
    }

    void currentSectionIgnoresBrokenEntries()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::textbook()));
        OutlineNavigator outline;
        outline.setDocument(&document);

        // Битый пункт с номером 999 не должен становиться текущим ни на какой
        // реальной странице.
        for (int page = 0; page < document.pageCount(); ++page) {
            outline.setCurrentPage(page);
            QVERIFY(outline.titleOf(outline.currentIndex())
                    != QStringLiteral("Broken link (page 999)"));
        }
    }

    // Сценарий «Документ без закладок».
    void reportsMissingOutline()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::noOutline()));
        OutlineNavigator outline;
        outline.setDocument(&document);

        QVERIFY(!outline.hasOutline());
        QCOMPARE(outline.entryCount(), 0);
        QCOMPARE(outline.model()->rowCount(QModelIndex()), 0);
        QVERIFY(!outline.currentIndex().isValid());

        // И документ при этом полностью работоспособен.
        QCOMPARE(document.pageCount(), 4);
        QVERIFY(!document.pageSize(0).isEmpty());
    }

    void followsDocumentChanges()
    {
        PdfDocumentModel document;
        OutlineNavigator outline;
        outline.setDocument(&document);
        QVERIFY(!outline.hasOutline());

        QVERIFY(document.open(fixtures::textbook()));
        QVERIFY(outline.hasOutline());
        QCOMPARE(outline.entryCount(), 15);

        QVERIFY(document.open(fixtures::noOutline()));
        QVERIFY(!outline.hasOutline());
    }

    void handlesLargeOutline()
    {
        PdfDocumentModel document;
        QVERIFY(document.open(fixtures::large()));
        OutlineNavigator outline;
        outline.setDocument(&document);

        QCOMPARE(outline.model()->rowCount(QModelIndex()), 20);
        QCOMPARE(outline.entryCount(), 80);

        // Глава 11 начинается на странице 100, её разделы - на 100, 103 и 106.
        outline.setCurrentPage(105);
        QCOMPARE(outline.titleOf(outline.currentIndex()),
                 QStringLiteral("Chapter 11, section 2"));
        outline.setCurrentPage(106);
        QCOMPARE(outline.titleOf(outline.currentIndex()),
                 QStringLiteral("Chapter 11, section 3"));
        outline.setCurrentPage(100);
        QCOMPARE(outline.titleOf(outline.currentIndex()),
                 QStringLiteral("Chapter 11, section 1"));
    }
};

QTEST_GUILESS_MAIN(TestOutlineNavigator)
#include "tst_outlinenavigator.moc"
