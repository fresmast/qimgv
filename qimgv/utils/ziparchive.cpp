#include "ziparchive.h"

#include <QBuffer>
#include <QDate>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QTime>

#include <cstring>

#include <zlib.h>

namespace {

constexpr quint32 kSigEOCD = 0x06054b50;
constexpr quint32 kSigCentralDir = 0x02014b50;
constexpr quint32 kSigLocalHeader = 0x04034b50;
constexpr quint32 kSigZip64EOCD = 0x06064b50;
constexpr quint32 kSigZip64Locator = 0x07064b50;

constexpr quint32 kUnknown32 = 0xFFFFFFFF;

inline quint16 rd16(const char *p) {
    return quint16(quint8(p[0])) | (quint16(quint8(p[1])) << 8);
}

inline quint32 rd32(const char *p) {
    return quint32(quint8(p[0]))
        | (quint32(quint8(p[1])) << 8)
        | (quint32(quint8(p[2])) << 16)
        | (quint32(quint8(p[3])) << 24);
}

inline quint64 rd64(const char *p) {
    return quint64(rd32(p)) | (quint64(rd32(p + 4)) << 32);
}

QDateTime dosDateTime(quint16 time, quint16 date) {
    if(date == 0 && time == 0)
        return QDateTime();
    const int year = 1980 + ((date >> 9) & 0x7f);
    const int month = (date >> 5) & 0xf;
    const int day = date & 0x1f;
    const int hour = (time >> 11) & 0x1f;
    const int minute = (time >> 5) & 0x3f;
    const int second = (time & 0x1f) * 2;
    return QDateTime(QDate(year, qBound(1, month, 12), qBound(1, day, 31)),
                     QTime(qBound(0, hour, 23), qBound(0, minute, 59), qBound(0, second, 59)));
}

// Info-ZIP stores UTF-8 names when general purpose bit 11 is set, everything
// else is whatever the code page of the archiver was.
QString decodeName(const QByteArray &raw, bool utf8) {
    if(utf8)
        return QString::fromUtf8(raw);
    const QString utf8Name = QString::fromUtf8(raw);
    if(utf8Name.toUtf8() == raw)
        return utf8Name;
    return QString::fromLocal8Bit(raw);
}

QString normalizeName(const QString &name) {
    QString result = name;
    result.replace('\\', '/');
    while(result.startsWith('/'))
        result.remove(0, 1);
    return result;
}

QByteArray inflateRaw(const QByteArray &input, qint64 expectedSize) {
    if(expectedSize <= 0 || expectedSize > (qint64(1) << 31))
        return QByteArray();

    QByteArray output;
    output.resize(int(expectedSize));

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    strm.avail_in = uInt(input.size());
    strm.next_out = reinterpret_cast<Bytef *>(output.data());
    strm.avail_out = uInt(output.size());

    if(inflateInit2(&strm, -MAX_WBITS) != Z_OK)
        return QByteArray();

    const int ret = inflate(&strm, Z_FINISH);
    const qint64 produced = qint64(output.size()) - qint64(strm.avail_out);
    inflateEnd(&strm);

    if(ret != Z_STREAM_END || produced != expectedSize)
        return QByteArray();

    return output;
}

QByteArray readAt(QIODevice &device, qint64 offset, qint64 size) {
    if(offset < 0 || size < 0)
        return QByteArray();
    if(!device.seek(offset))
        return QByteArray();
    return device.read(size);
}

void setError(QString *error, const QString &message) {
    if(error)
        *error = message;
}

} // namespace

std::shared_ptr<ZipArchive> ZipArchive::open(const QString &path, QString *error) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) {
        setError(error, "cannot open file");
        return nullptr;
    }

    return openFrom(file, path, QByteArray(), error);
}

std::shared_ptr<ZipArchive> ZipArchive::openBytes(const QByteArray &data, QString *error) {
    if(data.size() < 22) {
        setError(error, "file too small to be a zip");
        return nullptr;
    }

    QByteArray buffer = data;
    QBuffer device(&buffer);
    if(!device.open(QIODevice::ReadOnly)) {
        setError(error, "cannot open memory buffer");
        return nullptr;
    }

    return openFrom(device, QString(), data, error);
}

std::shared_ptr<ZipArchive> ZipArchive::openFrom(QIODevice &file, const QString &path,
                                                 const QByteArray &memoryData, QString *error) {
    const qint64 fileSize = file.size();
    if(fileSize < 22) {
        setError(error, "file too small to be a zip");
        return nullptr;
    }

    // The end of central directory record sits at the very end, possibly
    // followed by a comment of up to 64 KiB.
    const qint64 tailSize = qMin<qint64>(fileSize, 22 + 65535);
    const QByteArray tail = readAt(file, fileSize - tailSize, tailSize);
    if(tail.size() != tailSize) {
        setError(error, "cannot read end of central directory");
        return nullptr;
    }

    int eocdPos = -1;
    for(int i = int(tail.size()) - 22; i >= 0; --i) {
        if(rd32(tail.constData() + i) == kSigEOCD) {
            eocdPos = i;
            break;
        }
    }
    if(eocdPos < 0) {
        setError(error, "not a zip archive");
        return nullptr;
    }

    const char *eocd = tail.constData() + eocdPos;
    quint64 entryCount = rd16(eocd + 10);
    quint64 cdSize = rd32(eocd + 12);
    quint64 cdOffset = rd32(eocd + 16);

    // zip64: the classic fields are saturated and real values live in the
    // zip64 end of central directory record, pointed at by a locator.
    if(cdOffset == kUnknown32 || cdSize == kUnknown32 || entryCount == 0xFFFF) {
        if(eocdPos >= 20 && rd32(eocd - 20) == kSigZip64Locator) {
            const quint64 zip64Offset = rd64(eocd - 20 + 8);
            const QByteArray zip64 = readAt(file, qint64(zip64Offset), 56);
            if(zip64.size() == 56 && rd32(zip64.constData()) == kSigZip64EOCD) {
                entryCount = rd64(zip64.constData() + 32);
                cdSize = rd64(zip64.constData() + 40);
                cdOffset = rd64(zip64.constData() + 48);
            }
        }
        if(cdOffset == kUnknown32 || cdSize == kUnknown32 || entryCount == 0xFFFF) {
            setError(error, "zip64 archive without readable zip64 end record");
            return nullptr;
        }
    }

    if(cdOffset + cdSize > quint64(fileSize)) {
        setError(error, "central directory out of bounds");
        return nullptr;
    }

    const QByteArray centralDir = readAt(file, qint64(cdOffset), qint64(cdSize));
    if(quint64(centralDir.size()) != cdSize) {
        setError(error, "cannot read central directory");
        return nullptr;
    }

    std::shared_ptr<ZipArchive> archive(new ZipArchive);
    archive->mPath = path;
    archive->mData = memoryData;

    int pos = 0;
    while(quint64(pos) + 46 <= quint64(centralDir.size())
          && entryCount > quint64(archive->mEntries.size())) {
        const char *cd = centralDir.constData() + pos;
        if(rd32(cd) != kSigCentralDir)
            break;

        const quint16 flags = rd16(cd + 8);
        const quint16 method = rd16(cd + 10);
        const quint16 modTime = rd16(cd + 12);
        const quint16 modDate = rd16(cd + 14);
        const quint32 crc = rd32(cd + 16);
        quint64 compSize = rd32(cd + 20);
        quint64 uncompSize = rd32(cd + 24);
        const quint16 nameLen = rd16(cd + 28);
        const quint16 extraLen = rd16(cd + 30);
        const quint16 commentLen = rd16(cd + 32);
        quint64 localOffset = rd32(cd + 42);

        if(pos + 46 + nameLen + extraLen + commentLen > centralDir.size())
            break;

        const QByteArray rawName = centralDir.mid(pos + 46, nameLen);
        const QByteArray extra = centralDir.mid(pos + 46 + nameLen, extraLen);

        // zip64 extended information extra field
        if(compSize == kUnknown32 || uncompSize == kUnknown32 || localOffset == kUnknown32) {
            int extraPos = 0;
            while(extraPos + 4 <= extra.size()) {
                const quint16 headerId = rd16(extra.constData() + extraPos);
                const quint16 dataSize = rd16(extra.constData() + extraPos + 2);
                if(headerId == 0x0001) {
                    const char *data = extra.constData() + extraPos + 4;
                    int fieldPos = 0;
                    if(uncompSize == kUnknown32 && fieldPos + 8 <= dataSize) {
                        uncompSize = rd64(data + fieldPos);
                        fieldPos += 8;
                    }
                    if(compSize == kUnknown32 && fieldPos + 8 <= dataSize) {
                        compSize = rd64(data + fieldPos);
                        fieldPos += 8;
                    }
                    if(localOffset == kUnknown32 && fieldPos + 8 <= dataSize)
                        localOffset = rd64(data + fieldPos);
                    break;
                }
                extraPos += 4 + dataSize;
            }
        }

        Entry entry;
        entry.name = normalizeName(decodeName(rawName, flags & 0x800));
        entry.size = qint64(uncompSize);
        entry.compressedSize = qint64(compSize);
        entry.crc = crc;
        entry.method = method;
        entry.flags = flags;
        entry.localHeaderOffset = quint32(localOffset);
        entry.mtime = dosDateTime(modTime, modDate);
        entry.isDir = entry.name.endsWith('/') || (entry.size == 0 && entry.name.isEmpty());
        if(entry.name.endsWith('/'))
            entry.name.chop(1);

        // Encrypted entries cannot be read without a password; skip them so
        // they simply don't show up in the browser.
        if(!entry.name.isEmpty() && !(flags & 0x1)) {
            archive->mIndex.insert(entry.name, archive->mEntries.size());
            archive->mEntries.append(entry);
        }

        pos += 46 + nameLen + extraLen + commentLen;
    }

    if(archive->mEntries.isEmpty()) {
        setError(error, "archive has no readable entries");
        return nullptr;
    }

    return archive;
}

bool ZipArchive::find(const QString &name, Entry &entry) const {
    const auto it = mIndex.constFind(name);
    if(it == mIndex.constEnd())
        return false;
    entry = mEntries.at(it.value());
    return true;
}

QByteArray ZipArchive::readData(const Entry &entry) const {
    if(entry.isDir || entry.flags & 0x1)
        return QByteArray();

    QFile file;
    if(mData.isEmpty()) {
        file.setFileName(mPath);
        if(!file.open(QIODevice::ReadOnly))
            return QByteArray();
    }

    auto fetch = [&](qint64 offset, qint64 size) -> QByteArray {
        if(offset < 0 || size < 0)
            return QByteArray();
        if(!mData.isEmpty()) {
            if(offset + size > mData.size())
                return QByteArray();
            return mData.mid(int(offset), int(size));
        }
        if(!file.seek(offset))
            return QByteArray();
        return file.read(size);
    };

    const QByteArray localHeader = fetch(qint64(entry.localHeaderOffset), 30);
    if(localHeader.size() != 30 || rd32(localHeader.constData()) != kSigLocalHeader) {
        qDebug() << "ZipArchive: bad local header for" << entry.name;
        return QByteArray();
    }
    const quint16 nameLen = rd16(localHeader.constData() + 26);
    const quint16 extraLen = rd16(localHeader.constData() + 28);

    const QByteArray compressed = fetch(qint64(entry.localHeaderOffset) + 30 + nameLen + extraLen,
                                        entry.compressedSize);
    if(compressed.size() != entry.compressedSize) {
        qDebug() << "ZipArchive: truncated data for" << entry.name;
        return QByteArray();
    }

    QByteArray data;
    if(entry.method == 0) {
        data = compressed;
    } else if(entry.method == 8) {
        data = inflateRaw(compressed, entry.size);
    } else {
        qDebug() << "ZipArchive: unsupported compression method"
                 << entry.method << "for" << entry.name;
        return QByteArray();
    }

    if(data.size() != entry.size) {
        qDebug() << "ZipArchive: size mismatch for" << entry.name;
        return QByteArray();
    }

    const quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef *>(data.constData()),
                                      uInt(data.size())));
    if(crc != entry.crc) {
        qDebug() << "ZipArchive: crc mismatch for" << entry.name;
        return QByteArray();
    }

    return data;
}
