#include "encryptedimage.h"

#include "rpgmakerimage.h"

#include <QByteArray>
#include <QDebug>
#include <QFile>

#include <cstring>

namespace {

// GAX header: 4 byte signature, 16 byte XOR key, then the encrypted payload.
constexpr int kGaxHeaderSize = 0x14;
constexpr int kGaxKeySize = 16;
// Looking at 64 bytes is enough to recognize every container we support.
constexpr int kSniffSize = 64;

const char kGaxSignature[4] = { 0x00, 0x00, 0x00, 0x01 };

// First 16 bytes of every normal PNG file: signature + IHDR length/type.
const uchar kPngHeader[kGaxKeySize] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52
};

// GaxLib key schedule (see GARbro's Crowd/ImageGAX.cs): the payload is XORed
// with the current 16 byte key, then the second to last plaintext byte of every
// full block selects one of eight key updates.
inline void gaxAdvanceKey(uchar *k, uchar m) {
    switch(m & 0x7) {
    case 0:
        k[0] += m;
        k[3] += uchar(m + 2);
        k[4] = uchar(k[2] + m + 11);
        k[8] = uchar(k[6] + 7);
        break;
    case 1:
        k[2] = uchar(k[9] + k[10]);
        k[6] = uchar(k[7] + k[15]);
        k[8] += k[1];
        k[15] = uchar(k[3] + k[5]);
        break;
    case 2:
        k[1] += k[2];
        k[5] += k[6];
        k[7] += k[8];
        k[10] += k[11];
        break;
    case 3:
        k[9] = uchar(k[1] + k[2]);
        k[11] = uchar(k[5] + k[6]);
        k[12] = uchar(k[7] + k[8]);
        k[13] = uchar(k[10] + k[11]);
        break;
    case 4:
        k[0] = uchar(k[1] + 0x6F);
        k[3] = uchar(k[4] + 0x47);
        k[4] = uchar(k[5] + 0x11);
        k[14] = uchar(k[15] + 0x40);
        break;
    case 5:
        k[2] += k[10];
        k[4] = uchar(k[5] + k[12]);
        k[6] = uchar(k[8] + k[14]);
        k[8] = uchar(k[0] + k[11]);
        break;
    case 6:
        k[9] = uchar(k[1] + k[11]);
        k[11] = uchar(k[3] + k[13]);
        k[13] = uchar(k[5] + k[15]);
        k[15] = uchar(k[7] + k[9]);
        // case 6 always continues with case 7
    case 7:
        k[1] = uchar(k[5] + k[9]);
        k[2] = uchar(k[6] + k[10]);
        k[3] = uchar(k[7] + k[11]);
        k[4] = uchar(k[8] + k[12]);
        break;
    }
}

QByteArray gaxDecryptPayload(const QByteArray &payload, const QByteArray &key) {
    QByteArray result = payload;
    if(key.size() < kGaxKeySize || result.isEmpty())
        return result;

    uchar currentKey[kGaxKeySize];
    std::memcpy(currentKey, key.constData(), kGaxKeySize);

    uchar *data = reinterpret_cast<uchar *>(result.data());
    const int size = result.size();
    for(int offset = 0; offset < size; offset += kGaxKeySize) {
        const int blockSize = qMin(kGaxKeySize, size - offset);
        for(int i = 0; i < blockSize; ++i)
            data[offset + i] ^= currentKey[i];
        // The schedule is driven by the last plaintext byte of a full block.
        if(blockSize == kGaxKeySize)
            gaxAdvanceKey(currentKey, data[offset + 14]);
    }
    return result;
}

bool looksLikeGax(const QByteArray &header) {
    if(header.size() < kGaxHeaderSize + kGaxKeySize)
        return false;
    if(std::memcmp(header.constData(), kGaxSignature, sizeof(kGaxSignature)) != 0)
        return false;

    // Verify the first block: once decrypted it has to be the PNG header. The
    // same check also accepts files whose payload was not encrypted.
    const QByteArray key = header.mid(4, kGaxKeySize);
    QByteArray first = header.mid(kGaxHeaderSize, kGaxKeySize);
    for(int i = 0; i < kGaxKeySize; ++i)
        first[i] = first[i] ^ key[i];
    if(std::memcmp(first.constData(), kPngHeader, kGaxKeySize) == 0)
        return true;
    return std::memcmp(header.constData() + kGaxHeaderSize, kPngHeader, kGaxKeySize) == 0;
}

QImage decodeGax(const QByteArray &data, const QString &name) {
    if(data.isEmpty()) {
        qWarning() << "EncryptedImage: empty data" << name;
        return QImage();
    }

    if(looksLikeGax(data.left(kGaxHeaderSize + kGaxKeySize))) {
        const QByteArray key = data.mid(4, kGaxKeySize);
        QImage image = RpgMakerImage::decodeImageData(
            gaxDecryptPayload(data.mid(kGaxHeaderSize), key), name);
        if(!image.isNull())
            return image;
        // Some files keep the header but not the encryption.
        image = RpgMakerImage::decodeImageData(data.mid(kGaxHeaderSize), name);
        if(!image.isNull())
            return image;
    }

    // Header stripped or not present at all: try the data as is.
    QImage image = RpgMakerImage::decodeImageData(data, name);
    if(!image.isNull())
        return image;
    if(data.size() > kGaxHeaderSize) {
        image = RpgMakerImage::decodeImageData(data.mid(kGaxHeaderSize), name);
        if(!image.isNull())
            return image;
    }

    qWarning() << "EncryptedImage: cannot decode GAX container" << name;
    return QImage();
}

} // namespace

QStringList EncryptedImage::supportedFormats() {
    QStringList formats = RpgMakerImage::supportedFormats();
    formats << QStringLiteral("gax");
    return formats;
}

bool EncryptedImage::isSupportedFormat(const QString &format) {
    return RpgMakerImage::isSupportedFormat(format)
        || format.compare(QLatin1String("gax"), Qt::CaseInsensitive) == 0;
}

QImage EncryptedImage::decode(const QString &path, const QString &format) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) {
        qWarning() << "EncryptedImage: cannot open" << path;
        return QImage();
    }
    const QByteArray data = file.readAll();
    file.close();
    return decodeData(data, format, path);
}

QImage EncryptedImage::decodeData(const QByteArray &data, const QString &format,
                                  const QString &name) {
    if(data.isEmpty()) {
        qWarning() << "EncryptedImage: empty data" << name;
        return QImage();
    }
    if(RpgMakerImage::isSupportedFormat(format))
        return RpgMakerImage::decodeImageData(data, name);
    if(format.compare(QLatin1String("gax"), Qt::CaseInsensitive) != 0) {
        qWarning() << "EncryptedImage: unsupported container format" << format;
        return QImage();
    }
    return decodeGax(data, name);
}

QString EncryptedImage::detectFormat(const QString &path) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly))
        return QString();
    const QByteArray header = file.read(kSniffSize);
    file.close();
    return detectFormat(header);
}

QString EncryptedImage::detectFormat(const QByteArray &header) {
    if(looksLikeGax(header))
        return QStringLiteral("gax");
    // MV and MZ containers share the same layout and are decoded by the same
    // code, so reporting the MZ extension is fine for both.
    if(RpgMakerImage::looksEncrypted(header))
        return QStringLiteral("png_");
    return QString();
}
