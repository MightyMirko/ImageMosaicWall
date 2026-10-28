#include "imageprocessing.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QImage>
#include <QImageReader>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QThreadPool>

#include <exiv2/exiv2.hpp>

namespace
{

QImage solidImage(QSize size, const QColor &color)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

bool similar(const QColor &a, const QColor &b, int tolerance)
{
    return qAbs(a.red() - b.red()) <= tolerance && qAbs(a.green() - b.green()) <= tolerance &&
           qAbs(a.blue() - b.blue()) <= tolerance;
}

bool similar(const QImage &a, const QImage &b, int tolerance)
{
    if (a.size() != b.size())
        return false;

    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (!similar(a.pixelColor(x, y), b.pixelColor(x, y), tolerance))
                return false;

    return true;
}

} // namespace

class TestImageProcessing : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString saveImage(const QString &name, const QImage &image)
    {
        QString path = m_dir.filePath(name);
        if (!image.save(path))
            qFatal("could not save %s", qPrintable(path));
        return path;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());

        // Keeps the default tile cache out of the user's real cache directory.
        QStandardPaths::setTestModeEnabled(true);
    }

    void exifOrientationMatchesQt_data()
    {
        QTest::addColumn<int>("orientation");

        for (int o = 1; o <= 8; ++o)
            QTest::newRow(qPrintable(QString("orientation %1").arg(o))) << o;
    }

    // applyExifOrientation() is used for embedded previews; it must agree with
    // Qt's own auto-transform, which is used when files are loaded directly.
    void exifOrientationMatchesQt()
    {
        QFETCH(int, orientation);

        // Asymmetric content, so every mirror and rotation gives a different image.
        QImage source = solidImage(QSize(60, 40), Qt::gray);
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 20; ++x)
                source.setPixelColor(x, y, Qt::red);
        for (int y = 25; y < 40; ++y)
            for (int x = 45; x < 60; ++x)
                source.setPixelColor(x, y, Qt::blue);

        QString path = m_dir.filePath(QString("orientation%1.jpg").arg(orientation));
        QVERIFY(source.save(path, "JPEG", 100));

        auto exifImage = Exiv2::ImageFactory::open(path.toStdString());
        exifImage->readMetadata();
        exifImage->exifData()["Exif.Image.Orientation"] = static_cast<uint16_t>(orientation);
        exifImage->writeMetadata();

        QImageReader rawReader(path);
        rawReader.setAutoTransform(false);
        QImage mine = applyExifOrientation(rawReader.read(), orientation);

        QImageReader qtReader(path);
        qtReader.setAutoTransform(true);
        QImage reference = qtReader.read();

        QVERIFY(!reference.isNull());
        QCOMPARE(mine.size(), reference.size());
        QVERIFY(similar(mine.convertToFormat(QImage::Format_RGB32), reference.convertToFormat(QImage::Format_RGB32), 24));
    }

    void loadOrientedImageRotatesPortrait()
    {
        QString path = m_dir.filePath("portrait.jpg");
        QVERIFY(solidImage(QSize(60, 40), Qt::green).save(path, "JPEG", 100));

        auto exifImage = Exiv2::ImageFactory::open(path.toStdString());
        exifImage->readMetadata();
        exifImage->exifData()["Exif.Image.Orientation"] = static_cast<uint16_t>(6);
        exifImage->writeMetadata();

        QCOMPARE(loadOrientedImage(path).size(), QSize(40, 60));

        // Decoding at reduced size must cover the requested size in display
        // orientation and keep the aspect ratio, also for rotated images.
        QImage reduced = loadOrientedImage(path, QSize(10, 30));
        QVERIFY2(reduced.width() >= 10 && reduced.height() >= 30, qPrintable(QString("%1x%2").arg(reduced.width()).arg(reduced.height())));
        QVERIFY(reduced.height() < 60);
        QVERIFY(qAbs(double(reduced.width()) / reduced.height() - 40.0 / 60.0) < 0.05);

        // Never scaled up.
        QCOMPARE(loadOrientedImage(path, QSize(400, 400)).size(), QSize(40, 60));
    }

    // The save dialog must never suggest the base image itself (issue #1).
    void suggestedFileNameNeverOverwritesBaseImage()
    {
        QString base = saveImage("IMG_1234.jpg", solidImage(QSize(8, 8), Qt::red));
        QDir dir(m_dir.path());

        QCOMPARE(suggestedMosaicFileName(base), dir.filePath("IMG_1234_mosaic.png"));

        saveImage("IMG_1234_mosaic.png", solidImage(QSize(8, 8), Qt::red));
        QCOMPARE(suggestedMosaicFileName(base), dir.filePath("IMG_1234_mosaic_2.png"));

        saveImage("IMG_1234_mosaic_2.png", solidImage(QSize(8, 8), Qt::red));
        QCOMPARE(suggestedMosaicFileName(base), dir.filePath("IMG_1234_mosaic_3.png"));

        // A file that is already called *.png must not be suggested either.
        QString pngBase = saveImage("scan.png", solidImage(QSize(8, 8), Qt::red));
        QCOMPARE(suggestedMosaicFileName(pngBase), dir.filePath("scan_mosaic.png"));

        QVERIFY(suggestedMosaicFileName(QString()).endsWith("mosaic.png"));
    }

    void imageNameFilters()
    {
        QStringList filters = supportedImageNameFilters();

        QVERIFY(filters.contains("*.jpg"));
        QVERIFY(filters.contains("*.jpeg"));
        QVERIFY(filters.contains("*.png"));
        QVERIFY(!filters.contains("*.svg"));
        QVERIFY(!filters.contains("*.ico"));
        QCOMPARE(filters.removeDuplicates(), 0);
    }

    // Unchanged files come from the cache; a changed file is analysed again (issue #4).
    void cacheReusesUnchangedFiles()
    {
        QString cacheFile = m_dir.filePath("cache/tiles.sqlite");
        QString red = saveImage("cache-red.png", solidImage(QSize(16, 16), Qt::red));
        QString blue = saveImage("cache-blue.png", solidImage(QSize(16, 16), Qt::blue));

        {
            ImageProcessing first;
            first.setCacheFile(cacheFile);
            first.processMosaicImages({red, blue});
            QCOMPARE(first.cacheHits(), 0);
        }

        {
            ImageProcessing second;
            second.setCacheFile(cacheFile);
            second.processMosaicImages({red, blue});
            QCOMPARE(second.cacheHits(), 2);
            QVERIFY(similar(second.getImageMeanMap().value(red), Qt::red, 0));
            QVERIFY(similar(second.getImageMeanMap().value(blue), Qt::blue, 0));
        }

        // Same path, new content and a newer modification time.
        saveImage("cache-red.png", solidImage(QSize(16, 16), Qt::green));
        QFile changed(red);
        QVERIFY(changed.open(QIODevice::ReadWrite));
        QVERIFY(changed.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime));
        changed.close();

        {
            ImageProcessing third;
            third.setCacheFile(cacheFile);
            third.processMosaicImages({red, blue});
            QCOMPARE(third.cacheHits(), 1);
            QVERIFY(similar(third.getImageMeanMap().value(red), Qt::green, 0));
        }
    }

    // A cache that can't be created must not stop the analysis.
    void unusableCacheStillWorks()
    {
        QString image = saveImage("nocache.png", solidImage(QSize(16, 16), Qt::red));

        ImageProcessing processing;
        processing.setCacheFile(image + "/not-a-directory/tiles.sqlite");
        processing.processMosaicImages({image});

        QCOMPARE(processing.cacheHits(), 0);
        QVERIFY(similar(processing.getImageMeanMap().value(image), Qt::red, 0));
    }

    // A RAW file next to a JPEG (or any other format) of the same shot is used only once.
    void rawDuplicatesAreSkipped()
    {
        QList<QString> files = {"/photos/IMG_1.JPG", "/photos/IMG_1.CR2", "/photos/IMG_2.nef",
                                "/photos/IMG_3.heic", "/photos/img_3.dng", "/photos/other/IMG_1.cr2",
                                "/photos/IMG_4.png"};

        QList<QString> expected = {"/photos/IMG_1.JPG", "/photos/IMG_2.nef", "/photos/IMG_3.heic",
                                   "/photos/other/IMG_1.cr2", "/photos/IMG_4.png"};

        QCOMPARE(withoutRawDuplicates(files), expected);
    }

    void rawFilesPolicy()
    {
        QList<QString> files = {"/p/A.JPG", "/p/A.CR2", "/p/B.nef", "/p/C.png"};

        QCOMPARE(applyRawFilesPolicy(files, RawFiles::Always), files);
        QCOMPARE(applyRawFilesPolicy(files, RawFiles::WithoutCopies), QList<QString>({"/p/A.JPG", "/p/B.nef", "/p/C.png"}));
        QCOMPARE(applyRawFilesPolicy(files, RawFiles::Never), QList<QString>({"/p/A.JPG", "/p/C.png"}));
    }

    void gridCellsMeanOfUniformImage()
    {
        ImageProcessing processing;
        processing.processGrid(solidImage(QSize(90, 60), QColor(200, 100, 50)), QSize(3, 2));

        auto colors = processing.getGridColorMap();
        QCOMPARE(colors.size(), size_t(6));

        for (const QColor &c : colors)
            QVERIFY(similar(c, QColor(200, 100, 50), 0));
    }

    // 10 px wide with 3 columns gives 4 px cells; the last column only has 2 px
    // inside the image. It must not come out darker than the others.
    void gridEdgeCellsAreNotDarker()
    {
        ImageProcessing processing;
        processing.processGrid(solidImage(QSize(10, 10), Qt::white), QSize(3, 3));

        for (const QColor &c : processing.getGridColorMap())
            QVERIFY2(similar(c, Qt::white, 0), qPrintable(c.name()));
    }

    // 10 px wide with 6 columns gives 2 px cells; the sixth column starts at x = 10
    // and lies entirely outside the image. That must not divide by zero.
    void gridCellsOutsideTheImage()
    {
        ImageProcessing processing;
        processing.processGrid(solidImage(QSize(10, 10), Qt::white), QSize(6, 6));

        auto colors = processing.getGridColorMap();
        QCOMPARE(colors.size(), size_t(36));
        QVERIFY(similar(colors.at(0), Qt::white, 0));
    }

    void generateRefusesMismatchedGrid()
    {
        ImageProcessing processing;
        processing.processMosaicImages({saveImage("mismatch.png", solidImage(QSize(8, 8), Qt::red))});
        processing.processGrid(solidImage(QSize(40, 40), Qt::red), QSize(4, 4));

        QVERIFY(!processing.generateImage(QSize(40, 40), QSize(5, 5), 0));
        QVERIFY(processing.generateImage(QSize(40, 40), QSize(4, 4), 0));
    }

    void generatePicksClosestColor()
    {
        ImageProcessing processing;
        processing.processMosaicImages({saveImage("red.png", solidImage(QSize(8, 8), Qt::red)),
                                        saveImage("blue.png", solidImage(QSize(8, 8), Qt::blue))});

        QImage base = solidImage(QSize(40, 20), Qt::red);
        for (int y = 0; y < 20; ++y)
            for (int x = 20; x < 40; ++x)
                base.setPixelColor(x, y, Qt::blue);

        processing.processGrid(base, QSize(2, 1));
        QVERIFY(processing.generateImage(base.size(), QSize(2, 1), 0));

        const QImage *out = processing.getOutputImage();
        QVERIFY(out != nullptr);
        QCOMPARE(out->size(), base.size());
        QVERIFY(similar(out->pixelColor(10, 10), Qt::red, 0));
        QVERIFY(similar(out->pixelColor(30, 10), Qt::blue, 0));
    }

    // With only 3 images and the highest variation, every candidate is excluded
    // by the neighbourhood history for most cells. Every cell must still be filled.
    void generateFillsEveryCellWithFewImages()
    {
        ImageProcessing processing;
        processing.processMosaicImages({saveImage("few1.png", solidImage(QSize(8, 8), Qt::red)),
                                        saveImage("few2.png", solidImage(QSize(8, 8), Qt::green)),
                                        saveImage("few3.png", solidImage(QSize(8, 8), Qt::blue))});

        QImage base = solidImage(QSize(100, 100), Qt::darkGray);
        processing.processGrid(base, QSize(10, 10));
        QVERIFY(processing.generateImage(base.size(), QSize(10, 10), 15));

        const QImage *out = processing.getOutputImage();
        QVERIFY(out != nullptr);

        for (int y = 0; y < out->height(); ++y)
            for (int x = 0; x < out->width(); ++x)
                QVERIFY2(qAlpha(out->pixel(x, y)) == 255, qPrintable(QString("empty pixel at %1,%2").arg(x).arg(y)));
    }

    // A wide tile (red | green | blue thirds) in a square cell must be cropped
    // around its center, so only the green third is visible.
    void generateCropsTilesAroundTheCenter()
    {
        QImage tile(30, 10, QImage::Format_RGB32);
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 30; ++x)
                tile.setPixelColor(x, y, x < 10 ? Qt::red : (x < 20 ? Qt::green : Qt::blue));

        ImageProcessing processing;
        processing.processMosaicImages({saveImage("thirds.png", tile)});

        QImage base = solidImage(QSize(10, 10), Qt::green);
        processing.processGrid(base, QSize(1, 1));
        QVERIFY(processing.generateImage(base.size(), QSize(1, 1), 0));

        const QImage *out = processing.getOutputImage();
        QVERIFY(out != nullptr);
        QVERIFY(similar(out->pixelColor(0, 0), Qt::green, 0));
        QVERIFY(similar(out->pixelColor(5, 5), Qt::green, 0));
        QVERIFY(similar(out->pixelColor(9, 9), Qt::green, 0));
    }

    // The same settings must give the same mosaic, whatever the number of threads (issue #15).
    void generateIsReproducible()
    {
        QList<QString> files;
        for (int i = 0; i < 40; ++i)
            files << saveImage(QString("repro%1.png").arg(i), solidImage(QSize(8, 8), QColor::fromHsv(i * 9, 200, 100 + i * 3)));

        QImage base(120, 80, QImage::Format_RGB32);
        for (int y = 0; y < base.height(); ++y)
            for (int x = 0; x < base.width(); ++x)
                base.setPixelColor(x, y, QColor::fromHsv((x * 3) % 360, 200, 120 + y));

        auto generate = [&](int threads) {
            int previous = QThreadPool::globalInstance()->maxThreadCount();
            QThreadPool::globalInstance()->setMaxThreadCount(threads);

            ImageProcessing processing;
            processing.processMosaicImages(files);
            processing.processGrid(base, QSize(24, 16));
            bool ok = processing.generateImage(base.size(), QSize(24, 16), 2);
            QImage out = ok ? *processing.getOutputImage() : QImage();

            QThreadPool::globalInstance()->setMaxThreadCount(previous);
            return out;
        };

        QImage single = generate(1);
        QImage parallel = generate(QThread::idealThreadCount() > 1 ? QThread::idealThreadCount() : 4);

        QVERIFY(!single.isNull());
        QVERIFY(single == parallel);
        QVERIFY(single == generate(1));
    }

    // With variation 1, no image may appear in any of the 8 neighbouring cells, even
    // though one image is the best match for every cell.
    void generateRespectsVariation()
    {
        QList<QString> files;
        for (int i = 0; i < 12; ++i)
            files << saveImage(QString("var%1.png").arg(i), solidImage(QSize(8, 8), QColor(100 + i * 10, 100, 100)));

        QImage base = solidImage(QSize(60, 60), QColor(100, 100, 100));

        ImageProcessing processing;
        processing.processMosaicImages(files);
        processing.processGrid(base, QSize(6, 6));
        QVERIFY(processing.generateImage(base.size(), QSize(6, 6), 1));

        const QImage *out = processing.getOutputImage();
        auto cellColor = [&](int cx, int cy) { return out->pixel(cx * 10 + 5, cy * 10 + 5); };

        for (int cy = 0; cy < 6; ++cy)
            for (int cx = 0; cx < 6; ++cx)
                for (int ny = qMax(0, cy - 1); ny <= qMin(5, cy + 1); ++ny)
                    for (int nx = qMax(0, cx - 1); nx <= qMin(5, cx + 1); ++nx)
                        if (nx != cx || ny != cy)
                            QVERIFY2(cellColor(cx, cy) != cellColor(nx, ny),
                                     qPrintable(QString("cells %1,%2 and %3,%4 use the same image").arg(cx).arg(cy).arg(nx).arg(ny)));
    }

    // Grid that doesn't divide the output evenly: cells that start past the
    // image edge are skipped instead of writing outside the buffer.
    void generateWithUnevenGrid()
    {
        ImageProcessing processing;
        processing.processMosaicImages({saveImage("uneven.png", solidImage(QSize(8, 8), Qt::yellow))});

        QImage base = solidImage(QSize(100, 70), Qt::yellow);
        processing.processGrid(base, QSize(37, 23));
        QVERIFY(processing.generateImage(base.size(), QSize(37, 23), 1));

        const QImage *out = processing.getOutputImage();
        QVERIFY(out != nullptr);
        QCOMPARE(out->size(), base.size());
        QVERIFY(similar(out->pixelColor(99, 69), Qt::yellow, 0));
    }
};

QTEST_GUILESS_MAIN(TestImageProcessing)

#include "test_imageprocessing.moc"
