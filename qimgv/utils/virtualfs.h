#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <vector>

// Read-only virtual filesystem that exposes archives as directories.
//
// A path inside an archive looks like "D:/games/Game.zip!img/pictures/a.png":
// the part before '!' is the real archive file, everything after it is the
// entry name inside the archive. The archive root itself is ".../Game.zip!".
//
// Everything here falls back to the real filesystem for regular paths, so
// callers can treat both uniformly.
namespace VirtualFs {

struct Entry {
    QString name;   // display name, last path component
    QString path;   // full path, virtual or real
    qint64 size = 0;
    QDateTime mtime;
    bool isDir = false;
};

// Archive extensions that are shown as folders.
QStringList archiveSuffixes();

// Extension check only, no stat. Real paths only.
bool isArchiveFile(const QString &path);
// Extension check that also accepts paths inside archives (nested archives).
bool isArchivePath(const QString &path);

bool isVirtual(const QString &path);

// Splits "a.zip!inner/path" into "a.zip" and "inner/path" (empty for the root).
bool split(const QString &path, QString &archivePath, QString &innerPath);

// Joins an archive path and an entry name; an empty entry returns the root.
QString join(const QString &archivePath, const QString &innerPath);

QString fileName(const QString &path);
QString suffix(const QString &path);
QString parentPath(const QString &path);

// The real file behind a path: the archive itself for virtual paths.
QString realPath(const QString &path);
// Directory that holds the real file; a good default for "save as".
QString realDirectory(const QString &path);

QDateTime lastModified(const QString &path);

bool exists(const QString &path);
bool isDirectory(const QString &path);
bool stat(const QString &path, Entry &entry);

// Whole (decompressed) contents of a file, virtual or real.
QByteArray readAll(const QString &path);
// First maxBytes of a file; avoids decompressing more than necessary.
QByteArray readHead(const QString &path, int maxBytes);

// Lists a directory inside an archive. Returns false for real paths, callers
// keep using their own directory iteration for those.
bool listDir(const QString &dirPath, std::vector<Entry> &out, bool recursive);

} // namespace VirtualFs
