#include "imagestatic.h"
#include "utils/encryptedimage.h"
#include "utils/virtualfs.h"

#include <QBuffer>

#include <time.h>

ImageStatic::ImageStatic(QString _path)
    : Image(_path)
{
    load();
}

ImageStatic::ImageStatic(std::unique_ptr<DocumentInfo> _info)
    : Image(std::move(_info))
{
    load();
}

ImageStatic::~ImageStatic() {
}

//load image data from disk
void ImageStatic::load() {
    if(isLoaded()) {
        return;
    }
    if(mDocInfo->mimeType().name() == "image/vnd.microsoft.icon")
        loadICO();
    else if(EncryptedImage::isSupportedFormat(mDocInfo->format()))
        loadEncrypted();
    else
        loadGeneric();
}

void ImageStatic::loadEncrypted() {
    QImage decoded;
    if(VirtualFs::isVirtual(mPath))
        decoded = EncryptedImage::decodeData(VirtualFs::readAll(mPath), mDocInfo->format(), mPath);
    else
        decoded = EncryptedImage::decode(mPath, mDocInfo->format());
    std::unique_ptr<const QImage> img(new QImage(decoded));
    image = std::move(img);
    mLoaded = true;
}


void ImageStatic::loadGeneric() {
    /* QImageReader::read() seems more reliable than just reading via QImage.
     * For example: "Invalid JPEG file structure: two SOF markers"
     * QImageReader::read() returns false, but still reads an image. Meanwhile QImage just fails.
     * I havent checked qimage's code, but it seems like it sees an exception
     * from libjpeg or whatever and just gives up on reading the file.
     *
     * tldr: qimage bad
     */
    QImage *tmp = new QImage();
    const bool virtualPath = VirtualFs::isVirtual(mPath);
    QByteArray fileData;
    QBuffer buffer;
    if(virtualPath) {
        fileData = VirtualFs::readAll(mPath);
        buffer.setData(fileData);
        buffer.open(QIODevice::ReadOnly);
    }
    {
        QImageReader r;
        if(virtualPath)
            r.setDevice(&buffer);
        else
            r.setFileName(mPath);
        if(!mDocInfo->format().isEmpty())
            r.setFormat(mDocInfo->format().toStdString().c_str());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        r.setAllocationLimit(settings->memoryAllocationLimit());
#endif
        r.read(tmp);
    }
    // The extension can lie: some games ship encrypted ".png_" or ".gax"
    // images named ".png". Qt cannot decode those, so give the container
    // decoder a chance before reporting a broken file.
    if(tmp->isNull()) {
        const QString containerFormat =
            EncryptedImage::detectFormat(VirtualFs::readHead(mPath, 64));
        if(!containerFormat.isEmpty()) {
            delete tmp;
            if(virtualPath)
                tmp = new QImage(EncryptedImage::decodeData(fileData, containerFormat, mPath));
            else
                tmp = new QImage(EncryptedImage::decode(mPath, containerFormat));
        }
    }
    std::unique_ptr<const QImage> img(tmp);
    img = ImageLib::exifRotated(std::move(img), mDocInfo.get()->exifOrientation());
    // scaling this format via qt results in transparent background
    // it rare enough so lets just convert it to the closest working thing
    if(img->format() == QImage::Format_Mono) {
        QImage *imgConverted = new QImage();
        *imgConverted = img->convertToFormat(QImage::Format_Grayscale8);
        image.reset(imgConverted);
    } else {
        // set image
        image = std::move(img);
    }
    mLoaded = true;
}

// TODO: move this out somewhere to use in other places
void ImageStatic::loadICO() {
    if(VirtualFs::isVirtual(mPath)) {
        const QByteArray data = VirtualFs::readAll(mPath);
        QBuffer buffer;
        buffer.setData(data);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "ico");
        // pick the largest icon, like QIcon does
        int bestIndex = 0;
        QSize bestSize;
        const int imageCount = reader.imageCount();
        for(int i = 0; i < imageCount; ++i) {
            reader.jumpToImage(i);
            const QSize size = reader.size();
            if(size.width() > bestSize.width()) {
                bestSize = size;
                bestIndex = i;
            }
        }
        reader.jumpToImage(bestIndex);
        std::unique_ptr<const QImage> img(new QImage(reader.read()));
        image = std::move(img);
        mLoaded = true;
        return;
    }
    // Big brain code. It's mostly for small ico files so whatever. I'm not patching Qt for this.
    QIcon icon(mPath);
    QList<QSize> sizes = icon.availableSizes();
    QSize maxSize(0, 0);
    for(auto sz : sizes)
        if(maxSize.width() < sz.width())
            maxSize = sz;
    QPixmap iconPix = icon.pixmap(maxSize);
    std::unique_ptr<const QImage> img(new QImage(iconPix.toImage()));
    image = std::move(img);
    mLoaded = true;
}

QString ImageStatic::generateHash(QString str) {
    return QString(QCryptographicHash::hash(str.toUtf8(), QCryptographicHash::Md5).toHex());
}

// TODO: move saving to directorymodel
bool ImageStatic::save(QString destPath) {
    // Archive entries live inside a read-only container: writing them back is
    // impossible, the caller has to pick a real destination.
    if(VirtualFs::isVirtual(destPath)) {
        qDebug() << "ImageStatic::save() - archive entries are read-only:" << destPath;
        return false;
    }
    // Writing encrypted RPG Maker containers is not supported. Refuse before
    // the backup/restore code below touches the original file.
    if(EncryptedImage::isSupportedFormat(QFileInfo(destPath).suffix())) {
        qDebug() << "ImageStatic::save() - cannot write RPG Maker container:" << destPath;
        return false;
    }
    QString tmpPath = destPath + "_" + generateHash(destPath);
    QFileInfo fi(destPath);
    QString ext = fi.suffix();
    // png compression note from libpng
    // Note that tests have shown that zlib compression levels 3-6 usually perform as well
    // as level 9 for PNG images, and do considerably fewer caclulations
    int quality = 95;
    if(ext.compare("png", Qt::CaseInsensitive) == 0)
        quality = 30;
    else if(ext.compare("jpg", Qt::CaseInsensitive) == 0 || ext.compare("jpeg", Qt::CaseInsensitive) == 0)
        quality = settings->JPEGSaveQuality();

    bool backupExists = false, success = false, originalExists = false;

    if(QFile::exists(destPath))
        originalExists = true;

    // backup the original file if possible
    if(originalExists) {
        QFile::remove(tmpPath);
        if(!QFile::copy(destPath, tmpPath)) {
            qDebug() << "ImageStatic::save() - Could not create file backup.";
            return false;
        }
        backupExists = true;
    }
    // save file
    if(isEdited()) {
        success = imageEdited->save(destPath, ext.toStdString().c_str(), quality);
        image.swap(imageEdited);
        discardEditedImage();
    } else {
        success = image->save(destPath, ext.toStdString().c_str(), quality);
    }
    if(backupExists) {
        if(success) {
            // everything ok - remove the backup
            QFile file(tmpPath);
            file.remove();
        } else if(originalExists) {
            // revert on fail
            QFile::remove(mDocInfo->filePath());
            QFile::copy(tmpPath, mDocInfo->filePath());
            QFile::remove(tmpPath);
        }
    }
    if(destPath == mPath && success)
        mDocInfo->refresh();
    return success;
}

bool ImageStatic::save() {
    return save(mPath);
}

std::unique_ptr<QPixmap> ImageStatic::getPixmap() {
    std::unique_ptr<QPixmap> pix(new QPixmap());
    isEdited()?pix->convertFromImage(*imageEdited):pix->convertFromImage(*image, Qt::NoFormatConversion);
    return pix;
}

std::shared_ptr<const QImage> ImageStatic::getSourceImage() {
    return image;
}

std::shared_ptr<const QImage> ImageStatic::getImage() {
    return isEdited()?imageEdited:image;
}

int ImageStatic::height() {
    return isEdited()?imageEdited->height():image->height();
}

int ImageStatic::width() {
    return isEdited()?imageEdited->width():image->width();
}

QSize ImageStatic::size() {
    return isEdited()?imageEdited->size():image->size();
}

bool ImageStatic::setEditedImage(std::unique_ptr<const QImage> imageEditedNew) {
    if(imageEditedNew && imageEditedNew->width() != 0) {
        discardEditedImage();
        imageEdited = std::move(imageEditedNew);
        mEdited = true;
        return true;
    }
    return false;
}

bool ImageStatic::discardEditedImage() {
    if(imageEdited) {
        imageEdited.reset();
        mEdited = false;
        return true;
    }
    return false;
}
