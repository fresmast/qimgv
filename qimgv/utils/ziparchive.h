#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

#include <memory>

class QIODevice;

// Minimal read-only zip reader.
//
// Only what the archive browser needs: central directory index, stored and
// deflate entries, zip64 sizes. Encrypted entries are skipped, no writing.
class ZipArchive {
public:
    struct Entry {
        QString name;               // normalized: '/' separators, no leading '/'
        qint64 size = 0;            // uncompressed size
        qint64 compressedSize = 0;
        quint32 crc = 0;
        quint16 method = 0;         // 0 = stored, 8 = deflate
        quint16 flags = 0;
        quint32 localHeaderOffset = 0;
        QDateTime mtime;
        bool isDir = false;
    };

    // Returns nullptr on failure; error gets a human readable reason if set.
    static std::shared_ptr<ZipArchive> open(const QString &path, QString *error = nullptr);
    // Same, for an archive that lives in memory (archives inside archives).
    static std::shared_ptr<ZipArchive> openBytes(const QByteArray &data, QString *error = nullptr);

    const QVector<Entry> &entries() const { return mEntries; }

    bool find(const QString &name, Entry &entry) const;

    // Decompresses one entry. Returns an empty array on failure.
    QByteArray readData(const Entry &entry) const;

private:
    static std::shared_ptr<ZipArchive> openFrom(QIODevice &device, const QString &path,
                                                const QByteArray &memoryData, QString *error);

    QString mPath;
    QByteArray mData; // non-empty for archives opened from memory
    QVector<Entry> mEntries;
    QHash<QString, int> mIndex;
};
