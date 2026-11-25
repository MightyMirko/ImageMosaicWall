#ifndef TIFFWRITER_H
#define TIFFWRITER_H

#include <QFile>
#include <QImage>
#include <QSize>
#include <QString>

// Writes an uncompressed 8 bit RGB TIFF a few rows at a time, so images far larger
// than the memory can be saved. Files over 4 GB are written as BigTIFF.
//
//     TiffWriter writer;
//     writer.open(fileName, size, 300);
//     for (each strip of rows)
//         writer.writeRows(strip);
//     writer.close();
class TiffWriter
{
public:
    ~TiffWriter();

    bool open(const QString &fileName, QSize size, int dotsPerInch);
    // The next rows of the image, from the top; any format, as wide as the image.
    bool writeRows(const QImage &rows);
    // Writes the image directory; fails if not all rows were written.
    bool close();
    QString errorString() const;

    // BigTIFF also for small files; for tests.
    void setForceBigTiff(bool force);
    bool isBigTiff() const;

private:
    bool fail(const QString &error);

    QFile m_file;
    QSize m_size;
    int m_dotsPerInch = 0;
    int m_rowsWritten = 0;
    bool m_forceBigTiff = false;
    bool m_bigTiff = false;
    QString m_error;
};

#endif // TIFFWRITER_H
