#include "tiffwriter.h"

#include <vector>

// The layout follows TIFF 6.0 (baseline RGB, uncompressed) and the BigTIFF extension:
//
//     header | pixel rows, top to bottom | strip tables and values | image directory
//
// The pixels come right after the header, so they can be written as they come; the
// directory, which points to them, is written last and the header updated to point to it.

namespace
{

enum Type : quint16
{
    Short = 3,
    Long = 4,
    Rational = 5,
    Long8 = 16
};

const int ClassicHeaderSize = 8;
const int BigHeaderSize = 16;

// Strips of about 1 MB, so readers can load parts of the image.
int rowsPerStrip(QSize size)
{
    return qMax(1, (1 << 20) / (size.width() * 3));
}

struct Entry
{
    quint16 tag;
    quint16 type;
    quint64 count;
    QByteArray data;   // the values, little endian; stored in the entry if they fit
};

QByteArray littleEndian(quint64 value, int bytes)
{
    QByteArray data(bytes, 0);
    for (int i = 0; i < bytes; ++i)
        data[i] = char((value >> (8 * i)) & 0xff);
    return data;
}

Entry shortEntry(quint16 tag, std::initializer_list<quint16> values)
{
    QByteArray data;
    for (quint16 v : values)
        data += littleEndian(v, 2);
    return {tag, Short, quint64(values.size()), data};
}

Entry longEntry(quint16 tag, quint32 value)
{
    return {tag, Long, 1, littleEndian(value, 4)};
}

Entry rationalEntry(quint16 tag, quint32 numerator, quint32 denominator)
{
    return {tag, Rational, 1, littleEndian(numerator, 4) + littleEndian(denominator, 4)};
}

} // namespace

TiffWriter::~TiffWriter()
{
    if (m_file.isOpen())
        m_file.close();
}

bool TiffWriter::open(const QString &fileName, QSize size, int dotsPerInch)
{
    m_size = size;
    m_dotsPerInch = dotsPerInch;
    m_rowsWritten = 0;
    m_error.clear();

    if (size.isEmpty())
        return fail(QStringLiteral("The image is empty."));

    // Classic TIFF uses 32 bit offsets; the pixels plus the strip tables must stay below 4 GB.
    qint64 pixelBytes = qint64(size.width()) * size.height() * 3;
    qint64 strips = (size.height() + rowsPerStrip(size) - 1) / rowsPerStrip(size);
    m_bigTiff = m_forceBigTiff || pixelBytes + strips * 8 + 4096 > Q_INT64_C(0xffffffff);

    m_file.setFileName(fileName);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(m_file.errorString());

    // The directory offset is filled in by close().
    QByteArray header = m_bigTiff ? QByteArray("II") + littleEndian(43, 2) + littleEndian(8, 2) + littleEndian(0, 2) +
                                        littleEndian(0, 8)
                                  : QByteArray("II") + littleEndian(42, 2) + littleEndian(0, 4);

    if (m_file.write(header) != header.size())
        return fail(m_file.errorString());

    return true;
}

bool TiffWriter::writeRows(const QImage &rows)
{
    if (!m_file.isOpen())
        return fail(QStringLiteral("The file is not open."));

    if (rows.width() != m_size.width() || m_rowsWritten + rows.height() > m_size.height())
        return fail(QStringLiteral("The rows don't fit the image."));

    QImage rgb = rows.convertToFormat(QImage::Format_RGB888);

    for (int y = 0; y < rgb.height(); ++y)
    {
        qint64 bytes = qint64(rgb.width()) * 3;
        if (m_file.write(reinterpret_cast<const char *>(rgb.constScanLine(y)), bytes) != bytes)
            return fail(m_file.errorString());
    }

    m_rowsWritten += rgb.height();
    return true;
}

bool TiffWriter::close()
{
    if (!m_file.isOpen())
        return fail(QStringLiteral("The file is not open."));

    if (m_rowsWritten != m_size.height())
        return fail(QStringLiteral("Not all rows of the image were written."));

    int offsetBytes = m_bigTiff ? 8 : 4;
    qint64 dataStart = m_bigTiff ? BigHeaderSize : ClassicHeaderSize;
    qint64 rowBytes = qint64(m_size.width()) * 3;
    int stripRows = rowsPerStrip(m_size);
    int strips = (m_size.height() + stripRows - 1) / stripRows;

    QByteArray offsets, byteCounts;
    for (int i = 0; i < strips; ++i)
    {
        int rows = qMin(stripRows, m_size.height() - i * stripRows);
        offsets += littleEndian(quint64(dataStart + qint64(i) * stripRows * rowBytes), offsetBytes);
        byteCounts += littleEndian(quint64(rows * rowBytes), offsetBytes);
    }

    quint16 offsetType = m_bigTiff ? Long8 : Long;

    // Sorted by tag, as required.
    std::vector<Entry> entries{
        longEntry(256, quint32(m_size.width())),          // ImageWidth
        longEntry(257, quint32(m_size.height())),         // ImageLength
        shortEntry(258, {8, 8, 8}),                       // BitsPerSample
        shortEntry(259, {1}),                             // Compression: none
        shortEntry(262, {2}),                             // PhotometricInterpretation: RGB
        {273, offsetType, quint64(strips), offsets},      // StripOffsets
        shortEntry(277, {3}),                             // SamplesPerPixel
        longEntry(278, quint32(stripRows)),               // RowsPerStrip
        {279, offsetType, quint64(strips), byteCounts},   // StripByteCounts
        rationalEntry(282, quint32(m_dotsPerInch), 1),    // XResolution
        rationalEntry(283, quint32(m_dotsPerInch), 1),    // YResolution
        shortEntry(284, {1}),                             // PlanarConfiguration: interleaved
        shortEntry(296, {2}),                             // ResolutionUnit: inch
    };

    // Values that don't fit into their entry go before the directory, word aligned.
    qint64 position = m_file.pos();
    QByteArray extra;
    std::vector<qint64> valueOffsets(entries.size(), -1);

    auto align = [&]() {
        while ((position + extra.size()) % 2 != 0)
            extra += char(0);
    };

    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].data.size() > offsetBytes)
        {
            align();
            valueOffsets[i] = position + extra.size();
            extra += entries[i].data;
        }
    }

    align();
    qint64 directoryOffset = position + extra.size();

    QByteArray directory = littleEndian(entries.size(), m_bigTiff ? 8 : 2);
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const Entry &entry = entries[i];
        directory += littleEndian(entry.tag, 2) + littleEndian(entry.type, 2) + littleEndian(entry.count, offsetBytes);

        if (valueOffsets[i] >= 0)
            directory += littleEndian(quint64(valueOffsets[i]), offsetBytes);
        else
            directory += entry.data + QByteArray(offsetBytes - entry.data.size(), 0);
    }
    directory += littleEndian(0, offsetBytes);   // no further directory

    if (m_file.write(extra) != extra.size() || m_file.write(directory) != directory.size())
        return fail(m_file.errorString());

    // Point the header to the directory.
    QByteArray pointer = littleEndian(quint64(directoryOffset), offsetBytes);
    if (!m_file.seek(m_bigTiff ? 8 : 4) || m_file.write(pointer) != pointer.size())
        return fail(m_file.errorString());

    m_file.close();
    if (m_file.error() != QFileDevice::NoError)
        return fail(m_file.errorString());

    return true;
}

QString TiffWriter::errorString() const
{
    return m_error;
}

void TiffWriter::setForceBigTiff(bool force)
{
    m_forceBigTiff = force;
}

bool TiffWriter::isBigTiff() const
{
    return m_bigTiff;
}

bool TiffWriter::fail(const QString &error)
{
    m_error = error;

    if (m_file.isOpen())
        m_file.close();

    return false;
}
