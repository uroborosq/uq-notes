#include "documentlayout.h"

#include <QtGlobal>

#include <algorithm>
#include <limits>

void DocumentLayout::setPageSizes(const QList<QSizeF> &sizes)
{
    m_sizes = sizes;
    rebuild();
}

void DocumentLayout::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    rebuild();
}

void DocumentLayout::setRowSpacing(qreal spacing)
{
    if (qFuzzyCompare(m_rowSpacing, spacing))
        return;
    m_rowSpacing = spacing;
    rebuild();
}

void DocumentLayout::setSpreadGap(qreal gap)
{
    if (qFuzzyCompare(m_spreadGap, gap))
        return;
    m_spreadGap = gap;
    rebuild();
}

void DocumentLayout::rebuild()
{
    m_rects.clear();
    m_rows.clear();
    m_rowOfPage.assign(m_sizes.size(), -1);
    m_documentSize = QSizeF();

    if (m_sizes.isEmpty())
        return;

    // Состав строк. В развороте первая страница идёт одна как обложка, дальше
    // пары чётная-нечётная - как в печатной книге.
    QList<QList<int>> rows;
    if (m_mode == Mode::Single) {
        for (int page = 0; page < m_sizes.size(); ++page)
            rows.append(QList<int>{page});
    } else {
        rows.append(QList<int>{0});
        for (int page = 1; page < m_sizes.size(); page += 2) {
            QList<int> row{page};
            if (page + 1 < m_sizes.size())
                row.append(page + 1);
            rows.append(row);
        }
    }

    // Ширина строки и общая ширина документа.
    QList<qreal> rowWidths;
    qreal documentWidth = 0.0;
    for (const QList<int> &row : rows) {
        qreal width = 0.0;
        for (int i = 0; i < row.size(); ++i) {
            width += m_sizes[row[i]].width();
            if (i > 0)
                width += m_spreadGap;
        }
        rowWidths.append(width);
        documentWidth = qMax(documentWidth, width);
    }

    m_rects.resize(m_sizes.size());

    qreal y = 0.0;
    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const QList<int> &row = rows[rowIndex];

        qreal rowHeight = 0.0;
        for (int page : row)
            rowHeight = qMax(rowHeight, m_sizes[page].height());

        // Строка центрируется по ширине документа, а страницы разного формата
        // внутри строки выравниваются по общей средней линии: так обе страницы
        // разворота видны целиком и пропорции не трогаются.
        qreal x = (documentWidth - rowWidths[rowIndex]) / 2.0;
        for (int i = 0; i < row.size(); ++i) {
            const int page = row[i];
            const QSizeF size = m_sizes[page];
            const qreal top = y + (rowHeight - size.height()) / 2.0;
            m_rects[page] = QRectF(QPointF(x, top), size);
            m_rowOfPage[page] = rowIndex;
            x += size.width() + m_spreadGap;
        }

        Row entry;
        entry.pages = row;
        entry.rect = QRectF(0.0, y, documentWidth, rowHeight);
        m_rows.append(entry);

        y += rowHeight + m_rowSpacing;
    }

    m_documentSize = QSizeF(documentWidth, y - m_rowSpacing);
}

QRectF DocumentLayout::pageRect(int page) const
{
    if (page < 0 || page >= m_rects.size())
        return QRectF();
    return m_rects[page];
}

QList<int> DocumentLayout::pagesIntersecting(const QRectF &documentRect) const
{
    QList<int> result;
    // Строки идут сверху вниз и не пересекаются по вертикали, так что первую
    // подходящую можно найти двоичным поиском, а дальше идти подряд.
    const auto below = std::lower_bound(
        m_rows.cbegin(), m_rows.cend(), documentRect.top(),
        [](const Row &row, qreal top) { return row.rect.bottom() < top; });

    for (auto it = below; it != m_rows.cend(); ++it) {
        if (it->rect.top() > documentRect.bottom())
            break;
        for (int page : it->pages) {
            if (m_rects[page].intersects(documentRect))
                result.append(page);
        }
    }
    return result;
}

int DocumentLayout::pageAt(const QPointF &documentPoint) const
{
    for (int page : pagesIntersecting(QRectF(documentPoint, QSizeF(0.01, 0.01)))) {
        if (m_rects[page].contains(documentPoint))
            return page;
    }
    return -1;
}

int DocumentLayout::nearestPage(const QPointF &documentPoint) const
{
    if (m_rects.isEmpty())
        return -1;

    int best = 0;
    qreal bestDistance = std::numeric_limits<qreal>::max();
    for (int page = 0; page < m_rects.size(); ++page) {
        const QRectF rect = m_rects[page];
        const qreal dx = qMax(qMax(rect.left() - documentPoint.x(), 0.0),
                              documentPoint.x() - rect.right());
        const qreal dy = qMax(qMax(rect.top() - documentPoint.y(), 0.0),
                              documentPoint.y() - rect.bottom());
        const qreal distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = page;
        }
    }
    return best;
}

int DocumentLayout::rowOf(int page) const
{
    if (page < 0 || page >= m_rowOfPage.size())
        return -1;
    return m_rowOfPage[page];
}

QList<int> DocumentLayout::pagesInRow(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    return m_rows[row].pages;
}

QList<int> DocumentLayout::pagesInRows(int firstRow, int lastRow) const
{
    const int first = qMax(0, firstRow);
    const int last = qMin(int(m_rows.size()) - 1, lastRow);

    QList<int> result;
    for (int row = first; row <= last; ++row)
        result.append(m_rows[row].pages);
    return result;
}

QRectF DocumentLayout::rowRect(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return QRectF();
    return m_rows[row].rect;
}
