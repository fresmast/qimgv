#include "documentinfo.h"
#include "utils/encryptedimage.h"
#include "utils/virtualfs.h"

#include <QBuffer>

DocumentInfo::DocumentInfo(QString path)
    : mDocumentType(DocumentType::NONE),
      mOrientation(0),
      mFormat(""),
      exifLoaded(false)
{
    mPath = path;
    VirtualFs::Entry entry;
    if(!VirtualFs::stat(path, entry) || entry.isDir) {
        qDebug() << "FileInfo: cannot open: " << path;
        return;
    }
    detectFormat();
}

DocumentInfo::~DocumentInfo() {
}

// ##############################################################
// ####################### PUBLIC METHODS #######################
// ##############################################################

QString DocumentInfo::directoryPath() const {
    if(VirtualFs::isVirtual(mPath))
        return VirtualFs::parentPath(mPath);
    return QFileInfo(mPath).absolutePath();
}

QString DocumentInfo::filePath() const {
    if(VirtualFs::isVirtual(mPath))
        return mPath;
    return QFileInfo(mPath).absoluteFilePath();
}

QString DocumentInfo::fileName() const {
    return VirtualFs::fileName(mPath);
}

QString DocumentInfo::baseName() const {
    const QString name = fileName();
    const int dot = name.lastIndexOf('.');
    return dot > 0 ? name.left(dot) : name;
}

// bytes
qint64 DocumentInfo::fileSize() const {
    VirtualFs::Entry entry;
    if(VirtualFs::stat(mPath, entry))
        return entry.size;
    return 0;
}

DocumentType DocumentInfo::type() const {
    return mDocumentType;
}

QMimeType DocumentInfo::mimeType() const {
    return mMimeType;
}

QString DocumentInfo::format() const {
    return mFormat;
}

QDateTime DocumentInfo::lastModified() const {
    return VirtualFs::lastModified(mPath);
}

// For cases like orientation / even mimetype change we just reload
// Image from scratch, so don`t bother handling it here
void DocumentInfo::refresh() {
    // archive entries are re-read from the archive; nothing to refresh
}

int DocumentInfo::exifOrientation() const {
    return mOrientation;
}

// ##############################################################
// ####################### PRIVATE METHODS ######################
// ##############################################################
void DocumentInfo::detectFormat() {
    if(mDocumentType != DocumentType::NONE)
        return;
    const QString suffixString = VirtualFs::suffix(mPath);
    if(EncryptedImage::isSupportedFormat(suffixString)) {
        mFormat = suffixString;
        mDocumentType = DocumentType::STATIC;
        return;
    }
    QMimeDatabase mimeDb;
    if(VirtualFs::isVirtual(mPath))
        mMimeType = mimeDb.mimeTypeForData(VirtualFs::readHead(mPath, 16384));
    else
        mMimeType = mimeDb.mimeTypeForFile(mPath, QMimeDatabase::MatchContent);
    auto mimeName = mMimeType.name().toUtf8();
    auto suffix = suffixString.toUtf8();
    if(mimeName == "image/jpeg") {
        mFormat = "jpg";
        mDocumentType = DocumentType::STATIC;
    } else if(mimeName == "image/png") {
        if(QImageReader::supportedImageFormats().contains("apng") && detectAPNG()) {
            mFormat = "apng";
            mDocumentType = DocumentType::ANIMATED;
        } else {
            mFormat = "png";
            mDocumentType = DocumentType::STATIC;
        }
    } else if(mimeName == "image/gif") {
        mFormat = "gif";
        mDocumentType = DocumentType::ANIMATED;
    } else if(mimeName == "image/webp" || (mimeName == "audio/x-riff" && suffix == "webp")) {
        mFormat = "webp";
        mDocumentType = detectAnimatedWebP() ? DocumentType::ANIMATED : DocumentType::STATIC;
    } else if(mimeName == "image/jxl") {
        mFormat = "jxl";
        mDocumentType = detectAnimatedJxl() ? DocumentType::ANIMATED : DocumentType::STATIC;
        if(mDocumentType == DocumentType::ANIMATED && !settings->jxlAnimation()) {
            mDocumentType = DocumentType::NONE;
            qDebug() << "animated jxl is off; skipping file";
        }
    } else if(mimeName == "image/avif") {
        mFormat = "avif";
        mDocumentType = detectAnimatedAvif() ? DocumentType::ANIMATED : DocumentType::STATIC;
    } else if(mimeName == "image/bmp") {
        mFormat = "bmp";
        mDocumentType = DocumentType::STATIC;
    } else if(settings->videoPlayback() && settings->videoFormats().contains(mimeName)) {
        mDocumentType = DocumentType::VIDEO;
        mFormat = settings->videoFormats().value(mimeName);
    } else {
        // Extensions can lie: some games ship their encrypted images with a
        // ".png" extension. Sniff the first bytes before giving up on the file.
        const QString containerFormat =
            EncryptedImage::detectFormat(VirtualFs::readHead(mPath, 64));
        if(!containerFormat.isEmpty()) {
            mFormat = containerFormat;
            mDocumentType = DocumentType::STATIC;
            return;
        }
        // just try to open via suffix if all of the above fails
        mFormat = suffix;
        if(mFormat.compare("jfif", Qt::CaseInsensitive) == 0)
            mFormat = "jpg";
        if(settings->videoPlayback() && settings->videoFormats().values().contains(suffix))
            mDocumentType = DocumentType::VIDEO;
        else
            mDocumentType = DocumentType::STATIC;
    }
    loadExifOrientation();
}

inline
// dumb apng detector
bool DocumentInfo::detectAPNG() {
    return VirtualFs::readHead(mPath, 120).contains("acTL");
}

bool DocumentInfo::detectAnimatedWebP() {
    const QByteArray head = VirtualFs::readHead(mPath, 64);
    if(head.size() < 21)
        return false;
    const char *buf = head.constData();
    if(std::memcmp(buf + 12, "VP8X", 4) != 0)
        return false;
    return (buf[20] & (1 << 1)) != 0;
}

// TODO avoid creating multiple QImageReader instances
bool DocumentInfo::detectAnimatedJxl() {
    if(VirtualFs::isVirtual(mPath)) {
        const QByteArray data = VirtualFs::readAll(mPath);
        if(data.isEmpty())
            return false;
        QBuffer buffer;
        buffer.setData(data);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "jxl");
        return reader.supportsAnimation();
    }
    QImageReader r(mPath, "jxl");
    return r.supportsAnimation();
}

bool DocumentInfo::detectAnimatedAvif() {
    const QByteArray head = VirtualFs::readHead(mPath, 32);
    if(head.size() < 12)
        return false;
    return std::memcmp(head.constData() + 4, "ftypavis", 8) == 0;
}

void DocumentInfo::loadExifTags() {
    if(exifLoaded)
        return;
    exifLoaded = true;
    exifTags.clear();
    if(EncryptedImage::isSupportedFormat(mFormat))
        return;
    // exiv2 needs a real file; archive entries are skipped
    if(VirtualFs::isVirtual(mPath))
        return;
#ifdef USE_EXIV2
    try {
        std::unique_ptr<Exiv2::Image> image;

#if EXIV2_TEST_VERSION(0, 28, 0)
        // Exiv2 0.28 removed the wide-string overload on Windows.
        image = Exiv2::ImageFactory::open(mPath.toUtf8().toStdString());
#else
        image = Exiv2::ImageFactory::open(toStdString(mPath));
#endif

        assert(image.get() != 0);
        image->readMetadata();
        Exiv2::ExifData &exifData = image->exifData();
        if(exifData.empty())
            return;

        Exiv2::ExifKey make("Exif.Image.Make");
        Exiv2::ExifKey model("Exif.Image.Model");
        Exiv2::ExifKey dateTime("Exif.Image.DateTime");
        Exiv2::ExifKey exposureTime("Exif.Photo.ExposureTime");
        Exiv2::ExifKey fnumber("Exif.Photo.FNumber");
        Exiv2::ExifKey isoSpeedRatings("Exif.Photo.ISOSpeedRatings");
        Exiv2::ExifKey flash("Exif.Photo.Flash");
        Exiv2::ExifKey focalLength("Exif.Photo.FocalLength");
        Exiv2::ExifKey userComment("Exif.Photo.UserComment");

        Exiv2::ExifData::const_iterator it;

        it = exifData.findKey(make);
        if(it != exifData.end() /* && it->count() */)
            exifTags.insert(QObject::tr("Make"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(model);
        if(it != exifData.end())
            exifTags.insert(QObject::tr("Model"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(dateTime);
        if(it != exifData.end())
            exifTags.insert(QObject::tr("Date/Time"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(exposureTime);
        if(it != exifData.end()) {
            Exiv2::Rational r = it->toRational();
            if(r.first < r.second) {
                qreal exp = round(static_cast<qreal>(r.second) / r.first);
                exifTags.insert(QObject::tr("ExposureTime"), "1/" + QString::number(exp) + QObject::tr(" sec"));
            } else {
                qreal exp = round(static_cast<qreal>(r.first) / r.second);
                exifTags.insert(QObject::tr("ExposureTime"), QString::number(exp) + QObject::tr(" sec"));
            }
        }

        it = exifData.findKey(fnumber);
        if(it != exifData.end()) {
            Exiv2::Rational r = it->toRational();
            qreal fn = static_cast<qreal>(r.first) / r.second;
            exifTags.insert(QObject::tr("F Number"), "f/" + QString::number(fn, 'g', 3));
        }

        it = exifData.findKey(isoSpeedRatings);
        if(it != exifData.end())
            exifTags.insert(QObject::tr("ISO Speed ratings"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(flash);
        if(it != exifData.end())
            exifTags.insert(QObject::tr("Flash"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(focalLength);
        if(it != exifData.end()) {
            Exiv2::Rational r = it->toRational();
            qreal fn = static_cast<qreal>(r.first) / r.second;
            exifTags.insert(QObject::tr("Focal Length"), QString::number(fn, 'g', 3) + QObject::tr(" mm"));
        }

        it = exifData.findKey(userComment);
        if(it != exifData.end()) {
            // crop out 'charset=ascii' etc"
            auto comment = QString::fromStdString(it->value().toString());
            if(comment.startsWith("charset="))
                comment.remove(0, comment.indexOf(" ") + 1);
            exifTags.insert(QObject::tr("UserComment"), comment);
        }
    }

// this should work with both 0.28 and <0.28
#if not EXIV2_TEST_VERSION(0, 28, 0)
#ifdef __WIN32
    catch (Exiv2::BasicError<wchar_t>& e) {
        qDebug() << "Caught Exiv2::BasicError exception:\n" << e.what() << "\n";
        return;
    }
#else
    catch (Exiv2::BasicError<char>& e) {
        qDebug() << "Caught Exiv2::BasicError exception:\n" << e.what() << "\n";
        return;
    }
#endif
#endif

    catch (Exiv2::Error& e) {
        qDebug() << "Caught Exiv2 exception:\n" << e.what() << "\n";
        return;
    }
#endif
}

QMap<QString, QString> DocumentInfo::getExifTags() {
    if(!exifLoaded)
        loadExifTags();
    return exifTags;
}

void DocumentInfo::loadExifOrientation() {
    if(mDocumentType == DocumentType::VIDEO || mDocumentType == DocumentType::NONE ||
       EncryptedImage::isSupportedFormat(mFormat))
        return;

    const std::string format = mFormat.toStdString();
    QByteArray data;
    QBuffer buffer;
    QImageReader *reader = nullptr;
    if(VirtualFs::isVirtual(mPath)) {
        data = VirtualFs::readAll(mPath);
        if(data.isEmpty())
            return;
        buffer.setData(data);
        buffer.open(QIODevice::ReadOnly);
        if(format.empty())
            reader = new QImageReader(&buffer);
        else
            reader = new QImageReader(&buffer, format.c_str());
    } else if(format.empty()) {
        reader = new QImageReader(mPath);
    } else {
        reader = new QImageReader(mPath, format.c_str());
    }

    if(reader->canRead())
        mOrientation = static_cast<int>(reader->transformation());
    delete reader;
}
