#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QString>
#include <QStringList>

/*
 * Decoder for RPG Maker MV/MZ encrypted images.
 *
 * .rpgmvp (MV) and .png_ (MZ) files consist of a 16-byte fake header,
 * followed by 16 bytes of the original image header XORed with the
 * project encryption key, followed by the rest of the original file.
 * The payload itself may be PNG, JPEG or WebP.
 */
class RpgMakerImage {
public:
    // Formats supported by this decoder, without leading dots.
    static QStringList supportedFormats();
    static bool isSupportedFormat(const QString &format);

    // Returns a null QImage on failure.
    static QImage decode(const QString &path, const QString &format);

private:
    static QImage decodeEncrypted(const QString &path);
    static QImage decodeImageData(const QByteArray &data, const QString &path);

    static QList<QByteArray> findKeyCandidates(const QString &path);
    static QByteArray readSystemKey(const QString &systemJsonPath);
    static QByteArray readCoreKey(const QString &coreJsPath);

    static QByteArray keyFromPngHeader(const QByteArray &data);
    static bool isPngPayload(const QByteArray &data);
    static bool keyMatches(const QByteArray &data, const QByteArray &key);
    static QByteArray decrypt(const QByteArray &data, const QByteArray &key);

    static bool isKnownImageHeader(const QByteArray &header);
};
