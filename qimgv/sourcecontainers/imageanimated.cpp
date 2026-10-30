#include "imageanimated.h"
#include "utils/virtualfs.h"
#include <time.h>

// TODO: this class is kinda useless now. redesign?

ImageAnimated::ImageAnimated(QString _path)
    : Image(_path)
{
    mSize.setWidth(0);
    mSize.setHeight(0);
    load();
}

ImageAnimated::ImageAnimated(std::unique_ptr<DocumentInfo> _info)
    : Image(std::move(_info))
{
    mSize.setWidth(0);
    mSize.setHeight(0);
    load();
}

ImageAnimated::~ImageAnimated() {
}

void ImageAnimated::load() {
    if(isLoaded())
        return;
    loadMovie();
    mLoaded = true;
}

void ImageAnimated::loadMovie() {
    movie.reset(new QMovie());
    // QMovie only takes a file name, so archive entries are fed through a
    // buffer that stays alive as long as the movie does.
    if(VirtualFs::isVirtual(mPath)) {
        mBuffer.reset(new QBuffer());
        mBuffer->setData(VirtualFs::readAll(mPath));
        mBuffer->open(QIODevice::ReadOnly);
        movie->setDevice(mBuffer.get());
    } else {
        mBuffer.reset();
        movie->setFileName(mPath);
    }
    movie->setFormat(mDocInfo->format().toStdString().c_str());
    movie->jumpToFrame(0);
    mSize = movie->frameRect().size();
    mFrameCount = movie->frameCount();
}

int ImageAnimated::frameCount() {
    return mFrameCount;
}

// TODO: overwrite (self included)
bool ImageAnimated::save(QString destPath) {
    if(VirtualFs::isVirtual(mPath)) {
        // read-only source: write the raw bytes to the chosen destination
        const QByteArray data = VirtualFs::readAll(mPath);
        QFile out(destPath);
        if(data.isEmpty() || !out.open(QIODevice::WriteOnly))
            return false;
        return out.write(data) == data.size();
    }
    QFile file(mPath);
    if(file.exists()) {
        if(!file.copy(destPath)) {
            qDebug() << "Unable to save file.";
            return false;
        } else {
            qDebug() << destPath << this->filePath();
            if(destPath == this->filePath()) {
                mDocInfo->refresh();
            }
            return true;
        }
    } else {
        qDebug() << "Unable to save file. Perhaps the source file was deleted?";
        return false;
    }
}

bool ImageAnimated::save() {
    //TODO
    return false;
}

// in case of gif returns current frame
std::unique_ptr<QPixmap> ImageAnimated::getPixmap() {
    if(VirtualFs::isVirtual(mPath)) {
        const QImage frame = QImage::fromData(VirtualFs::readAll(mPath));
        return std::unique_ptr<QPixmap>(new QPixmap(QPixmap::fromImage(frame)));
    }
    return std::unique_ptr<QPixmap>(new QPixmap(mPath, mDocInfo->format().toStdString().c_str()));
}

std::shared_ptr<const QImage> ImageAnimated::getImage() {
    if(VirtualFs::isVirtual(mPath)) {
        std::shared_ptr<QImage> frame(new QImage(QImage::fromData(VirtualFs::readAll(mPath))));
        return frame;
    }
    std::shared_ptr<const QImage> img(new QImage(mPath, mDocInfo->format().toStdString().c_str()));
    return img;
}

std::shared_ptr<QMovie> ImageAnimated::getMovie() {
    if(movie == nullptr)
        loadMovie();
    return movie;
}

int ImageAnimated::height() {
    return mSize.height();
}

int ImageAnimated::width() {
    return mSize.width();
}

QSize ImageAnimated::size() {
    return mSize;
}
