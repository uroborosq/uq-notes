// Генератор тестовых PDF. Задачи 3.1, 3.6, 8.2, 9.1 требуют документов с
// конкретными свойствами (разные форматы страниц, поворот, многоуровневое
// оглавление, настоящий текстовый слой, 200 страниц), которых нет под рукой;
// здесь они собираются воспроизводимо из Qt и qpdf.

#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QTextStream>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <functional>

namespace {

QTextStream out(stdout);

struct Section
{
    QString title;
    int page = 0;
    QList<Section> children;
};

struct PageSpec
{
    QSizeF sizePt;
    int rotate = 0;
    QString heading;
    QStringList body;
};

void drawPage(QPainter &painter, const PageSpec &spec, int number)
{
    painter.setPen(Qt::black);

    QFont heading(QStringLiteral("DejaVu Serif"), 20);
    painter.setFont(heading);
    painter.drawText(QPointF(56, 90), spec.heading);

    QFont body(QStringLiteral("DejaVu Serif"), 11);
    painter.setFont(body);
    qreal y = 130;
    for (const QString &line : spec.body) {
        painter.drawText(QPointF(56, y), line);
        y += 18;
    }

    QFont footer(QStringLiteral("DejaVu Sans"), 9);
    painter.setFont(footer);
    painter.drawText(QPointF(56, spec.sizePt.height() - 40),
                     QStringLiteral("page %1").arg(number + 1));

    // Рамка, по которой на глаз видно исходные границы страницы.
    painter.setPen(QPen(QColor(180, 180, 180), 0.5));
    painter.drawRect(QRectF(24, 24, spec.sizePt.width() - 48, spec.sizePt.height() - 48));
}

bool writePages(const QString &path, const QList<PageSpec> &pages)
{
    QPdfWriter writer(path);
    writer.setResolution(72);
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));

    QPainter painter;
    for (int i = 0; i < pages.size(); ++i) {
        writer.setPageSize(
            QPageSize(pages[i].sizePt, QPageSize::Point, QString(), QPageSize::ExactMatch));
        if (i == 0) {
            if (!painter.begin(&writer))
                return false;
        } else if (!writer.newPage()) {
            return false;
        }
        drawPage(painter, pages[i], i);
    }
    painter.end();
    return true;
}

// qpdf читает поток входного файла лениво, поэтому писать результат прямо
// поверх входа нельзя: файл обрежется под ногами у читателя.
void writeInPlace(QPDF &pdf, const QString &path)
{
    const QString temporary = path + QStringLiteral(".tmp");
    {
        QPDFWriter writer(pdf, temporary.toLocal8Bit().constData());
        writer.setStaticID(true);
        writer.write();
    }
    QFile::remove(path);
    QFile::rename(temporary, path);
}

QPDFObjectHandle destinationFor(QPDF &pdf, const std::vector<QPDFObjectHandle> &pages, int page)
{
    QPDFObjectHandle array = QPDFObjectHandle::newArray();
    if (page >= 0 && page < int(pages.size()))
        array.appendItem(pages[size_t(page)]);
    else
        array.appendItem(QPDFObjectHandle::newInteger(page)); // намеренно битая ссылка
    array.appendItem(QPDFObjectHandle::newName("/XYZ"));
    array.appendItem(QPDFObjectHandle::newNull());
    array.appendItem(QPDFObjectHandle::newNull());
    array.appendItem(QPDFObjectHandle::newNull());
    return array;
}

// Собирает связный список закладок по правилам PDF: /First, /Last, /Next,
// /Prev, /Parent и /Count с отрицательным значением для свёрнутых узлов.
int buildOutlineLevel(QPDF &pdf, const std::vector<QPDFObjectHandle> &pages,
                      const QList<Section> &sections, QPDFObjectHandle parent)
{
    if (sections.isEmpty())
        return 0;

    std::vector<QPDFObjectHandle> items;
    int visible = 0;

    for (const Section &section : sections) {
        QPDFObjectHandle item = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
        item.replaceKey("/Title",
                        QPDFObjectHandle::newUnicodeString(section.title.toStdString()));
        item.replaceKey("/Parent", parent);
        item.replaceKey("/Dest", destinationFor(pdf, pages, section.page));
        items.push_back(item);
        ++visible;
    }

    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0)
            items[i].replaceKey("/Prev", items[i - 1]);
        if (i + 1 < items.size())
            items[i].replaceKey("/Next", items[i + 1]);
        visible += buildOutlineLevel(pdf, pages, sections[int(i)].children, items[i]);
    }

    parent.replaceKey("/First", items.front());
    parent.replaceKey("/Last", items.back());
    parent.replaceKey("/Count", QPDFObjectHandle::newInteger(visible));
    return visible;
}

void addOutline(const QString &path, const QList<Section> &sections)
{
    QPDF pdf;
    pdf.processFile(path.toLocal8Bit().constData());

    std::vector<QPDFObjectHandle> pages;
    for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages())
        pages.push_back(page.getObjectHandle());

    QPDFObjectHandle outlines = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
    outlines.replaceKey("/Type", QPDFObjectHandle::newName("/Outlines"));
    buildOutlineLevel(pdf, pages, sections, outlines);
    pdf.getRoot().replaceKey("/Outlines", outlines);
    pdf.getRoot().replaceKey("/PageMode", QPDFObjectHandle::newName("/UseOutlines"));

    writeInPlace(pdf, path);
}

void applyRotation(const QString &path, const QList<PageSpec> &pages)
{
    QPDF pdf;
    pdf.processFile(path.toLocal8Bit().constData());
    QPDFPageDocumentHelper helper(pdf);
    int index = 0;
    for (QPDFPageObjectHelper &page : helper.getAllPages()) {
        if (index < pages.size() && pages[index].rotate != 0)
            page.getObjectHandle().replaceKey(
                "/Rotate", QPDFObjectHandle::newInteger(pages[index].rotate));
        ++index;
    }
    writeInPlace(pdf, path);
}

QStringList paragraph(const QString &seed, int lines)
{
    QStringList result;
    for (int i = 0; i < lines; ++i) {
        result << QStringLiteral(
                      "%1 line %2: the quick brown fox jumps over the lazy dog while "
                      "counting %3 pages.")
                      .arg(seed)
                      .arg(i + 1)
                      .arg((i + 1) * 7);
    }
    return result;
}

// textbook.pdf: настоящий текстовый слой, трёхуровневое оглавление и один
// намеренно битый пункт - сценарий «битая ссылка» из outline-navigation.
void makeTextbook(const QString &path)
{
    const QSizeF a4(595.276, 841.89);
    QList<PageSpec> pages;
    const QStringList headings = {
        QStringLiteral("Title page"),        QStringLiteral("Contents"),
        QStringLiteral("1 Introduction"),    QStringLiteral("1.1 Background"),
        QStringLiteral("1.1.1 Deep detail"), QStringLiteral("1.2 Scope"),
        QStringLiteral("2 Method"),          QStringLiteral("2.1 Apparatus"),
        QStringLiteral("2.2 Procedure"),     QStringLiteral("3 Results"),
        QStringLiteral("3.1 Tables"),        QStringLiteral("3.2 Discussion"),
        QStringLiteral("4 Conclusion"),      QStringLiteral("Index")};
    for (int i = 0; i < headings.size(); ++i) {
        QStringList body = paragraph(QStringLiteral("Section"), 24);
        if (i == 2)
            body.prepend(QStringLiteral("The unique search token is QUIRKBLOSSOM here."));
        if (i == 12)
            body.prepend(QStringLiteral("The closing unique token is ZEPHYRQUARTZ here."));
        pages.append({a4, 0, headings[i], body});
    }
    writePages(path, pages);

    const QList<Section> outline = {
        {QStringLiteral("Title page"), 0, {}},
        {QStringLiteral("Contents"), 1, {}},
        {QStringLiteral("1 Introduction"),
         2,
         {{QStringLiteral("1.1 Background"), 3, {{QStringLiteral("1.1.1 Deep detail"), 4, {}}}},
          {QStringLiteral("1.2 Scope"), 5, {}}}},
        {QStringLiteral("2 Method"),
         6,
         {{QStringLiteral("2.1 Apparatus"), 7, {}}, {QStringLiteral("2.2 Procedure"), 8, {}}}},
        {QStringLiteral("3 Results"),
         9,
         {{QStringLiteral("3.1 Tables"), 10, {}}, {QStringLiteral("3.2 Discussion"), 11, {}}}},
        {QStringLiteral("4 Conclusion"), 12, {}},
        {QStringLiteral("Index"), 13, {}},
        {QStringLiteral("Broken link (page 999)"), 999, {}},
    };
    addOutline(path, outline);
}

// mixed-pages.pdf: разные форматы и ненулевой поворот - сценарий «страницы
// разного размера и ориентации» из pdf-document.
void makeMixedPages(const QString &path)
{
    const QList<PageSpec> pages = {
        {QSizeF(595.276, 841.89), 0, QStringLiteral("A4 portrait, rotate 0"),
         paragraph(QStringLiteral("A4"), 6)},
        {QSizeF(841.89, 595.276), 0, QStringLiteral("A4 landscape, rotate 0"),
         paragraph(QStringLiteral("Landscape"), 6)},
        {QSizeF(419.528, 595.276), 90, QStringLiteral("A5 portrait, rotate 90"),
         paragraph(QStringLiteral("A5"), 6)},
        {QSizeF(612.0, 792.0), 180, QStringLiteral("US Letter, rotate 180"),
         paragraph(QStringLiteral("Letter"), 6)},
        {QSizeF(841.89, 1190.55), 270, QStringLiteral("A3 portrait, rotate 270"),
         paragraph(QStringLiteral("A3"), 6)},
    };
    writePages(path, pages);
    applyRotation(path, pages);
}

// no-outline.pdf: документ без закладок - сценарий «документ без закладок».
void makeNoOutline(const QString &path)
{
    QList<PageSpec> pages;
    for (int i = 0; i < 4; ++i) {
        pages.append({QSizeF(595.276, 841.89), 0, QStringLiteral("Plain page %1").arg(i + 1),
                      paragraph(QStringLiteral("Plain"), 10)});
    }
    writePages(path, pages);
}

// large.pdf: 200 страниц для бюджетов памяти и плавности (3.6, 9.1).
void makeLarge(const QString &path)
{
    QList<PageSpec> pages;
    for (int i = 0; i < 200; ++i) {
        pages.append({QSizeF(595.276, 841.89), 0, QStringLiteral("Chapter page %1").arg(i + 1),
                      paragraph(QStringLiteral("Dense"), 34)});
    }
    writePages(path, pages);

    QList<Section> outline;
    for (int chapter = 0; chapter < 20; ++chapter) {
        Section section{QStringLiteral("Chapter %1").arg(chapter + 1), chapter * 10, {}};
        for (int sub = 0; sub < 3; ++sub) {
            section.children.append({QStringLiteral("Chapter %1, section %2")
                                         .arg(chapter + 1)
                                         .arg(sub + 1),
                                     chapter * 10 + sub * 3, {}});
        }
        outline.append(section);
    }
    addOutline(path, outline);
}

// corrupt.pdf и encrypted.pdf: сценарий «повреждённый или зашифрованный файл».
void makeCorrupt(const QString &path)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write("%PDF-1.7\nthis is not a valid pdf body at all\n%%EOF\n");
}

void makeEncrypted(const QString &source, const QString &path)
{
    QPDF pdf;
    pdf.processFile(source.toLocal8Bit().constData());
    QPDFWriter writer(pdf, path.toLocal8Bit().constData());
    writer.setStaticID(true);
    writer.setR6EncryptionParameters("uq-user-secret", "uq-owner-secret", true, true, true, true,
                                     true, true, qpdf_r3p_full, true);
    writer.write();
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");

    const auto path = [&dir](const QString &name) { return dir + QLatin1Char('/') + name; };

    makeTextbook(path(QStringLiteral("textbook.pdf")));
    makeMixedPages(path(QStringLiteral("mixed-pages.pdf")));
    makeNoOutline(path(QStringLiteral("no-outline.pdf")));
    makeLarge(path(QStringLiteral("large.pdf")));
    makeCorrupt(path(QStringLiteral("corrupt.pdf")));
    makeEncrypted(path(QStringLiteral("textbook.pdf")), path(QStringLiteral("encrypted.pdf")));

    out << "fixtures written to " << dir << "\n";
    out.flush();
    return 0;
}
