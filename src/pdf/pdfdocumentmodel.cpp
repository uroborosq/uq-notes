#include "pdfdocumentmodel.h"

#include <QDir>
#include <QFileInfo>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

#include <exception>

namespace {

PdfDocumentModel::OpenResult classify(QPdfDocument::Error error)
{
    switch (error) {
    case QPdfDocument::Error::None:
        return PdfDocumentModel::OpenResult::Success;
    case QPdfDocument::Error::FileNotFound:
        return PdfDocumentModel::OpenResult::FileNotFound;
    case QPdfDocument::Error::InvalidFileFormat:
        return PdfDocumentModel::OpenResult::InvalidFormat;
    case QPdfDocument::Error::IncorrectPassword:
        return PdfDocumentModel::OpenResult::PasswordRequired;
    case QPdfDocument::Error::UnsupportedSecurityScheme:
        return PdfDocumentModel::OpenResult::UnsupportedEncryption;
    case QPdfDocument::Error::DataNotYetAvailable:
    case QPdfDocument::Error::Unknown:
        break;
    }
    return PdfDocumentModel::OpenResult::Unknown;
}

QString describe(PdfDocumentModel::OpenResult result, const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    switch (result) {
    case PdfDocumentModel::OpenResult::None:
    case PdfDocumentModel::OpenResult::Success:
        return QString();
    case PdfDocumentModel::OpenResult::FileNotFound:
        return PdfDocumentModel::tr("Файл не найден: %1").arg(path);
    case PdfDocumentModel::OpenResult::InvalidFormat:
        return PdfDocumentModel::tr("«%1» не является корректным PDF.").arg(name);
    case PdfDocumentModel::OpenResult::PasswordRequired:
        return PdfDocumentModel::tr("«%1» защищён паролем.").arg(name);
    case PdfDocumentModel::OpenResult::UnsupportedEncryption:
        return PdfDocumentModel::tr("«%1» зашифрован способом, который не поддерживается.")
            .arg(name);
    case PdfDocumentModel::OpenResult::Unknown:
        break;
    }
    return PdfDocumentModel::tr("Не удалось открыть «%1».").arg(name);
}

int normalizeRotation(int degrees)
{
    int value = degrees % 360;
    if (value < 0)
        value += 360;
    return (value / 90) * 90;
}

} // namespace

PdfDocumentModel::PdfDocumentModel(QObject *parent)
    : QObject(parent)
    , m_document(new QPdfDocument(this))
{
}

PdfDocumentModel::~PdfDocumentModel() = default;

int PdfDocumentModel::pageCount() const
{
    return m_state == State::Ready ? m_document->pageCount() : 0;
}

bool PdfDocumentModel::open(const QString &path, const QString &password)
{
    m_attemptedSource = path;

    if (!QFileInfo::exists(path))
        return fail(OpenResult::FileNotFound, path);

    // Пароль ставится до load: QPdfDocument применяет его при открытии.
    auto *candidate = new QPdfDocument(this);
    candidate->setPassword(password);
    const QPdfDocument::Error error = candidate->load(path);

    if (error != QPdfDocument::Error::None) {
        delete candidate;
        return fail(classify(error), path);
    }

    delete m_document;
    m_document = candidate;
    m_source = path;
    m_password = password;
    m_state = State::Ready;
    m_lastOpenResult = OpenResult::Success;
    m_errorString.clear();
    loadRotations(path, password);

    emit documentChanged();
    emit lastOpenResultChanged();
    return true;
}

QString PdfDocumentModel::toLocalPath(const QUrl &url)
{
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}

QUrl PdfDocumentModel::folderUrl() const
{
    if (m_source.isEmpty())
        return QUrl();
    return QUrl::fromLocalFile(QFileInfo(m_source).absolutePath());
}

QUrl PdfDocumentModel::suggestedExportUrl() const
{
    if (m_source.isEmpty())
        return QUrl();
    const QFileInfo info(m_source);
    return QUrl::fromLocalFile(
        info.dir().filePath(info.completeBaseName() + tr(" (с чернилами)") + QStringLiteral(".pdf")));
}

void PdfDocumentModel::close()
{
    delete m_document;
    m_document = new QPdfDocument(this);
    m_source.clear();
    m_password.clear();
    m_rotations.clear();
    m_state = State::Empty;
    emit documentChanged();
}

// Неудачное открытие меняет только результат попытки. Открытый документ,
// если он был, остаётся на месте - приложение обязано остаться работоспособным.
bool PdfDocumentModel::fail(OpenResult result, const QString &path)
{
    m_lastOpenResult = result;
    m_errorString = describe(result, path);
    emit lastOpenResultChanged();
    return false;
}

// QtPdf не отдаёт /Rotate, а он нужен экспорту: overlay рисуется в
// неповёрнутой системе координат страницы оригинала.
void PdfDocumentModel::loadRotations(const QString &path, const QString &password)
{
    m_rotations.clear();
    try {
        QPDF pdf;
        // Нас интересует только /Rotate. Замечания qpdf о структуре файла
        // пользователю не адресованы: документ открылся, страницы читаются.
        pdf.setSuppressWarnings(true);
        if (!password.isEmpty())
            pdf.processFile(path.toLocal8Bit().constData(), password.toLocal8Bit().constData());
        else
            pdf.processFile(path.toLocal8Bit().constData());

        int index = 0;
        for (QPDFPageObjectHelper &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
            // /Rotate наследуется от родительских узлов дерева страниц.
            QPDFObjectHandle rotate = page.getObjectHandle().getKey("/Rotate");
            if (rotate.isInteger())
                m_rotations.insert(index, normalizeRotation(int(rotate.getIntValue())));
            ++index;
        }
    } catch (const std::exception &) {
        // Поворот - уточнение, а не условие работы: без него страницы всё равно
        // показываются в размерах, которые уже отдаёт QtPdf с учётом поворота.
        m_rotations.clear();
    }
}

QSizeF PdfDocumentModel::pageSize(int page) const
{
    if (m_state != State::Ready || page < 0 || page >= m_document->pageCount())
        return QSizeF();
    return m_document->pagePointSize(page);
}

int PdfDocumentModel::pageRotation(int page) const
{
    if (m_state != State::Ready || page < 0 || page >= m_document->pageCount())
        return 0;
    return m_rotations.value(page, 0);
}
