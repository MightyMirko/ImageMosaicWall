#include "imageprocessing.h"
#include "tiffwriter.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QSet>
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

    // Issue #19: rows for tiles of a fixed shape.
    void rowsForTileShape()
    {
        QCOMPARE(rowsForTileAspect(QSize(3000, 2000), 30, 3.0 / 2.0), 30);   // 100 x 66.7 px
        QCOMPARE(rowsForTileAspect(QSize(3000, 2000), 30, 1.0), 20);         // 100 x 100 px
        QCOMPARE(rowsForTileAspect(QSize(2000, 3000), 20, 2.0 / 3.0), 20);   // 100 x 150 px
        QCOMPARE(rowsForTileAspect(QSize(5464, 8192), 100, 3.0 / 2.0), 225);
        QCOMPARE(rowsForTileAspect(QSize(4000, 100), 10, 1.0), 1);           // never 0
        QCOMPARE(rowsForTileAspect(QSize(), 10, 1.0), 1);
    }

    // With a folder, e.g. where the last mosaic was saved, the name is unique there.
    void suggestedFileNameInFolder()
    {
        QString base = saveImage("IMG_5678.jpg", solidImage(QSize(8, 8), Qt::red));
        QTemporaryDir other;
        QVERIFY(other.isValid());
        QDir dir(other.path());

        QCOMPARE(suggestedMosaicFileName(base, other.path()), dir.filePath("IMG_5678_mosaic.png"));

        QVERIFY(solidImage(QSize(8, 8), Qt::red).save(dir.filePath("IMG_5678_mosaic.png")));
        QCOMPARE(suggestedMosaicFileName(base, other.path()), dir.filePath("IMG_5678_mosaic_2.png"));

        QCOMPARE(suggestedMosaicFileName(QString(), other.path()), dir.filePath("mosaic.png"));
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

    // A use limit spreads a uniform area over several images (issue #14).
    void generateRespectsUseLimit()
    {
        QList<QString> files;
        for (int i = 0; i < 4; ++i)
            files << saveImage(QString("limit%1.png").arg(i), solidImage(QSize(8, 8), QColor(100 + i * 20, 100, 100)));

        QImage base = solidImage(QSize(40, 40), QColor(100, 100, 100));

        auto usesPerImage = [&](int maxUses) {
            ImageProcessing processing;
            processing.processMosaicImages(files);
            processing.processGrid(base, QSize(4, 4));
            processing.generateImage(base.size(), QSize(4, 4), 0, maxUses);

            QMap<QRgb, int> uses;
            for (int cy = 0; cy < 4; ++cy)
                for (int cx = 0; cx < 4; ++cx)
                    ++uses[processing.getOutputImage()->pixel(cx * 10 + 5, cy * 10 + 5)];
            return uses;
        };

        QCOMPARE(usesPerImage(0).size(), 1);           // no limit: the best image everywhere

        QMap<QRgb, int> limited = usesPerImage(4);     // 16 cells, 4 images, at most 4 each
        QCOMPARE(limited.size(), 4);
        for (int count : limited)
            QCOMPARE(count, 4);

        QMap<QRgb, int> tooLow = usesPerImage(2);      // impossible: still every cell filled
        int total = 0;
        for (int count : tooLow)
            total += count;
        QCOMPARE(total, 16);
    }

    // "Use every photo at least once" (issue #14): even photos that fit nowhere appear,
    // each in the cell it fits best.
    void generateUsesEveryImage()
    {
        // A dark base with a light top-left corner; one image fits the base, the
        // others only roughly the corner.
        QImage base = solidImage(QSize(60, 60), QColor(20, 20, 20));
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x)
                base.setPixelColor(x, y, QColor(240, 240, 240));

        QList<QString> files;
        files << saveImage("every-dark.png", solidImage(QSize(8, 8), QColor(20, 20, 20)));
        for (int i = 0; i < 5; ++i)
            files << saveImage(QString("every%1.png").arg(i), solidImage(QSize(8, 8), QColor(200, 40 * i, 255 - 40 * i)));

        auto uses = [&](bool everyImage, Matching matching) {
            ImageProcessing processing;
            processing.processMosaicImages(files);
            processing.processGrid(base, QSize(6, 6));
            processing.generateImage(base.size(), QSize(6, 6), 0, 0, 0.0, matching, everyImage);

            QMap<QRgb, int> result;
            for (int cy = 0; cy < 6; ++cy)
                for (int cx = 0; cx < 6; ++cx)
                    ++result[processing.getOutputImage()->pixel(cx * 10 + 5, cy * 10 + 5)];
            return result;
        };

        for (Matching matching : {Matching::ColorLayout, Matching::MeanColor})
        {
            QVERIFY(uses(false, matching).size() < 6);

            QMap<QRgb, int> every = uses(true, matching);
            QCOMPARE(every.size(), 6);
            QCOMPARE(every.value(QColor(20, 20, 20).rgb()), 31);   // all the remaining cells
        }
    }

    // With more photos than cells, every cell gets a different photo instead.
    void generateEveryImageWithTooFewCells()
    {
        QList<QString> files;
        for (int i = 0; i < 10; ++i)
            files << saveImage(QString("few%1.png").arg(i), solidImage(QSize(8, 8), QColor(100 + i * 5, 100, 100)));

        QImage base = solidImage(QSize(30, 30), QColor(100, 100, 100));

        ImageProcessing processing;
        processing.processMosaicImages(files);
        processing.processGrid(base, QSize(3, 3));
        QVERIFY(processing.generateImage(base.size(), QSize(3, 3), 0, 0, 0.0, Matching::MeanColor, true));

        QSet<QRgb> seen;
        for (int cy = 0; cy < 3; ++cy)
            for (int cx = 0; cx < 3; ++cx)
                seen.insert(processing.getOutputImage()->pixel(cx * 10 + 5, cy * 10 + 5));
        QCOMPARE(seen.size(), 9);
    }

    // Issue #17: the output can be larger than the base image, which is only sampled.
    void generateOutputLargerThanBase()
    {
        QImage base = solidImage(QSize(40, 30), QColor(200, 30, 30));
        for (int y = 0; y < 30; ++y)
            for (int x = 20; x < 40; ++x)
                base.setPixelColor(x, y, QColor(30, 30, 200));

        QList<QString> files{saveImage("big-red.png", solidImage(QSize(16, 12), QColor(200, 30, 30))),
                             saveImage("big-blue.png", solidImage(QSize(16, 12), QColor(30, 30, 200)))};

        ImageProcessing processing;
        processing.processMosaicImages(files);
        processing.processGrid(base, QSize(4, 3));
        QVERIFY(processing.generateImage(QSize(400, 300), QSize(4, 3), 0));

        const QImage *out = processing.getOutputImage();
        QCOMPARE(out->size(), QSize(400, 300));
        QVERIFY(similar(out->pixelColor(50, 150), QColor(200, 30, 30), 2));
        QVERIFY(similar(out->pixelColor(350, 150), QColor(30, 30, 200), 2));
        QCOMPARE(qAlpha(out->pixel(399, 299)), 255);
    }

    // Issue #18: any part of the mosaic can be drawn on its own, exactly as in the
    // whole mosaic, so large ones can be saved piece by piece.
    void renderRegionsMatchWholeMosaic()
    {
        QList<QString> files;
        for (int i = 0; i < 8; ++i)
            files << saveImage(QString("region%1.png").arg(i), solidImage(QSize(30, 20), QColor::fromHsv(i * 45, 200, 200)));

        QImage base(150, 100, QImage::Format_RGB32);
        for (int y = 0; y < base.height(); ++y)
            for (int x = 0; x < base.width(); ++x)
                base.setPixelColor(x, y, QColor::fromHsv((x * 2 + y) % 360, 200, 200));

        for (Pattern pattern : {Pattern::Grid, Pattern::Parquet})
        {
            ImageProcessing processing;
            processing.processMosaicImages(files);
            processing.processGrid(base, QSize(7, 5));
            QVERIFY(processing.generateImage(QSize(301, 203), QSize(7, 5), 1, 0, 0.25, Matching::ColorLayout, false,
                                             pattern));
            QImage whole = *processing.getOutputImage();

            // Strips, and a region that starts and ends inside cells.
            QImage stitched(whole.size(), QImage::Format_ARGB32);
            QPainter painter(&stitched);
            for (int y = 0; y < whole.height(); y += 37)
            {
                QRect strip(0, y, whole.width(), qMin(37, whole.height() - y));
                painter.drawImage(strip.topLeft(), processing.renderRegion(strip));
            }
            painter.end();
            QVERIFY(stitched == whole);

            QRect inside(23, 17, 101, 88);
            QVERIFY(processing.renderRegion(inside) == whole.copy(inside));

            // A preview has the mosaic's aspect ratio and fits the size asked for.
            QImage preview = processing.renderPreview(QSize(100, 100));
            QCOMPARE(preview.size(), QSize(100, 67));
            QCOMPARE(qAlpha(preview.pixel(99, 66)), 255);
        }
    }

    // A mosaic larger than the memory limit is placed, but not drawn as a whole.
    void largeMosaicIsNotKeptInMemory()
    {
        QList<QString> files{saveImage("limit-a.png", solidImage(QSize(30, 20), Qt::red)),
                             saveImage("limit-b.png", solidImage(QSize(30, 20), Qt::blue))};
        QImage base = solidImage(QSize(60, 40), Qt::red);

        ImageProcessing processing;
        processing.setMaxImageBytes(1000 * 1000 * 4 - 1);
        processing.processMosaicImages(files);
        processing.processGrid(base, QSize(6, 4));

        QVERIFY(processing.generateImage(QSize(999, 999), QSize(6, 4), 0));
        QVERIFY(processing.getOutputImage() != nullptr);

        QVERIFY(processing.generateImage(QSize(1500, 1000), QSize(6, 4), 0));
        QVERIFY(processing.getOutputImage() == nullptr);
        QVERIFY(processing.hasMosaic());
        QCOMPARE(processing.mosaicSize(), QSize(1500, 1000));
        QVERIFY(similar(processing.renderRegion(QRect(1400, 900, 50, 50)).pixelColor(10, 10), Qt::red, 0));
    }

    void tiffWriter_data()
    {
        QTest::addColumn<bool>("bigTiff");
        QTest::newRow("TIFF") << false;
        QTest::newRow("BigTIFF") << true;
    }

    // Issue #18: large mosaics are saved as TIFF in strips, without the whole image in memory.
    void tiffWriter()
    {
        QFETCH(bool, bigTiff);

        QImage image(37, 23, QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixel(x, y, qRgb(x * 7, y * 11, (x * y) % 256));

        QString path = m_dir.filePath(bigTiff ? "strips-big.tif" : "strips.tif");
        TiffWriter writer;
        writer.setForceBigTiff(bigTiff);
        QVERIFY(writer.open(path, image.size(), 150));
        QVERIFY(writer.writeRows(image.copy(0, 0, 37, 10)));
        QVERIFY(writer.writeRows(image.copy(0, 10, 37, 1)));
        QVERIFY(!writer.close());   // rows missing
        QVERIFY(!writer.errorString().isEmpty());

        QVERIFY(writer.open(path, image.size(), 150));
        QCOMPARE(writer.isBigTiff(), bigTiff);
        QVERIFY(writer.writeRows(image.copy(0, 0, 37, 10)));
        QVERIFY(writer.writeRows(image.copy(0, 10, 37, 13)));
        QVERIFY(!writer.writeRows(image.copy(0, 0, 37, 1)));   // one too many
        QVERIFY(writer.open(path, image.size(), 150));
        QVERIFY(writer.writeRows(image));
        QVERIFY(writer.close());

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray header = file.read(4);
        QCOMPARE(header, bigTiff ? QByteArray("II\x2b\x00", 4) : QByteArray("II\x2a\x00", 4));
        QVERIFY(file.size() > qint64(37 * 23 * 3));
        file.close();

        if (!QImageReader::supportedImageFormats().contains("tiff"))
            QSKIP("Qt has no TIFF reader here to read the file back");

        QImage read(path);
        QCOMPARE(read.size(), image.size());
        QVERIFY(similar(read.convertToFormat(QImage::Format_RGB32), image.convertToFormat(QImage::Format_RGB32), 0));
        QCOMPARE(qRound(read.dotsPerMeterX() * 0.0254), 150);
    }

    // Issue #20: in the parquet pattern a slot holds one portrait photo or two landscape
    // photos stacked, and the share of portrait slots follows the share of portraits.
    void parquetPlacesPhotosByOrientation()
    {
        // Slots of 20 x 28 px, cells of 20 x 28 (portrait) or 20 x 14 (landscape).
        QImage base = solidImage(QSize(160, 112), QColor(122, 122, 122));
        QSize grid(8, 4);

        QList<QColor> portraitColors{QColor(120, 120, 130)};
        QList<QColor> landscapeColors{QColor(120, 130, 120), QColor(125, 125, 125), QColor(115, 120, 125),
                                      QColor(120, 115, 120)};
        QList<QString> files;
        for (int i = 0; i < portraitColors.size(); ++i)
            files << saveImage(QString("pq-portrait%1.png").arg(i), solidImage(QSize(20, 30), portraitColors[i]));
        for (int i = 0; i < landscapeColors.size(); ++i)
            files << saveImage(QString("pq-landscape%1.png").arg(i), solidImage(QSize(30, 20), landscapeColors[i]));

        auto isOneOf = [](QRgb pixel, const QList<QColor> &colors) {
            for (const QColor &c : colors)
                if (similar(QColor(pixel), c, 2))
                    return true;
            return false;
        };

        for (Matching matching : {Matching::ColorLayout, Matching::MeanColor})
        {
            ImageProcessing processing;
            processing.processMosaicImages(files);
            processing.processGrid(base, grid);
            QVERIFY(processing.generateImage(base.size(), grid, 0, 0, 0.0, matching, false, Pattern::Parquet));
            const QImage *out = processing.getOutputImage();

            std::vector<bool> portrait(grid.width() * grid.height());
            for (int sy = 0; sy < grid.height(); ++sy)
            {
                for (int sx = 0; sx < grid.width(); ++sx)
                {
                    QRgb top = out->pixel(sx * 20 + 10, sy * 28 + 7);
                    QRgb bottom = out->pixel(sx * 20 + 10, sy * 28 + 21);

                    if (isOneOf(top, portraitColors))
                    {
                        QCOMPARE(bottom, top);   // one photo over the whole slot
                        portrait[sy * grid.width() + sx] = true;
                    }
                    else
                    {
                        QVERIFY(isOneOf(top, landscapeColors));
                        QVERIFY(isOneOf(bottom, landscapeColors));
                        QVERIFY(top != bottom);   // two photos, even with variation 0
                    }
                }
            }

            // 1 portrait and 4 landscape photos: 2 * 1 / (4 + 2 * 1) of the 32 slots.
            QCOMPARE(int(std::count(portrait.begin(), portrait.end(), true)), 11);

            // In an even area portraits are spread out instead of filling whole rows.
            for (int sy = 0; sy < grid.height(); ++sy)
                for (int sx = 0; sx + 1 < grid.width(); ++sx)
                    QVERIFY(!(portrait[sy * grid.width() + sx] && portrait[sy * grid.width() + sx + 1]));
        }
    }

    // With photos of only one orientation, every slot takes that orientation.
    void parquetWithOneOrientation()
    {
        QImage base = solidImage(QSize(80, 56), QColor(100, 100, 100));
        QSize grid(4, 2);

        auto portraitSlots = [&](const QList<QString> &files) {
            ImageProcessing processing;
            processing.processMosaicImages(files);
            processing.processGrid(base, grid);
            processing.generateImage(base.size(), grid, 0, 0, 0.0, Matching::MeanColor, false, Pattern::Parquet);
            const QImage *out = processing.getOutputImage();

            int count = 0;
            for (int sy = 0; sy < grid.height(); ++sy)
                for (int sx = 0; sx < grid.width(); ++sx)
                    if (out->pixel(sx * 20 + 10, sy * 28 + 7) == out->pixel(sx * 20 + 10, sy * 28 + 21))
                        ++count;
            return count;
        };

        QList<QString> landscapes{saveImage("one-l1.png", solidImage(QSize(30, 20), QColor(100, 100, 110))),
                                  saveImage("one-l2.png", solidImage(QSize(30, 20), QColor(110, 100, 100)))};
        QList<QString> portraits{saveImage("one-p1.png", solidImage(QSize(20, 30), QColor(100, 100, 110))),
                                 saveImage("one-p2.png", solidImage(QSize(20, 30), QColor(110, 100, 100)))};

        QCOMPARE(portraitSlots(landscapes), 0);
        QCOMPARE(portraitSlots(portraits), 8);
    }

    // Odd slot sizes: the stacked cells together fill the slot, nothing stays empty.
    void parquetCoversOddSizes()
    {
        QImage base = solidImage(QSize(101, 143), QColor(90, 140, 60));
        QList<QString> files{saveImage("odd-l.png", solidImage(QSize(30, 20), QColor(90, 140, 60))),
                             saveImage("odd-l2.png", solidImage(QSize(30, 20), QColor(95, 135, 65))),
                             saveImage("odd-p.png", solidImage(QSize(20, 30), QColor(90, 140, 60)))};

        ImageProcessing processing;
        processing.processMosaicImages(files);
        processing.processGrid(base, QSize(7, 7));
        QVERIFY(processing.generateImage(base.size(), QSize(7, 7), 1, 0, 0.0, Matching::ColorLayout, false,
                                         Pattern::Parquet));
        const QImage *out = processing.getOutputImage();

        for (int y = 0; y < out->height(); ++y)
            for (int x = 0; x < out->width(); ++x)
                QVERIFY2(qAlpha(out->pixel(x, y)) == 255, qPrintable(QString("pixel %1,%2 is empty").arg(x).arg(y)));
    }

    // "Use every photo" also works with the parquet pattern.
    void parquetUsesEveryImage()
    {
        QImage base = solidImage(QSize(200, 140), QColor(30, 30, 30));
        QList<QString> files;
        files << saveImage("pqe-dark-l.png", solidImage(QSize(30, 20), QColor(30, 30, 30)));
        files << saveImage("pqe-dark-p.png", solidImage(QSize(20, 30), QColor(30, 30, 45)));
        for (int i = 0; i < 3; ++i)
        {
            files << saveImage(QString("pqe-l%1.png").arg(i), solidImage(QSize(30, 20), QColor(200, 60 * i, 40)));
            files << saveImage(QString("pqe-p%1.png").arg(i), solidImage(QSize(20, 30), QColor(40, 60 * i, 200)));
        }

        ImageProcessing processing;
        processing.processMosaicImages(files);
        processing.processGrid(base, QSize(10, 5));
        QVERIFY(processing.generateImage(base.size(), QSize(10, 5), 0, 0, 0.0, Matching::MeanColor, true,
                                         Pattern::Parquet));
        const QImage *out = processing.getOutputImage();

        QSet<QRgb> seen;
        for (int y = 0; y < out->height(); y += 2)
            for (int x = 0; x < out->width(); x += 2)
                seen.insert(out->pixel(x, y));
        QCOMPARE(seen.size(), 8);
    }

    // Tint blends the cell's target color into the tile (issue #13).
    void generateTint()
    {
        QString blue = saveImage("tint-blue.png", solidImage(QSize(8, 8), QColor(0, 0, 200)));
        QImage base = solidImage(QSize(20, 20), QColor(200, 0, 0));

        auto centre = [&](double tint) {
            ImageProcessing processing;
            processing.processMosaicImages({blue});
            processing.processGrid(base, QSize(2, 2));
            processing.generateImage(base.size(), QSize(2, 2), 0, 0, tint);
            return processing.getOutputImage()->pixelColor(5, 5);
        };

        QVERIFY(similar(centre(0.0), QColor(0, 0, 200), 0));
        QVERIFY(similar(centre(0.5), QColor(100, 0, 100), 1));
        QVERIFY(similar(centre(1.0), QColor(200, 0, 0), 0));
    }

    // Two tiles with the same mean color but a different layout (issue #5): only the
    // color layout can tell which one fits a cell that is dark on the left.
    void layoutMatchingSeesStructure()
    {
        QImage leftRight = solidImage(QSize(16, 16), Qt::white);
        QImage topBottom = solidImage(QSize(16, 16), Qt::white);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x)
            {
                leftRight.setPixelColor(x, y, Qt::black);
                topBottom.setPixelColor(y, x, Qt::black);
            }

        // Sorted first, so it wins the tie in mean color matching.
        QString a = saveImage("layout-a-topbottom.png", topBottom);
        QString b = saveImage("layout-b-leftright.png", leftRight);

        QImage base = leftRight.scaled(40, 40);

        auto topRight = [&](Matching matching) {
            ImageProcessing processing;
            processing.processMosaicImages({a, b});
            processing.processGrid(base, QSize(1, 1));
            processing.generateImage(base.size(), QSize(1, 1), 0, 0, 0.0, matching);
            return processing.getOutputImage()->pixelColor(35, 5);
        };

        QVERIFY(similar(topRight(Matching::MeanColor), Qt::black, 0));   // the top-bottom tile
        QVERIFY(similar(topRight(Matching::ColorLayout), Qt::white, 0)); // the left-right tile
    }

    // Tiles are compared by the part that will be visible: the center of a wide tile.
    void layoutMatchingUsesVisiblePart()
    {
        QImage thirds(30, 10, QImage::Format_RGB32);
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 30; ++x)
                thirds.setPixelColor(x, y, x < 10 ? Qt::red : (x < 20 ? Qt::green : Qt::blue));

        QString wide = saveImage("visible-thirds.png", thirds);
        QString plain = saveImage("visible-darkgreen.png", solidImage(QSize(10, 10), QColor(0, 128, 0)));

        QImage base = solidImage(QSize(10, 10), Qt::green);

        auto centre = [&](Matching matching) {
            ImageProcessing processing;
            processing.processMosaicImages({wide, plain});
            processing.processGrid(base, QSize(1, 1));
            processing.generateImage(base.size(), QSize(1, 1), 0, 0, 0.0, matching);
            return processing.getOutputImage()->pixelColor(5, 5);
        };

        QVERIFY(similar(centre(Matching::MeanColor), QColor(0, 128, 0), 0)); // the mean of the whole wide tile is grey
        QVERIFY(similar(centre(Matching::ColorLayout), Qt::green, 0));      // its visible center is green
    }

    // Layouts loaded from the cache give the same mosaic as freshly analysed ones.
    void layoutsSurviveTheCache()
    {
        QString cacheFile = m_dir.filePath("layout-cache/tiles.sqlite");
        QList<QString> files;
        for (int i = 0; i < 6; ++i)
        {
            QImage tile = solidImage(QSize(12, 12), QColor::fromHsv(i * 60, 255, 255));
            for (int x = 0; x < 6; ++x)
                for (int y = 0; y < 12; ++y)
                    tile.setPixelColor(x, y, QColor::fromHsv(i * 60 + 30, 255, 120));
            files << saveImage(QString("layoutcache%1.png").arg(i), tile);
        }

        QImage base(60, 40, QImage::Format_RGB32);
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 60; ++x)
                base.setPixelColor(x, y, QColor::fromHsv(x * 6, 255, 100 + y * 3));

        auto generate = [&]() {
            ImageProcessing processing;
            processing.setCacheFile(cacheFile);
            processing.processMosaicImages(files);
            processing.processGrid(base, QSize(6, 4));
            processing.generateImage(base.size(), QSize(6, 4), 1);
            return qMakePair(processing.cacheHits(), *processing.getOutputImage());
        };

        auto fresh = generate();
        auto cached = generate();
        QCOMPARE(fresh.first, 0);
        QCOMPARE(cached.first, 6);
        QVERIFY(fresh.second == cached.second);
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
