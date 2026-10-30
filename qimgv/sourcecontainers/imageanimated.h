#pragma once

#include "image.h"
#include <QBuffer>
#include <QMovie>
#include <QTimer>

class ImageAnimated : public Image {
public:
    ImageAnimated(QString _path);
    ImageAnimated(std::unique_ptr<DocumentInfo> _info);
    ~ImageAnimated();

    std::unique_ptr<QPixmap> getPixmap();
    std::shared_ptr<const QImage> getImage();
    std::shared_ptr<QMovie> getMovie();
    int height();
    int width();
    QSize size();

    bool isEditable();
    bool isEdited();

    int frameCount();
public slots:
    bool save();
    bool save(QString destPath);

signals:
    void frameChanged(QPixmap*);

private:
    void load();
    QSize mSize;
    int mFrameCount;
    // Declared before "movie" so it is destroyed after it: QMovie keeps using
    // the device until it is destroyed.
    std::unique_ptr<QBuffer> mBuffer;
    std::shared_ptr<QMovie> movie;
    void loadMovie();
};
