#include "noteexporter.h"

#include <QFileInfo>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QDir>
#include <QTemporaryFile>
#include <QtConcurrent>

#include <qpdf/QPDFJob.hh>
#include <qpdf/QPDFUsage.hh>

#include "ink/stroketessellator.h"

namespace {

bool overlayWithQpdf(const QString &original, const QString &overlay, const QString &result,
                     QString *errorMessage)
{
    const QByteArray in = original.toLocal8Bit();
    const QByteArray ink = overlay.toLocal8Bit();
    const QByteArray out = result.toLocal8Bit();
    // --no-warn: замечания о структуре исходника не адресованы пользователю,
    // а настоящая ошибка всё равно придёт исключением.
    char const *argv[] = {"qpdf",     "--no-warn",     in.constData(), "--overlay",
                          ink.constData(), "--",       out.constData(), nullptr};
    try {
        QPDFJob job;
        job.initializeFromArgv(argv);
        job.run();
    } catch (const std::exception &error) {
        if (errorMessage)
            *errorMessage = QString::fromUtf8(error.what());
        return false;
    }
    return true;
}

} // namespace

NoteExporter::NoteExporter(QObject *parent)
    : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<bool>::finished, this, [this] {
        const bool ok = m_watcher.result();
        setBusy(false);
        m_progress = ok ? 1.0 : 0.0;
        m_statusText = ok ? tr("Экспорт завершён") : m_lastError;
        emit progressChanged();
        emit finished(ok, m_statusText);
    });
}

NoteExporter::~NoteExporter()
{
    if (m_watcher.isRunning())
        m_watcher.waitForFinished();
}

void NoteExporter::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit stateChanged();
}

QString NoteExporter::withPdfSuffix(const QString &path)
{
    if (path.isEmpty() || path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
        return path;
    return path + QStringLiteral(".pdf");
}

bool NoteExporter::exportToPdf(PdfDocumentModel *document, InkDocument *ink,
                               const QString &rawOutputPath)
{
    const QString outputPath = withPdfSuffix(rawOutputPath);
    if (m_busy || !document || !document->isReady() || outputPath.isEmpty())
        return false;

    // Всё, что нужно рабочему потоку, снимается здесь: модели остаются на
    // потоке UI и в фоне не трогаются.
    const QString source = document->source();
    QList<QSizeF> pageSizes;
    QList<int> rotations;
    for (int page = 0; page < document->pageCount(); ++page) {
        pageSizes.append(document->pageSize(page));
        rotations.append(document->pageRotation(page));
    }
    const QList<InkDocument::Record> strokes = ink ? ink->allStrokes()
                                                   : QList<InkDocument::Record>();

    setBusy(true);
    m_progress = 0.0;
    m_statusText = tr("Подготовка чернил…");
    m_lastError.clear();
    emit progressChanged();

    const int pageCount = pageSizes.size();
    m_watcher.setFuture(QtConcurrent::run([this, source, pageSizes, rotations, strokes,
                                           outputPath, pageCount] {
        QString error;
        const bool ok = exportNow(source, pageSizes, rotations, strokes, outputPath, &error,
                                  [this, pageCount](int page, int total) {
                                      Q_UNUSED(total)
                                      // Наложение через qpdf занимает примерно
                                      // столько же, сколько отрисовка чернил,
                                      // поэтому страницы дают первую половину.
                                      const qreal value =
                                          pageCount > 0 ? 0.5 * qreal(page + 1) / pageCount : 0.0;
                                      QMetaObject::invokeMethod(
                                          this,
                                          [this, value] {
                                              m_progress = value;
                                              m_statusText = tr("Отрисовка чернил…");
                                              emit progressChanged();
                                          },
                                          Qt::QueuedConnection);
                                  });
        if (!ok)
            m_lastError = error.isEmpty() ? tr("Экспорт не удался") : error;
        return ok;
    }));
    return true;
}

bool NoteExporter::exportNow(const QString &sourcePdf, const QList<QSizeF> &displayedPageSizes,
                             const QList<int> &pageRotations,
                             const QList<InkDocument::Record> &strokes,
                             const QString &outputPath, QString *errorMessage,
                             const std::function<void(int, int)> &onPage)
{
    if (!QFileInfo::exists(sourcePdf)) {
        if (errorMessage)
            *errorMessage = tr("Исходный PDF не найден: %1").arg(sourcePdf);
        return false;
    }
    if (displayedPageSizes.isEmpty()) {
        if (errorMessage)
            *errorMessage = tr("В документе нет страниц.");
        return false;
    }

    // Чернила по страницам: обход всех штрихов на каждую страницу был бы
    // квадратичным на документе в 200 страниц.
    QHash<int, QList<const InkDocument::Record *>> byPage;
    for (const InkDocument::Record &record : strokes) {
        if (record.stroke.page >= 0 && record.stroke.page < displayedPageSizes.size())
            byPage[record.stroke.page].append(&record);
    }

    // pageRotations здесь не используется: qpdf при наложении сам приводит
    // страницу overlay к показываемой ориентации назначения, поэтому чернила
    // рисуются прямо в тех координатах, в которых они и хранятся.
    Q_UNUSED(pageRotations)

    QTemporaryFile overlayFile(QDir::tempPath() + QStringLiteral("/uq-overlay-XXXXXX.pdf"));
    overlayFile.setAutoRemove(true);
    if (!overlayFile.open()) {
        if (errorMessage)
            *errorMessage = tr("Не удалось создать временный файл для чернил.");
        return false;
    }
    const QString overlayPath = overlayFile.fileName();
    overlayFile.close();

    {
        QPdfWriter writer(overlayPath);
        writer.setResolution(72); // 1 единица QPainter = 1 pt PDF
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setTitle(QStringLiteral("uq notes ink overlay"));

        QPainter painter;
        StrokeTessellator tessellator;

        // Overlay обязан иметь ту же нумерацию страниц, что и оригинал:
        // qpdf накладывает страницы по порядку.
        for (int page = 0; page < displayedPageSizes.size(); ++page) {
            const QSizeF size = displayedPageSizes[page];
            writer.setPageSize(
                QPageSize(size, QPageSize::Point, QString(), QPageSize::ExactMatch));

            if (page == 0) {
                if (!painter.begin(&writer)) {
                    if (errorMessage)
                        *errorMessage = tr("Не удалось начать отрисовку чернил.");
                    return false;
                }
            } else if (!writer.newPage()) {
                if (errorMessage)
                    *errorMessage = tr("Не удалось добавить страницу в overlay.");
                return false;
            }

            const auto it = byPage.constFind(page);
            if (it != byPage.constEnd()) {
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setPen(Qt::NoPen);

                for (const InkDocument::Record *record : it.value()) {
                    tessellator.setWidth(record->stroke.width);
                    // Тот же закон толщины, что на экране (ink-beauty-pass, D3).
                    tessellator.setStyle(record->stroke.style);
                    const QPolygonF outline = tessellator.outline(record->stroke.points);
                    if (outline.isEmpty())
                        continue;
                    QPainterPath path;
                    path.addPolygon(outline);
                    path.closeSubpath();
                    // Контур - те же примитивы, что полоса на экране: дуги
                    // торцов, стороны, внешние дуги стыков (design.md
                    // ink-beauty-pass, D7); паритет держат tst_inkparity и
                    // tst_export. Заливка по ненулевому обходу: на петлях и
                    // резких стыках контур сам себя перекрывает, и even-odd
                    // оставил бы дырки.
                    path.setFillRule(Qt::WindingFill);
                    painter.fillPath(path, record->stroke.color);
                }
                painter.restore();
            }

            if (onPage)
                onPage(page, displayedPageSizes.size());
        }
        painter.end();
    }

    return overlayWithQpdf(sourcePdf, overlayPath, outputPath, errorMessage);
}
