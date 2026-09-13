// Спайк 1.2/1.3: открыть реальный учебник через QPdfDocument, выгрузить дерево
// QPdfBookmarkModel в консоль и заодно подтвердить, что QtPdf из системных
// пакетов Arch линкуется и работает.

#include <QCoreApplication>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QTextStream>

namespace {

QTextStream out(stdout);

QString locationText(const QVariant &location)
{
    if (!location.isValid())
        return QStringLiteral("-");
    const QPointF point = location.toPointF();
    return QStringLiteral("(%1, %2)").arg(point.x(), 0, 'f', 1).arg(point.y(), 0, 'f', 1);
}

int dumpBookmarks(const QPdfBookmarkModel &model, const QModelIndex &parent, int depth, int pageCount)
{
    int count = 0;
    const int rows = model.rowCount(parent);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        const QString title = index.data(int(QPdfBookmarkModel::Role::Title)).toString();
        const int page = index.data(int(QPdfBookmarkModel::Role::Page)).toInt();
        const int level = index.data(int(QPdfBookmarkModel::Role::Level)).toInt();
        const QVariant location = index.data(int(QPdfBookmarkModel::Role::Location));

        const bool pageValid = page >= 0 && page < pageCount;
        out << QString(depth * 2, QLatin1Char(' ')) << "- " << title << "  [page " << page
            << (pageValid ? "" : " !OUT OF RANGE") << ", level " << level << ", at "
            << locationText(location) << "]\n";

        ++count;
        count += dumpBookmarks(model, index, depth + 1, pageCount);
    }
    return count;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        out << "usage: uq_spike_pdf_outline <file.pdf>\n";
        return 2;
    }

    const QString path = QString::fromLocal8Bit(argv[1]);
    out << "QtPdf from: " << QLibraryInfo::path(QLibraryInfo::LibrariesPath) << "\n";
    out << "Qt version: " << QLibraryInfo::version().toString() << "\n";
    out << "file: " << path << " (" << QFileInfo(path).size() << " bytes)\n\n";

    QPdfDocument document;
    const QPdfDocument::Error error = document.load(path);
    if (error != QPdfDocument::Error::None) {
        out << "load failed, error code " << int(error) << "\n";
        return 1;
    }

    const int pageCount = document.pageCount();
    out << "pages: " << pageCount << "\n";
    for (int page = 0; page < qMin(pageCount, 5); ++page) {
        const QSizeF size = document.pagePointSize(page);
        out << "  page " << page << ": " << size.width() << " x " << size.height() << " pt\n";
    }
    out << "\noutline:\n";

    QPdfBookmarkModel bookmarks;
    bookmarks.setDocument(&document);
    const int total = dumpBookmarks(bookmarks, QModelIndex(), 0, pageCount);

    out << "\ntotal outline entries: " << total << "\n";
    out << "top-level entries: " << bookmarks.rowCount(QModelIndex()) << "\n";
    out << (total > 0 ? "VERDICT: outline available via QPdfBookmarkModel\n"
                      : "VERDICT: no outline in this document\n");
    out.flush();
    return 0;
}
