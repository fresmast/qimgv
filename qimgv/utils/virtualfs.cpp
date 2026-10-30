#include "virtualfs.h"
#include "ziparchive.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>

namespace {

// Opened archives are cached by path and invalidated when the file on disk
// changes. Thumbnail threads and the browsed directory both hit this.
struct CacheItem {
    std::shared_ptr<ZipArchive> archive;
    qint64 mtime = 0;
    qint64 size = 0;
};

QHash<QString, CacheItem> gCache;
QMutex gCacheMutex;

std::shared_ptr<ZipArchive> openArchive(const QString &archivePath) {
    // Cache validity is checked against the real file at the bottom of the
    // nesting chain: "a.zip!b.zip!c.zip" depends on "a.zip".
    QString realFile = archivePath;
    for(;;) {
        QString outer;
        QString inner;
        if(!VirtualFs::split(realFile, outer, inner))
            break;
        realFile = outer;
    }
    const QFileInfo info(realFile);
    if(!info.exists() || !info.isFile())
        return nullptr;

    const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
    const qint64 size = info.size();

    {
        QMutexLocker locker(&gCacheMutex);
        const auto it = gCache.constFind(archivePath);
        if(it != gCache.constEnd() && it->mtime == mtime && it->size == size)
            return it->archive;
    }

    QString error;
    std::shared_ptr<ZipArchive> archive;

    QString outerPath;
    QString innerPath;
    if(VirtualFs::split(archivePath, outerPath, innerPath)) {
        // An archive stored inside another archive: unpack the inner one into
        // memory and index it from there.
        if(archivePath.count(QLatin1Char('!')) > 4) {
            qDebug() << "VirtualFs: archive nesting too deep:" << archivePath;
            return nullptr;
        }
        const std::shared_ptr<ZipArchive> parent = openArchive(outerPath);
        if(parent) {
            ZipArchive::Entry entry;
            if(parent->find(innerPath, entry)) {
                const QByteArray data = parent->readData(entry);
                if(!data.isEmpty())
                    archive = ZipArchive::openBytes(data, &error);
            }
        }
    } else {
        archive = ZipArchive::open(archivePath, &error);
    }

    if(!archive) {
        qDebug() << "VirtualFs: cannot open" << archivePath << ":" << error;
        QMutexLocker locker(&gCacheMutex);
        gCache.remove(archivePath);
        return nullptr;
    }

    QMutexLocker locker(&gCacheMutex);
    gCache.insert(archivePath, CacheItem{archive, mtime, size});
    return archive;
}

// Paths can arrive with native (backslash) separators from the command line,
// the shell or file associations. Everything inside VirtualFs works with '/'.
QString norm(const QString &path) {
    if(!path.contains('\\'))
        return path;
    QString out = path;
    out.replace('\\', '/');
    return out;
}

} // namespace

namespace VirtualFs {

QStringList archiveSuffixes() {
    return QStringList{"zip"};
}

bool isArchivePath(const QString &path) {
    const QString p = norm(path);
    const int dot = p.lastIndexOf('.');
    if(dot < 0)
        return false;
    const int lastSeparator = qMax(p.lastIndexOf('/'), p.lastIndexOf('!'));
    if(dot < lastSeparator)
        return false;
    return archiveSuffixes().contains(p.mid(dot + 1).toLower());
}

bool isArchiveFile(const QString &path) {
    if(path.contains('!'))
        return false;
    return isArchivePath(path);
}

bool isVirtual(const QString &path) {
    QString archivePath;
    QString innerPath;
    if(!split(path, archivePath, innerPath))
        return false;
    if(archivePath.contains('!'))
        return isVirtual(archivePath);
    return QFileInfo(archivePath).isFile();
}

bool split(const QString &path, QString &archivePath, QString &innerPath) {
    const QString p = norm(path);
    const int bang = p.lastIndexOf('!');
    if(bang <= 0)
        return false;
    archivePath = p.left(bang);
    innerPath = p.mid(bang + 1);
    while(innerPath.endsWith('/'))
        innerPath.chop(1);
    while(innerPath.startsWith('/'))
        innerPath.remove(0, 1);
    return true;
}

QString join(const QString &archivePath, const QString &innerPath) {
    QString inner = norm(innerPath);
    while(inner.startsWith('/'))
        inner.remove(0, 1);
    if(inner.isEmpty())
        return norm(archivePath) + QLatin1Char('!');
    return norm(archivePath) + QLatin1Char('!') + inner;
}

QString fileName(const QString &path) {
    const QString p = norm(path);
    int pos = p.lastIndexOf('/');
    const int bang = p.lastIndexOf('!');
    if(bang > pos)
        pos = bang;
    const QString name = p.mid(pos + 1);
    if(name.isEmpty() && bang > 0)
        return QFileInfo(p.left(bang)).fileName();
    return name;
}

QString suffix(const QString &path) {
    const QString name = fileName(path);
    const int dot = name.lastIndexOf('.');
    if(dot <= 0)
        return QString();
    return name.mid(dot + 1).toLower();
}

QString parentPath(const QString &path) {
    const QString p = norm(path);
    const int bang = p.lastIndexOf('!');
    if(bang > 0) {
        const QString archivePath = p.left(bang);
        QString inner = p.mid(bang + 1);
        while(inner.endsWith('/'))
            inner.chop(1);
        if(inner.isEmpty())
            return QFileInfo(archivePath).absolutePath();
        const int slash = inner.lastIndexOf('/');
        if(slash < 0)
            return archivePath + QLatin1Char('!');
        return archivePath + QLatin1Char('!') + inner.left(slash);
    }

    QString trimmed = p;
    while(trimmed.size() > 1 && trimmed.endsWith('/'))
        trimmed.chop(1);
    const int slash = trimmed.lastIndexOf('/');
    if(slash < 0)
        return QString();
#ifdef _WIN32
    // "/C:/dir" style roots are not valid, keep the drive letter path
    if(slash == 2 && trimmed.at(1) == ':')
        return trimmed.left(3);
#endif
    return trimmed.left(slash);
}

QString realPath(const QString &path) {
    // Walk out of every nesting level: "a.zip!b.zip!c.png" -> "a.zip".
    QString current = norm(path);
    for(;;) {
        QString archivePath;
        QString innerPath;
        if(!split(current, archivePath, innerPath))
            return current;
        current = archivePath;
    }
}

QString realDirectory(const QString &path) {
    return QFileInfo(realPath(path)).absolutePath();
}

QDateTime lastModified(const QString &path) {
    const QString p = norm(path);
    QString archivePath;
    QString innerPath;
    if(split(p, archivePath, innerPath)) {
        Entry entry;
        if(innerPath.isEmpty() || !stat(p, entry) || !entry.mtime.isValid())
            return QFileInfo(realPath(p)).lastModified();
        return entry.mtime;
    }
    return QFileInfo(p).lastModified();
}

bool exists(const QString &path) {
    Entry entry;
    return stat(path, entry);
}

bool isDirectory(const QString &path) {
    Entry entry;
    return stat(path, entry) && entry.isDir;
}

bool stat(const QString &path, Entry &entry) {
    QString archivePath;
    QString innerPath;
    if(split(path, archivePath, innerPath)) {
        const std::shared_ptr<ZipArchive> archive = openArchive(archivePath);
        if(!archive)
            return false;

        const QFileInfo archiveInfo(archivePath);
        const QDateTime archiveMtime = archiveInfo.isFile()
                ? archiveInfo.lastModified()
                : QFileInfo(realPath(archivePath)).lastModified();
        entry = Entry();
        entry.path = norm(path);
        entry.name = fileName(path);
        entry.mtime = archiveMtime;

        if(innerPath.isEmpty()) {
            // the archive itself behaves like a directory
            entry.name = archiveInfo.fileName();
            entry.isDir = true;
            return true;
        }

        ZipArchive::Entry zipEntry;
        if(archive->find(innerPath, zipEntry)) {
            entry.size = zipEntry.size;
            if(zipEntry.mtime.isValid())
                entry.mtime = zipEntry.mtime;
            // archives usually store folders as explicit entries too
            entry.isDir = zipEntry.isDir;
            return true;
        }

        // directories are often not stored as explicit entries
        const QString prefix = innerPath + QLatin1Char('/');
        for(const ZipArchive::Entry &candidate : archive->entries()) {
            if(candidate.name == innerPath || candidate.name.startsWith(prefix)) {
                entry.isDir = true;
                return true;
            }
        }

        return false;
    }

    const QFileInfo info(norm(path));
    if(!info.exists())
        return false;

    entry = Entry();
    entry.name = info.fileName();
    entry.path = norm(path);
    entry.size = info.size();
    entry.mtime = info.lastModified();
    entry.isDir = info.isDir();
    return true;
}

QByteArray readAll(const QString &path) {
    QString archivePath;
    QString innerPath;
    if(split(path, archivePath, innerPath)) {
        if(innerPath.isEmpty())
            return QByteArray();
        const std::shared_ptr<ZipArchive> archive = openArchive(archivePath);
        if(!archive)
            return QByteArray();
        ZipArchive::Entry entry;
        if(!archive->find(innerPath, entry))
            return QByteArray();
        return archive->readData(entry);
    }

    QFile file(norm(path));
    if(!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

QByteArray readHead(const QString &path, int maxBytes) {
    QString archivePath;
    QString innerPath;
    if(split(path, archivePath, innerPath)) {
        const QByteArray data = readAll(path);
        if(data.isEmpty())
            return data;
        return data.left(maxBytes);
    }

    QFile file(norm(path));
    if(!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.read(maxBytes);
}

bool listDir(const QString &dirPath, std::vector<Entry> &out, bool recursive) {
    QString archivePath;
    QString innerPath;
    if(!split(dirPath, archivePath, innerPath))
        return false;

    const std::shared_ptr<ZipArchive> archive = openArchive(archivePath);
    if(!archive)
        return false;

    const QFileInfo archiveInfo(archivePath);
    const QDateTime archiveMtime = archiveInfo.isFile()
            ? archiveInfo.lastModified()
            : QFileInfo(realPath(archivePath)).lastModified();
    const QString prefix = innerPath.isEmpty() ? QString() : innerPath + QLatin1Char('/');

    QSet<QString> knownDirs;
    auto appendDir = [&](const QString &name) {
        if(knownDirs.contains(name))
            return;
        knownDirs.insert(name);
        Entry entry;
        entry.name = name;
        entry.path = join(archivePath, innerPath.isEmpty() ? name : innerPath + QLatin1Char('/') + name);
        entry.isDir = true;
        entry.mtime = archiveMtime;
        out.push_back(entry);
    };

    for(const ZipArchive::Entry &zipEntry : archive->entries()) {
        if(!prefix.isEmpty() && !zipEntry.name.startsWith(prefix))
            continue;

        const QString relative = zipEntry.name.mid(prefix.size());
        if(relative.isEmpty())
            continue;

        if(zipEntry.isDir) {
            if(!recursive) {
                // explicit folder entries can be nested ("a/b/"): the list only
                // shows the first level
                const int slash = relative.indexOf('/');
                appendDir(slash >= 0 ? relative.left(slash) : relative);
            }
            continue;
        }

        const int slash = relative.indexOf('/');
        if(slash >= 0) {
            if(!recursive)
                appendDir(relative.left(slash));
            continue;
        }

        // an archive stored inside the archive is browsable as another folder
        const bool nestedArchive = isArchivePath(relative)
                && dirPath.count(QLatin1Char('!')) < 4;

        Entry entry;
        entry.name = relative;
        entry.path = join(archivePath, zipEntry.name);
        entry.size = zipEntry.size;
        entry.mtime = zipEntry.mtime.isValid() ? zipEntry.mtime : archiveMtime;
        entry.isDir = nestedArchive;
        out.push_back(entry);
    }

    return true;
}

} // namespace VirtualFs
