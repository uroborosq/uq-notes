// Спайк 1.4: чернила -> векторный overlay-PDF через QPdfWriter, наложение на
// оригинал через libqpdf. Проверяем, что оригинальный текстовый слой и закладки
// переживают операцию, а контуры остаются векторными.

#include <QFileInfo>
#include <QGuiApplication>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QPointF>
#include <QTextStream>
#include <QVector>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFJob.hh>
#include <qpdf/QPDFUsage.hh>

#include <cmath>

namespace {

QTextStream out(stdout);

struct InkPoint
{
    QPointF position;
    qreal pressure;
};

// Штрих переменной ширины как замкнутый контур: идём по одной стороне
// центральной линии и возвращаемся по другой. Только так переменная толщина
// от давления переживает экспорт (design.md, D6).
QPainterPath strokeOutline(const QVector<InkPoint> &points, qreal baseWidth)
{
    QPainterPath path;
    if (points.size() < 2)
        return path;

    QVector<QPointF> left;
    QVector<QPointF> right;
    left.reserve(points.size());
    right.reserve(points.size());

    for (int i = 0; i < points.size(); ++i) {
        const QPointF previous = points[qMax(i - 1, 0)].position;
        const QPointF next = points[qMin(i + 1, points.size() - 1)].position;
        QPointF direction = next - previous;
        const qreal length = std::hypot(direction.x(), direction.y());
        if (length < 1e-6)
            direction = QPointF(1.0, 0.0);
        else
            direction /= length;

        const QPointF normal(-direction.y(), direction.x());
        const qreal halfWidth = baseWidth * points[i].pressure * 0.5;
        left.append(points[i].position + normal * halfWidth);
        right.append(points[i].position - normal * halfWidth);
    }

    path.moveTo(left.first());
    for (int i = 1; i < left.size(); ++i)
        path.lineTo(left[i]);
    for (int i = right.size() - 1; i >= 0; --i)
        path.lineTo(right[i]);
    path.closeSubpath();
    return path;
}

QVector<InkPoint> sampleStroke(const QPointF &origin, qreal length, qreal amplitude)
{
    QVector<InkPoint> points;
    const int steps = 120;
    for (int i = 0; i <= steps; ++i) {
        const qreal t = qreal(i) / steps;
        const QPointF position(origin.x() + t * length,
                               origin.y() + std::sin(t * 6.0 * M_PI) * amplitude);
        // Давление нарастает и спадает - на экспорте это должно дать переменную ширину.
        const qreal pressure = 0.15 + 0.85 * std::sin(t * M_PI);
        points.append({position, pressure});
    }
    return points;
}

bool writeOverlay(const QString &path, const QPdfDocument &source, const QList<int> &inkedPages)
{
    QPdfWriter writer(path);
    writer.setResolution(72); // 1 единица QPainter = 1 pt PDF
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));

    QPainter painter;
    for (int page = 0; page < source.pageCount(); ++page) {
        const QSizeF sizePt = source.pagePointSize(page);
        writer.setPageSize(QPageSize(sizePt, QPageSize::Point, QString(), QPageSize::ExactMatch));
        if (page == 0) {
            if (!painter.begin(&writer))
                return false;
        } else if (!writer.newPage()) {
            return false;
        }

        if (!inkedPages.contains(page))
            continue; // пустая страница overlay - оригинал остаётся как есть

        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(20, 30, 200, 220));
        painter.drawPath(strokeOutline(sampleStroke(QPointF(60, 200), 460, 55), 9.0));
        painter.setBrush(QColor(200, 30, 40, 220));
        painter.drawPath(strokeOutline(sampleStroke(QPointF(60, 520), 460, 30), 14.0));
    }
    painter.end();
    return true;
}

bool overlayWithQpdf(const QString &original, const QString &overlay, const QString &result)
{
    const QByteArray a = original.toLocal8Bit();
    const QByteArray b = overlay.toLocal8Bit();
    const QByteArray c = result.toLocal8Bit();
    char const *argv[] = {"qpdf", a.constData(), "--overlay", b.constData(), "--",
                          c.constData(), nullptr};
    try {
        QPDFJob job;
        job.initializeFromArgv(argv);
        job.run();
    } catch (const std::exception &error) {
        out << "qpdf failed: " << error.what() << "\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    if (argc < 3) {
        out << "usage: uq_spike_export_overlay <source.pdf> <result.pdf>\n";
        return 2;
    }

    const QString source = QString::fromLocal8Bit(argv[1]);
    const QString result = QString::fromLocal8Bit(argv[2]);
    const QString overlay = result + QStringLiteral(".overlay.pdf");

    out << "qpdf library version: " << QString::fromStdString(QPDF::QPDFVersion()) << "\n";

    QPdfDocument document;
    if (document.load(source) != QPdfDocument::Error::None) {
        out << "cannot load " << source << "\n";
        return 1;
    }
    out << "source pages: " << document.pageCount() << "\n";

    const QList<int> inked = {0, 8, 100, document.pageCount() - 1};
    if (!writeOverlay(overlay, document, inked)) {
        out << "overlay generation failed\n";
        return 1;
    }
    out << "overlay written: " << overlay << " (" << QFileInfo(overlay).size() << " bytes)\n";

    if (!overlayWithQpdf(source, overlay, result))
        return 1;

    out << "result written: " << result << " (" << QFileInfo(result).size() << " bytes)\n";
    out << "inked pages (0-based): ";
    for (int page : inked)
        out << page << " ";
    out << "\n";
    out.flush();
    return 0;
}
