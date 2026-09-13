#pragma once

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSizeF>

// Раскладка документа: прямоугольники страниц в координатах документа (точки
// PDF). Отдельная от рендера чистая функция - смена режима отображения меняет
// только её, узлы страниц и чернил при этом не пересоздаются (design.md, D5).
//
// Координаты документа: ось Y растёт вниз, начало - левый верхний угол
// ограничивающего прямоугольника всего документа.
class DocumentLayout
{
public:
    enum class Mode {
        Single, // одна страница: страницы в столбик
        Spread, // разворот: обложка одна, дальше пары, как в печатной книге
    };

    void setPageSizes(const QList<QSizeF> &sizes);
    const QList<QSizeF> &pageSizes() const { return m_sizes; }

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    // Зазор между строками раскладки и между страницами внутри разворота.
    void setRowSpacing(qreal spacing);
    void setSpreadGap(qreal gap);
    qreal rowSpacing() const { return m_rowSpacing; }
    qreal spreadGap() const { return m_spreadGap; }

    int pageCount() const { return int(m_sizes.size()); }
    bool isEmpty() const { return m_sizes.isEmpty(); }

    QRectF pageRect(int page) const;
    QSizeF documentSize() const { return m_documentSize; }

    // Страницы, пересекающие прямоугольник в координатах документа.
    QList<int> pagesIntersecting(const QRectF &documentRect) const;
    // Страница под точкой, или -1 если точка попала в промежуток.
    int pageAt(const QPointF &documentPoint) const;
    // Страница, ближайшая по вертикали к точке - для сохранения позиции при
    // смене режима, когда точка может оказаться в промежутке.
    int nearestPage(const QPointF &documentPoint) const;

    // Индекс строки раскладки и состав строки: в одностраничном режиме это
    // одна страница, в развороте - пара (или обложка).
    int rowOf(int page) const;
    QList<int> pagesInRow(int row) const;
    // Страницы строк с firstRow по lastRow включительно. Диапазон зажимается в
    // границы документа: зона удержания отсчитывается строками вокруг видимых и
    // у краёв документа выходит за них.
    QList<int> pagesInRows(int firstRow, int lastRow) const;
    int rowCount() const { return int(m_rows.size()); }

    // Строка, в которую попадает страница при переходе по оглавлению.
    QRectF rowRect(int row) const;

private:
    void rebuild();

    struct Row
    {
        QList<int> pages;
        QRectF rect; // объединяющий прямоугольник строки
    };

    QList<QSizeF> m_sizes;
    QList<QRectF> m_rects;
    QList<Row> m_rows;
    QList<int> m_rowOfPage;
    QSizeF m_documentSize;

    Mode m_mode = Mode::Single;
    qreal m_rowSpacing = 16.0;
    qreal m_spreadGap = 8.0;
};
