// Задача 4.1 и часть 4.5: раскладка документа в одностраничном режиме и в
// развороте. Сценарии из specs/canvas-view/spec.md.

#include <QTest>

#include "canvas/documentlayout.h"

class TestDocumentLayout : public QObject
{
    Q_OBJECT

private:
    static QList<QSizeF> uniform(int count, const QSizeF &size = QSizeF(595, 842))
    {
        QList<QSizeF> sizes;
        for (int i = 0; i < count; ++i)
            sizes.append(size);
        return sizes;
    }

private slots:
    void singleModeStacksPagesInOrder()
    {
        DocumentLayout layout;
        layout.setRowSpacing(16);
        layout.setPageSizes(uniform(3));

        QCOMPARE(layout.pageCount(), 3);
        QCOMPARE(layout.pageRect(0), QRectF(0, 0, 595, 842));
        QCOMPARE(layout.pageRect(1), QRectF(0, 858, 595, 842));
        QCOMPARE(layout.pageRect(2), QRectF(0, 1716, 595, 842));
        QCOMPARE(layout.documentSize(), QSizeF(595, 842 * 3 + 16 * 2));
    }

    // Страницы разного размера: каждая своего размера, все по общей центральной
    // вертикали, документ по ширине - по самой широкой странице.
    void singleModeCentersPagesOfDifferentWidths()
    {
        DocumentLayout layout;
        layout.setRowSpacing(10);
        layout.setPageSizes({QSizeF(400, 500), QSizeF(800, 300), QSizeF(600, 200)});

        QCOMPARE(layout.documentSize().width(), 800.0);
        QCOMPARE(layout.pageRect(0), QRectF(200, 0, 400, 500));
        QCOMPARE(layout.pageRect(1), QRectF(0, 510, 800, 300));
        QCOMPARE(layout.pageRect(2), QRectF(100, 820, 600, 200));
        QCOMPARE(layout.documentSize().height(), 500.0 + 10 + 300 + 10 + 200);
    }

    // Сценарий «Расположение пар в развороте»: первая страница одиночно как
    // обложка, дальше пары.
    void spreadModePutsCoverAloneThenPairs()
    {
        DocumentLayout layout;
        layout.setMode(DocumentLayout::Mode::Spread);
        layout.setRowSpacing(16);
        layout.setSpreadGap(8);
        layout.setPageSizes(uniform(6));

        // Шесть страниц: обложка, две полные пары и последняя страница одна.
        QCOMPARE(layout.rowCount(), 4);
        QCOMPARE(layout.pagesInRow(0), QList<int>({0}));
        QCOMPARE(layout.pagesInRow(1), QList<int>({1, 2}));
        QCOMPARE(layout.pagesInRow(2), QList<int>({3, 4}));
        QCOMPARE(layout.pagesInRow(3), QList<int>({5}));
        QCOMPARE(layout.rowOf(0), 0);
        QCOMPARE(layout.rowOf(2), 1);
        QCOMPARE(layout.rowOf(5), 3);
    }

    void spreadModeHandlesTrailingSinglePage()
    {
        DocumentLayout layout;
        layout.setMode(DocumentLayout::Mode::Spread);
        layout.setPageSizes(uniform(4));

        QCOMPARE(layout.rowCount(), 3);
        QCOMPARE(layout.pagesInRow(0), QList<int>({0}));
        QCOMPARE(layout.pagesInRow(1), QList<int>({1, 2}));
        QCOMPARE(layout.pagesInRow(2), QList<int>({3}));
    }

    // Сценарий «Разворот из страниц разного формата»: обе целиком, пропорции
    // сохранены, выровнены по вертикали.
    void spreadAlignsDifferentFormatsOnCommonLine()
    {
        DocumentLayout layout;
        layout.setMode(DocumentLayout::Mode::Spread);
        layout.setRowSpacing(20);
        layout.setSpreadGap(10);
        layout.setPageSizes({QSizeF(595, 842), QSizeF(595, 842), QSizeF(400, 600)});

        const QRectF left = layout.pageRect(1);
        const QRectF right = layout.pageRect(2);

        // Пропорции не тронуты.
        QCOMPARE(left.size(), QSizeF(595, 842));
        QCOMPARE(right.size(), QSizeF(400, 600));
        // Обе страницы внутри строки и выровнены по общей средней линии.
        QCOMPARE(left.center().y(), right.center().y());
        QVERIFY(right.left() >= left.right());
        QCOMPARE(right.left() - left.right(), 10.0);
        // Обе видны целиком внутри прямоугольника строки.
        const QRectF row = layout.rowRect(layout.rowOf(1));
        QVERIFY(row.contains(left));
        QVERIFY(row.contains(right));
    }

    void switchingModeKeepsPagesAndChangesRows()
    {
        DocumentLayout layout;
        layout.setPageSizes(uniform(5));
        QCOMPARE(layout.rowCount(), 5);

        layout.setMode(DocumentLayout::Mode::Spread);
        QCOMPARE(layout.rowCount(), 3);
        QCOMPARE(layout.pageCount(), 5);

        layout.setMode(DocumentLayout::Mode::Single);
        QCOMPARE(layout.rowCount(), 5);
        QCOMPARE(layout.pageRect(0), QRectF(0, 0, 595, 842));
    }

    void reportsPagesIntersectingViewport()
    {
        DocumentLayout layout;
        layout.setRowSpacing(16);
        layout.setPageSizes(uniform(200));

        // Окно высотой в одну страницу где-то в середине документа.
        const qreal step = 842 + 16;
        const QRectF viewport(0, step * 50 + 100, 595, 842);
        const QList<int> visible = layout.pagesIntersecting(viewport);
        QCOMPARE(visible, QList<int>({50, 51}));

        QVERIFY(layout.pagesIntersecting(QRectF(0, -5000, 595, 100)).isEmpty());
        QVERIFY(layout.pagesIntersecting(QRectF(0, 1e7, 595, 100)).isEmpty());
    }

    void findsPageUnderPointAndNearestPage()
    {
        DocumentLayout layout;
        layout.setRowSpacing(16);
        layout.setPageSizes(uniform(3));

        QCOMPARE(layout.pageAt(QPointF(10, 10)), 0);
        QCOMPARE(layout.pageAt(QPointF(10, 900)), 1);
        // Точка в промежутке между страницами не принадлежит ни одной.
        QCOMPARE(layout.pageAt(QPointF(10, 850)), -1);
        // Но ближайшая страница у неё есть.
        QCOMPARE(layout.nearestPage(QPointF(10, 850)), 0);
        QCOMPARE(layout.nearestPage(QPointF(10, 856)), 1);
    }

    // Зона удержания отсчитывается строками вокруг видимых: выборка по
    // диапазону строк должна отдавать все страницы этих строк и молча зажимать
    // диапазон у краёв документа.
    void returnsPagesOfRowRange()
    {
        DocumentLayout layout;
        layout.setPageSizes(uniform(5));

        // Одностраничный режим: строка - это страница.
        QCOMPARE(layout.pagesInRows(1, 3), (QList<int>{1, 2, 3}));
        QCOMPARE(layout.pagesInRows(2, 2), (QList<int>{2}));

        // Диапазон за краями документа зажимается, а не отдаёт пустоту.
        QCOMPARE(layout.pagesInRows(-4, 1), (QList<int>{0, 1}));
        QCOMPARE(layout.pagesInRows(3, 99), (QList<int>{3, 4}));
        QCOMPARE(layout.pagesInRows(-10, 10), (QList<int>{0, 1, 2, 3, 4}));

        // Пустой диапазон - когда он целиком за пределами документа.
        QCOMPARE(layout.pagesInRows(7, 9), QList<int>());
        QCOMPARE(layout.pagesInRows(3, 1), QList<int>());

        // В развороте строка - пара страниц, кроме обложки.
        layout.setMode(DocumentLayout::Mode::Spread);
        QCOMPARE(layout.pagesInRows(0, 1), (QList<int>{0, 1, 2}));
        QCOMPARE(layout.pagesInRows(1, 99), (QList<int>{1, 2, 3, 4}));
    }

    void emptyDocumentHasNoLayout()
    {
        DocumentLayout layout;
        QVERIFY(layout.isEmpty());
        QCOMPARE(layout.documentSize(), QSizeF());
        QCOMPARE(layout.pageRect(0), QRectF());
        QCOMPARE(layout.pageAt(QPointF(0, 0)), -1);
        QCOMPARE(layout.nearestPage(QPointF(0, 0)), -1);
        QCOMPARE(layout.pagesInRows(0, 4), QList<int>());
    }
};

QTEST_APPLESS_MAIN(TestDocumentLayout)
#include "tst_documentlayout.moc"
