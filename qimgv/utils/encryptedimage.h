#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

/*
 * Front end for the encrypted image containers qimgv can read:
 *
 *   .rpgmvp  RPG Maker MV
 *   .png_    RPG Maker MZ
 *   .gax     GaxLib (used by some Crowd / doujin games)
 *
 * RPG Maker containers keep the real payload (PNG, JPEG or WebP) XORed with a
 * 16 byte project key behind a 16 byte fake header. GAX files use the same fake
 * header layout, but store the XOR key in the header and mutate it after every
 * block, so no project files are needed to read them.
 *
 * Files are normally recognized by their extension, but some games ship their
 * encrypted images with a ".png" extension. Those are found by content sniffing
 * (detectFormat()) or after the regular loader failed, and decoded here as well.
 */
class EncryptedImage {
public:
    // Container formats supported by this decoder, without leading dots.
    static QStringList supportedFormats();
    static bool isSupportedFormat(const QString &format);

    // Returns a null QImage on failure.
    static QImage decode(const QString &path, const QString &format);

    // Looks at the file contents and returns the matching container format, or
    // an empty string if the file is not an encrypted image. Cheap: reads at
    // most 64 bytes. Used for files whose extension does not match their
    // contents.
    static QString detectFormat(const QString &path);
};
