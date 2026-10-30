#include "rpgmakerimage.h"

#include "settings.h"

#include <QBuffer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>

namespace {

constexpr int kFakeHeaderSize = 16;
constexpr int kKeySize = 16;

// First 16 bytes of every normal PNG file: signature + IHDR length/type.
const uchar kPngHeader[kFakeHeaderSize] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52
};

// Valid keys are cached per image directory, so loading a folder full of
// encrypted images does not re-read System.json for every file.
QMutex g_keyCacheMutex;
QHash<QString, QList<QByteArray>> g_keyCache;

} // namespace

QStringList RpgMakerImage::supportedFormats() {
    return QStringList() << QStringLiteral("rpgmvp") << QStringLiteral("png_");
}

bool RpgMakerImage::isSupportedFormat(const QString &format) {
    const QString lower = format.toLower();
    return lower == QLatin1String("rpgmvp") || lower == QLatin1String("png_");
}

QImage RpgMakerImage::decode(const QString &path, const QString &format) {
    if(!isSupportedFormat(format))
        return QImage();
    return decodeEncrypted(path);
}

QImage RpgMakerImage::decodeEncrypted(const QString &path) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) {
        qWarning() << "RpgMakerImage: cannot open" << path;
        return QImage();
    }
    const QByteArray data = file.readAll();
    file.close();

    if(data.isEmpty()) {
        qWarning() << "RpgMakerImage: empty file" << path;
        return QImage();
    }

    // Some tools strip the fake header and leave a normal image behind.
    // Accept these files as well instead of failing on them.
    if(isKnownImageHeader(data.left(kFakeHeaderSize)))
        return decodeImageData(data, path);

    if(data.size() < kFakeHeaderSize * 2) {
        qWarning() << "RpgMakerImage: file is too short" << path;
        return QImage();
    }

    // The payload is usually a PNG. In that case the key can be recovered
    // from the known first 16 bytes without touching System.json at all.
    // Validate the following IHDR CRC first: for JPEG/WebP payloads the same
    // XOR trick would still produce a PNG-looking header and only fail later.
    if(isPngPayload(data)) {
        const QByteArray pngKey = keyFromPngHeader(data);
        if(!pngKey.isEmpty()) {
            const QImage image = decodeImageData(decrypt(data, pngKey), path);
            if(!image.isNull())
                return image;
        }
    }

    // JPEG/WebP payloads require the project key from System.json or from
    // rpg_core.js. Try every candidate until one passes the header check.
    const QList<QByteArray> candidates = findKeyCandidates(path);
    for(const QByteArray &key : candidates) {
        if(key.size() < kKeySize)
            continue;
        if(!keyMatches(data, key))
            continue;
        const QImage image = decodeImageData(decrypt(data, key), path);
        if(!image.isNull())
            return image;
    }

    qWarning() << "RpgMakerImage: no valid encryption key for" << path;
    return QImage();
}

QImage RpgMakerImage::decodeImageData(const QByteArray &data, const QString &path) {
    if(data.isEmpty())
        return QImage();

    QBuffer buffer;
    buffer.setData(data);
    if(!buffer.open(QIODevice::ReadOnly))
        return QImage();

    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if(settings)
        reader.setAllocationLimit(settings->memoryAllocationLimit());
#endif

    QImage image = reader.read();
    if(image.isNull())
        qWarning() << "RpgMakerImage: cannot decode image data from" << path
                   << ":" << reader.errorString();
    return image;
}

QByteArray RpgMakerImage::keyFromPngHeader(const QByteArray &data) {
    if(data.size() < kFakeHeaderSize * 2)
        return QByteArray();

    QByteArray key(kKeySize, 0);
    for(int i = 0; i < kKeySize; ++i)
        key[i] = data[kFakeHeaderSize + i] ^ static_cast<char>(kPngHeader[i]);
    return key;
}

bool RpgMakerImage::isPngPayload(const QByteArray &data) {
    // Layout for a standard encrypted PNG:
    //   0..15   fake header
    //   16..31  encrypted PNG signature + IHDR length/type
    //   32..44  plain IHDR data (13 bytes)
    //   45..48  plain IHDR CRC (big endian)
    if(data.size() < 49)
        return false;

    const uchar *raw = reinterpret_cast<const uchar *>(data.constData());
    const uchar *ihdrData = raw + 32;
    const uchar *crcPtr = raw + 45;

    // CRC32 over "IHDR" + IHDR data. The chunk type is known because the
    // first 16 bytes of every PNG are fixed up to and including "IHDR".
    quint32 crc = 0xFFFFFFFFu;
    const uchar chunkType[4] = { 'I', 'H', 'D', 'R' };
    for(int i = 0; i < 4; ++i) {
        crc ^= chunkType[i];
        for(int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    for(int i = 0; i < 13; ++i) {
        crc ^= ihdrData[i];
        for(int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    crc ^= 0xFFFFFFFFu;

    const quint32 expected = (quint32(crcPtr[0]) << 24)
                           | (quint32(crcPtr[1]) << 16)
                           | (quint32(crcPtr[2]) << 8)
                           |  quint32(crcPtr[3]);
    return crc == expected;
}

bool RpgMakerImage::keyMatches(const QByteArray &data, const QByteArray &key) {
    if(key.size() < kKeySize || data.size() < kFakeHeaderSize + kKeySize)
        return false;

    // 12 bytes are enough to tell PNG, JPEG and WebP apart.
    QByteArray header(12, 0);
    for(int i = 0; i < header.size(); ++i)
        header[i] = data[kFakeHeaderSize + i] ^ key[i];
    return isKnownImageHeader(header);
}

QByteArray RpgMakerImage::decrypt(const QByteArray &data, const QByteArray &key) {
    if(key.size() < kKeySize || data.size() < kFakeHeaderSize * 2)
        return QByteArray();

    QByteArray result = data.mid(kFakeHeaderSize, kKeySize);
    for(int i = 0; i < kKeySize; ++i)
        result[i] = result[i] ^ key[i];

    const int payloadOffset = kFakeHeaderSize * 2;
    result.append(data.constData() + payloadOffset, data.size() - payloadOffset);
    return result;
}

bool RpgMakerImage::isKnownImageHeader(const QByteArray &header) {
    if(header.startsWith("\x89PNG\r\n\x1a\n"))
        return true;
    if(header.startsWith("\xFF\xD8\xFF"))
        return true;
    if(header.size() >= 12 && header.startsWith("RIFF") && header.mid(8, 4) == "WEBP")
        return true;
    if(header.startsWith("GIF87a") || header.startsWith("GIF89a"))
        return true;
    if(header.startsWith("BM"))
        return true;
    return false;
}

QList<QByteArray> RpgMakerImage::findKeyCandidates(const QString &path) {
    const QString cacheKey = QFileInfo(path).absolutePath();
    {
        QMutexLocker locker(&g_keyCacheMutex);
        auto it = g_keyCache.constFind(cacheKey);
        if(it != g_keyCache.constEnd())
            return it.value();
    }

    QList<QByteArray> keys;
    QSet<QByteArray> seen;

    // Search from the image directory upwards. This covers both MV
    // (www/data/System.json) and MZ (data/System.json) layouts.
    QDir dir = QFileInfo(path).absoluteDir();
    while(true) {
        const QStringList locations = {
            QStringLiteral("data/System.json"),
            QStringLiteral("www/data/System.json")
        };
        for(const QString &location : locations) {
            const QString jsonPath = dir.filePath(location);
            if(!QFileInfo::exists(jsonPath))
                continue;
            const QByteArray key = readSystemKey(jsonPath);
            if(key.size() >= kKeySize && !seen.contains(key)) {
                seen.insert(key);
                keys.append(key);
            }
        }
        if(!dir.cdUp())
            break;
    }

    // A few games hardcode the key in rpg_core.js instead of System.json.
    if(keys.isEmpty()) {
        dir = QFileInfo(path).absoluteDir();
        while(true) {
            const QStringList locations = {
                QStringLiteral("js/rpg_core.js"),
                QStringLiteral("www/js/rpg_core.js")
            };
            for(const QString &location : locations) {
                const QString jsPath = dir.filePath(location);
                if(!QFileInfo::exists(jsPath))
                    continue;
                const QByteArray key = readCoreKey(jsPath);
                if(key.size() >= kKeySize && !seen.contains(key)) {
                    seen.insert(key);
                    keys.append(key);
                }
            }
            if(!dir.cdUp())
                break;
        }
    }

    {
        QMutexLocker locker(&g_keyCacheMutex);
        g_keyCache.insert(cacheKey, keys);
    }
    return keys;
}

QByteArray RpgMakerImage::readSystemKey(const QString &systemJsonPath) {
    QFile file(systemJsonPath);
    if(!file.open(QIODevice::ReadOnly))
        return QByteArray();

    QByteArray json = file.readAll();
    if(json.startsWith("\xEF\xBB\xBF"))
        json.remove(0, 3);

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if(error.error != QJsonParseError::NoError || !document.isObject())
        return QByteArray();

    const QString keyString = document.object().value(QStringLiteral("encryptionKey"))
                                                    .toString().trimmed();
    static const QRegularExpression keyRegex(QStringLiteral("^[0-9A-Fa-f]{32}$"));
    if(!keyRegex.match(keyString).hasMatch())
        return QByteArray();

    return QByteArray::fromHex(keyString.toLatin1());
}

QByteArray RpgMakerImage::readCoreKey(const QString &coreJsPath) {
    QFile file(coreJsPath);
    if(!file.open(QIODevice::ReadOnly))
        return QByteArray();

    const QString source = QString::fromUtf8(file.readAll());
    static const QRegularExpression keyRegex(
        QStringLiteral("_encryptionKey\\s*=\\s*[\"']([0-9A-Fa-f]{32})[\"']"));
    const QRegularExpressionMatch match = keyRegex.match(source);
    if(!match.hasMatch())
        return QByteArray();

    return QByteArray::fromHex(match.captured(1).toLatin1());
}
