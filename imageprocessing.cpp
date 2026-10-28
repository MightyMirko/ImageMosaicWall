#include "imageprocessing.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <limits>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSet>
#include <QtMath>
#include <exiv2/exiv2.hpp>
#include <exiv2/exv_conf.h>
#include <exiv2/version.hpp>

#ifndef EXIV2_TEST_VERSION
#    define EXIV2_TEST_VERSION(major,minor,patch) \
         ( EXIV2_VERSION >= EXIV2_MAKE_VERSION(major,minor,patch) )
#endif

#if EXIV2_TEST_VERSION(0,27,99)
#   define AutoPtr UniquePtr
#   define AnyError Error
#   define kerErrorMessage ErrorCode::kerErrorMessage
#endif

namespace
{

struct Candidate
{
    double distance;
    int image;

    // Ties are broken by image index, so the order never depends on chance.
    bool operator<(const Candidate &other) const
    {
        return distance < other.distance || (distance == other.distance && image < other.image);
    }
};

// How many of the best matching images are kept per cell. Only when all of them are
// already used nearby is the whole list searched again.
const int CandidatesPerCell = 64;

// Cells and the visible part of tiles are compared as a grid of this many colors per side.
const int CellLayoutSize = 4;
using LabLayout = std::array<ColorLab, CellLayoutSize * CellLayoutSize>;

TileLayout computeTileLayout(const QImage &image)
{
    // Smooth scaling averages the pixels; 8 x 8 blocks of 8 x 8 pixels each.
    const int blockPixels = 8;
    QImage small = image.scaled(TileLayoutSize * blockPixels, TileLayoutSize * blockPixels,
                                Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                       .convertToFormat(QImage::Format_ARGB32);
    TileLayout layout;

    for (int by = 0; by < TileLayoutSize; ++by)
    {
        for (int bx = 0; bx < TileLayoutSize; ++bx)
        {
            int r = 0, g = 0, b = 0;

            for (int y = 0; y < blockPixels; ++y)
            {
                const QRgb *line = reinterpret_cast<const QRgb *>(small.constScanLine(by * blockPixels + y));

                for (int x = 0; x < blockPixels; ++x)
                {
                    QRgb c = line[bx * blockPixels + x];
                    r += qRed(c);
                    g += qGreen(c);
                    b += qBlue(c);
                }
            }

            int i = (by * TileLayoutSize + bx) * 3;
            layout[i] = quint8(r / (blockPixels * blockPixels));
            layout[i + 1] = quint8(g / (blockPixels * blockPixels));
            layout[i + 2] = quint8(b / (blockPixels * blockPixels));
        }
    }

    return layout;
}

// The colors of the part of a tile that is visible in a cell: tiles are cropped around
// their center to the cell's aspect ratio. Resamples the stored 8 x 8 grid of the whole
// tile to a 4 x 4 grid of that part, weighting each block by its overlapping area.
LabLayout visibleTileLayout(const TileLayout &layout, float tileAspect, float cellAspect)
{
    double x0 = 0.0, x1 = 1.0, y0 = 0.0, y1 = 1.0;

    if (tileAspect > cellAspect)
    {
        double visible = cellAspect / tileAspect;
        x0 = (1.0 - visible) / 2.0;
        x1 = x0 + visible;
    }
    else if (tileAspect < cellAspect)
    {
        double visible = tileAspect / cellAspect;
        y0 = (1.0 - visible) / 2.0;
        y1 = y0 + visible;
    }

    LabLayout result;

    for (int v = 0; v < CellLayoutSize; ++v)
    {
        double ry0 = y0 + (y1 - y0) * v / CellLayoutSize;
        double ry1 = y0 + (y1 - y0) * (v + 1) / CellLayoutSize;

        for (int u = 0; u < CellLayoutSize; ++u)
        {
            double rx0 = x0 + (x1 - x0) * u / CellLayoutSize;
            double rx1 = x0 + (x1 - x0) * (u + 1) / CellLayoutSize;
            double r = 0.0, g = 0.0, b = 0.0, total = 0.0;

            for (int by = 0; by < TileLayoutSize; ++by)
            {
                double oy = std::min(ry1, double(by + 1) / TileLayoutSize) - std::max(ry0, double(by) / TileLayoutSize);
                if (oy <= 0.0)
                    continue;

                for (int bx = 0; bx < TileLayoutSize; ++bx)
                {
                    double ox = std::min(rx1, double(bx + 1) / TileLayoutSize) - std::max(rx0, double(bx) / TileLayoutSize);
                    if (ox <= 0.0)
                        continue;

                    int i = (by * TileLayoutSize + bx) * 3;
                    double weight = ox * oy;
                    r += layout[i] * weight;
                    g += layout[i + 1] * weight;
                    b += layout[i + 2] * weight;
                    total += weight;
                }
            }

            QColor color(qRound(r / total), qRound(g / total), qRound(b / total));
            result[v * CellLayoutSize + u] = toLab(color);
        }
    }

    return result;
}

// The colors of a 4 x 4 grid over one cell of the base image (ARGB32). Parts of the cell
// outside the image get the cell's mean color.
LabLayout cellLayout(const QImage &image, const QRect &cell, QColor mean)
{
    LabLayout result;
    ColorLab meanLab = toLab(mean);

    for (int v = 0; v < CellLayoutSize; ++v)
    {
        int top = cell.top() + cell.height() * v / CellLayoutSize;
        int bottom = std::min(image.height(), cell.top() + cell.height() * (v + 1) / CellLayoutSize);

        for (int u = 0; u < CellLayoutSize; ++u)
        {
            int left = cell.left() + cell.width() * u / CellLayoutSize;
            int right = std::min(image.width(), cell.left() + cell.width() * (u + 1) / CellLayoutSize);
            qint64 r = 0, g = 0, b = 0, count = 0;

            for (int y = top; y < bottom; ++y)
            {
                const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));

                for (int x = left; x < right; ++x)
                {
                    r += qRed(line[x]);
                    g += qGreen(line[x]);
                    b += qBlue(line[x]);
                    ++count;
                }
            }

            if (count > 0)
            {
                QColor color(int(r / count), int(g / count), int(b / count));
                result[v * CellLayoutSize + u] = toLab(color);
            }
            else
            {
                result[v * CellLayoutSize + u] = meanLab;
            }
        }
    }

    return result;
}

} // namespace

ImageProcessing::ImageProcessing(QObject *parent)
    : QObject(parent)
    , m_skipBackgroundProcess(false)
    , m_imageMeanMap()
{
    Exiv2::XmpParser::initialize();
    ::atexit(Exiv2::XmpParser::terminate);
}

ImageProcessing::~ImageProcessing()
{
    m_skipBackgroundProcess = true;
}

void ImageProcessing::processMosaicImages(const QList<QString> &imageList)
{
    m_imageMeanMap.clear();
    m_imageShapes.clear();
    m_newCacheEntries.clear();
    m_cacheHits = 0;

    // Only files that are new or changed since the last run are read again.
    TileCache cache(m_cacheFile.isEmpty() ? TileCache::defaultFileName() : m_cacheFile);
    calculateImageMeanMap(imageList, cache.load());
    cache.store(m_newCacheEntries);
    m_newCacheEntries.clear();

    // The search in generateImage() compares every image with every cell.
    // Convert the mean colors to Lab once here instead of for every cell, and keep
    // them in plain vectors (in the map's sorted order, so ties resolve as before).
    m_imagePaths.clear();
    m_imageLabs.clear();
    m_imageAspects.clear();
    m_imageLayouts.clear();
    m_imagePaths.reserve(m_imageMeanMap.size());
    m_imageLabs.reserve(m_imageMeanMap.size());
    m_imageAspects.reserve(m_imageMeanMap.size());
    m_imageLayouts.reserve(m_imageMeanMap.size());

    for (auto it = m_imageMeanMap.constBegin(); it != m_imageMeanMap.constEnd(); ++it)
    {
        QColor color = it.value();
        auto shape = m_imageShapes.value(it.key());
        m_imagePaths.push_back(it.key());
        m_imageLabs.push_back(toLab(color));
        m_imageAspects.push_back(shape.first);
        m_imageLayouts.push_back(shape.second);
    }
}

bool ImageProcessing::generateImage(QSize outputSize, QSize gridSize, int history, int maxUses, double tint,
                                    Matching matching)
{
    if (!isReady())
        return false;

    int numCells = gridSize.width() * gridSize.height();

    // The cell colors must have been calculated for this exact grid, otherwise
    // generateImage() reads past the end of m_gridColorMap.
    if (m_gridColorMap.size() != static_cast<size_t>(numCells))
        return false;

    m_outputSize = outputSize;

    m_outputImage = std::unique_ptr<QImage>(new QImage(outputSize, QImage::Format_ARGB32));
    m_outputImage->fill(Qt::transparent);

    // Worker threads write their cells straight into the pixel buffer. Calling
    // QImage::setPixel() from several threads is not safe, it detaches each time.
    m_outputBits = m_outputImage->bits();
    m_outputBytesPerLine = m_outputImage->bytesPerLine();

    QSize cellSize(qCeil(outputSize.width() / double(gridSize.width())),
                   qCeil(outputSize.height() / double(gridSize.height())));
    int gridWidth = gridSize.width();
    int numImages = int(m_imagePaths.size());

    std::vector<int> cells(numCells);
    std::iota(cells.begin(), cells.end(), 0);

    // 1. In parallel: the best matching images for every cell.

    std::vector<ColorLab> cellLabs(numCells);
    for (int cell = 0; cell < numCells; ++cell)
    {
        QColor color = m_gridColorMap[cell];
        cellLabs[cell] = toLab(color);
    }

    // With Matching::ColorLayout, cells and tiles are compared as 4 x 4 grids of colors,
    // so a tile that is dark on the left goes where the base image is dark on the left.
    // The tiles' grids are taken from the part that is visible at this cell size.
    bool layoutMatching = matching == Matching::ColorLayout && !m_baseImage.isNull();
    std::vector<LabLayout> cellLayouts;
    std::vector<LabLayout> tileLayouts;

    if (layoutMatching)
    {
        QSize baseCell(qCeil(m_baseImage.width() / double(gridWidth)), qCeil(m_baseImage.height() / double(gridSize.height())));
        cellLayouts.resize(numCells);

        QtConcurrent::blockingMap(cells, [&](int cell) {
            QRect rect(QPoint((cell % gridWidth) * baseCell.width(), (cell / gridWidth) * baseCell.height()), baseCell);
            cellLayouts[cell] = cellLayout(m_baseImage, rect, m_gridColorMap[cell]);
        });

        float cellAspect = float(cellSize.width()) / cellSize.height();
        tileLayouts.resize(numImages);

        for (int i = 0; i < numImages; ++i)
            tileLayouts[i] = visibleTileLayout(m_imageLayouts[i], m_imageAspects[i], cellAspect);
    }

    auto distance = [&](int cell, int image) {
        if (!layoutMatching)
            return calculateDistance(cellLabs[cell], m_imageLabs[image]);

        double sum = 0.0;
        for (int k = 0; k < CellLayoutSize * CellLayoutSize; ++k)
            sum += calculateDistance(cellLayouts[cell][k], tileLayouts[image][k]);

        return sum / (CellLayoutSize * CellLayoutSize);
    };

    std::vector<std::vector<Candidate>> candidates(numCells);

    QtConcurrent::blockingMap(cells, [&](int cell) {
        if (m_skipBackgroundProcess)
            return;

        // Keep a sorted list of the best ones while scanning. Once it is full, almost
        // every image is rejected by one comparison with the worst entry.
        std::vector<Candidate> &best = candidates[cell];
        best.reserve(CandidatesPerCell);

        for (int i = 0; i < numImages; ++i)
        {
            Candidate candidate{distance(cell, i), i};

            if (int(best.size()) == CandidatesPerCell)
            {
                if (!(candidate < best.back()))
                    continue;

                best.pop_back();
            }

            best.insert(std::upper_bound(best.begin(), best.end(), candidate), candidate);
        }
    });

    if (m_skipBackgroundProcess)
        return false;

    // 2. One cell after the other, so every choice sees all choices made before it.
    // Cells whose best match is worst go first: they depend most on their few good
    // images, which easier cells would otherwise take. No randomness is involved, so
    // the same settings always give the same mosaic, whatever the number of threads.

    std::vector<int> order = cells;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return candidates[a].front().distance > candidates[b].front().distance;
    });

    std::vector<int> assigned(numCells, -1);
    std::vector<int> blockedAt(numImages, -1);   // step at which an image was last seen nearby
    std::vector<int> uses(numImages, 0);

    auto underLimit = [&](int image) { return maxUses <= 0 || uses[image] < maxUses; };

    // Best image in the whole list that passes the given test, or -1.
    auto bestOf = [&](int cell, auto &&allowed) {
        int chosen = -1;
        double best = std::numeric_limits<double>::max();

        for (int i = 0; i < numImages; ++i)
        {
            if (!allowed(i))
                continue;

            double d = distance(cell, i);

            if (d < best)
            {
                best = d;
                chosen = i;
            }
        }

        return chosen;
    };

    for (int step = 0; step < numCells; ++step)
    {
        int cell = order[step];
        int cx = cell % gridWidth;
        int cy = cell / gridWidth;

        // Images already used within the variation radius are not allowed here.
        for (int y = qMax(0, cy - history); y <= qMin(gridSize.height() - 1, cy + history); ++y)
        {
            for (int x = qMax(0, cx - history); x <= qMin(gridWidth - 1, cx + history); ++x)
            {
                int used = assigned[y * gridWidth + x];
                if (used >= 0)
                    blockedAt[used] = step;
            }
        }

        auto notNearby = [&](int image) { return blockedAt[image] != step; };
        int chosen = -1;

        for (const Candidate &candidate : candidates[cell])
        {
            if (notNearby(candidate.image) && underLimit(candidate.image))
            {
                chosen = candidate.image;
                break;
            }
        }

        // All close candidates are used nearby or used up: look through all images.
        if (chosen < 0)
            chosen = bestOf(cell, [&](int i) { return notNearby(i) && underLimit(i); });

        // There are more cells than the rules allow (small folder, high variation or a low
        // use limit). Rather repeat an image nearby than exceed the limit, and rather exceed
        // the limit than leave the cell empty.
        if (chosen < 0 && maxUses > 0)
            chosen = bestOf(cell, underLimit);

        if (chosen < 0)
            chosen = bestOf(cell, notNearby);

        if (chosen < 0)
            chosen = candidates[cell].front().image;

        assigned[cell] = chosen;
        ++uses[chosen];
    }

    // 3. In parallel: load, crop and draw the tiles. Cells never overlap.

    QtConcurrent::blockingMap(cells, [&](int cell) {
        if (!m_skipBackgroundProcess)
            renderCell(cell, m_imagePaths[assigned[cell]], cellSize, gridWidth, tint);
    });

    return !m_skipBackgroundProcess;
}

void ImageProcessing::calculateGridCellsMean(const QImage &baseImage,
                                             const QSize &gridSize,
                                             std::vector<QColor> &colorMap,
                                             int pos,
                                             int numCells)
{
    int nx = 0, ny = 0, g = pos;
    double gx = ceil(baseImage.width() / static_cast<float>(gridSize.width()));
    double gy = ceil(baseImage.height() / static_cast<float>(gridSize.height()));

    QSize gridCellSize(floor(gx), floor(gy));

    for (; g < pos + numCells; ++g)
    {
        // Cells at the right and bottom edge can be partly outside the image, so only
        // the pixels actually summed are counted. 64 bit sums, large cells overflow int.
        qint64 red = 0, green = 0, blue = 0;
        qint64 numberOfPixel = 0;
        gx = (g % gridSize.width()) * gridCellSize.width();
        gy = g / gridSize.width() * gridCellSize.height();

        for (int y = 0; y < gridCellSize.height(); ++y)
        {
            ny = y + gy;
            if (ny >= baseImage.height() || ny < 0)
                continue;

            // processGrid() passes the image as Format_ARGB32, so a scan line is an array of QRgb.
            const QRgb *line = reinterpret_cast<const QRgb *>(baseImage.constScanLine(ny));

            for (int x = 0; x < gridCellSize.width(); ++x)
            {
                nx = x + gx;

                if (nx < 0 || nx >= baseImage.width())
                    continue;

                red += qRed(line[nx]);
                green += qGreen(line[nx]);
                blue += qBlue(line[nx]);
                ++numberOfPixel;
            }
        }

        // A cell entirely outside the image is never rendered, it just stays black.
        if (numberOfPixel > 0)
        {
            red /= numberOfPixel;
            green /= numberOfPixel;
            blue /= numberOfPixel;
        }

        colorMap.at(g) = QColor(red, green, blue);
        emit cellProcessed(g);
    }
}

void ImageProcessing::calculateImageMeanMap(const QList<QString> imageList,
                                            const QHash<QString, TileCache::Entry> &cached)
{
    std::function<void(const QString)> scale = [&](const QString imageFileName) {
        if (m_skipBackgroundProcess)
            return;

        QFileInfo info(imageFileName);
        qint64 size = info.size();
        qint64 mtime = info.lastModified().toMSecsSinceEpoch();

        auto hit = cached.constFind(imageFileName);

        if (hit != cached.constEnd() && hit->size == size && hit->mtime == mtime &&
            hit->layout.size() == int(sizeof(TileLayout)) && hit->aspect > 0.0f)
        {
            TileLayout layout;
            std::memcpy(layout.data(), hit->layout.constData(), sizeof(TileLayout));

            {
                QMutexLocker lock(&m_lockMean);
                m_imageMeanMap.insert(imageFileName, QColor(hit->color));
                m_imageShapes.insert(imageFileName, qMakePair(hit->aspect, layout));
            }

            ++m_cacheHits;
            emit imageProcessed(imageFileName);
            return;
        }

        QImage image = extractThumbnail(imageFileName, QSize(128, 128));
        bool fromPreview = !image.isNull();

        if (!fromPreview)
            image = loadOrientedImage(imageFileName, QSize(128, 128));

        if (image.isNull())
            return;

        float aspect = float(image.width()) / image.height();
        TileLayout layout = computeTileLayout(image);

        // The mean color is computed as before: on the embedded preview, or on a
        // 128 x 128 sample of the file.
        if (!fromPreview)
            image = image.scaled(QSize(128, 128), Qt::IgnoreAspectRatio, Qt::FastTransformation);

        // Reading QRgb values from scan lines is much faster than a QColor per pixel.
        // ARGB32 keeps the same 8 bit color values pixelColor() returned for any format.
        image = image.convertToFormat(QImage::Format_ARGB32);

        uint64_t meanR = 0;
        uint64_t meanG = 0;
        uint64_t meanB = 0;

        for (int y = 0; y < image.height(); ++y)
        {
            const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));

            for (int x = 0; x < image.width(); ++x)
            {
                meanR += qRed(line[x]);
                meanG += qGreen(line[x]);
                meanB += qBlue(line[x]);
            }
        }

        auto dim = image.width() * image.height();
        meanR /= dim;
        meanG /= dim;
        meanB /= dim;

        if (m_skipBackgroundProcess)
            return;
        {
            QMutexLocker lock(&m_lockMean);
            auto color = QColor(meanR, meanG, meanB);
            m_imageMeanMap.insert(imageFileName, color);
            m_imageShapes.insert(imageFileName, qMakePair(aspect, layout));
            m_newCacheEntries.append(
                {imageFileName,
                 TileCache::Entry{size, mtime, color.rgb(), aspect,
                                  QByteArray(reinterpret_cast<const char *>(layout.data()), int(layout.size()))}});
        }
        emit imageProcessed(imageFileName);
    };

    QtConcurrent::map(imageList, scale).waitForFinished();
}

void ImageProcessing::renderCell(int cell, const QString &imagePath, QSize cellSize, int gridWidth, double tint)
{
    GridPoint p(cell % gridWidth, cell / gridWidth);

    QImage image = extractThumbnail(imagePath, cellSize);
    if (image.isNull())
        image = loadOrientedImage(imagePath, cellSize);

    // Fill the cell and crop the overflow evenly from both sides, keeping the center.
    QImage scaled = image.scaled(cellSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPoint cropOrigin((scaled.width() - cellSize.width()) / 2, (scaled.height() - cellSize.height()) / 2);
    QImage cellImage = scaled.copy(QRect(cropOrigin, cellSize)).convertToFormat(QImage::Format_ARGB32);

    // Blend the cell's color in the base image into the tile, as the mosaic reads better
    // from a distance: pixel = (1 - tint) * pixel + tint * target.
    if (tint > 0.0)
    {
        int strength = qBound(0, qRound(tint * 256), 256);
        QRgb target = m_gridColorMap[cell].rgb();
        int tr = qRed(target) * strength;
        int tg = qGreen(target) * strength;
        int tb = qBlue(target) * strength;

        for (int y = 0; y < cellImage.height(); ++y)
        {
            QRgb *line = reinterpret_cast<QRgb *>(cellImage.scanLine(y));

            for (int x = 0; x < cellImage.width(); ++x)
            {
                QRgb c = line[x];
                line[x] = qRgba((qRed(c) * (256 - strength) + tr) >> 8,
                                (qGreen(c) * (256 - strength) + tg) >> 8,
                                (qBlue(c) * (256 - strength) + tb) >> 8,
                                qAlpha(c));
            }
        }
    }

    emit mosaicGenerated(p);

    // Each cell is a separate region of the buffer, so threads never write the same pixels.
    int x0 = p.x() * cellSize.width();
    int y0 = p.y() * cellSize.height();
    int w = qMin(cellImage.width(), m_outputSize.width() - x0);
    int h = qMin(cellImage.height(), m_outputSize.height() - y0);

    // With a rounded-up cell size the last rows/columns can start past the image edge.
    if (w <= 0 || h <= 0)
        return;

    for (int y = 0; y < h; ++y)
    {
        memcpy(m_outputBits + (y0 + y) * m_outputBytesPerLine + x0 * 4, cellImage.constScanLine(y), w * 4);
    }
}

double ImageProcessing::calculateDistance(QColor rhs, QColor lhs) const
{
    return abs(rhs.redF() - lhs.redF()) + abs(rhs.greenF() - lhs.greenF()) + abs(rhs.blueF() - lhs.blueF());
}

double ImageProcessing::calculateDistance(ColorLab rhs, ColorLab lhs) const
{
    return abs(rhs.L - lhs.L) + abs(rhs.a - lhs.a) + abs(rhs.b - lhs.b);
}

QMap<QString, QColor> ImageProcessing::getImageMeanMap() const
{
    return m_imageMeanMap;
}

void ImageProcessing::setCacheFile(const QString &fileName)
{
    m_cacheFile = fileName;
}

int ImageProcessing::cacheHits() const
{
    return m_cacheHits;
}

bool ImageProcessing::skipBackgroundProcesses() const
{
    return m_skipBackgroundProcess;
}

void ImageProcessing::processCanceled(bool canceled)
{
    m_skipBackgroundProcess = canceled;
}

const QImage *ImageProcessing::getOutputImage() const
{
    return m_outputImage.get();
}

void ImageProcessing::processGrid(const QImage &baseImage, QSize gridSize)
{
    int maxThreads = QThreadPool::globalInstance()->maxThreadCount();
    int numCells = gridSize.width() * gridSize.height();
    int itemsPerThread = numCells / maxThreads;
    QThreadPool pool;

    m_gridColorMap.resize(numCells);

    // Converted once here for all threads; calculateGridCellsMean() reads ARGB32 scan lines.
    QImage argbImage = baseImage.convertToFormat(QImage::Format_ARGB32);
    m_baseImage = argbImage;

    QList<QFuture<void>> jobs;
    int numCellsPerThread = itemsPerThread;
    for (int t = 0; t < maxThreads; ++t)
    {
        if (t == (maxThreads - 1))
        {
            numCellsPerThread += numCells % maxThreads;
        }

        std::function<void()> meanFnc = [=]() {
            this->calculateGridCellsMean(argbImage, gridSize, m_gridColorMap, t * itemsPerThread, numCellsPerThread);
        };

        jobs << QtConcurrent::run(&pool, meanFnc);
    }

    for (auto &job : jobs)
        job.waitForFinished();
}

std::vector<QColor> ImageProcessing::getGridColorMap() const
{
    return m_gridColorMap;
}

bool ImageProcessing::isReady()
{
    return !(m_gridColorMap.size() == 0 || m_imageMeanMap.size() == 0);
}

QImage applyExifOrientation(const QImage &image, int orientation)
{
    // Maps the stored image to its display orientation, see the Exif 2.3 spec, tag 0x0112.
    // QTransform(m11, m12, m21, m22, dx, dy): x' = m11 * x + m21 * y, y' = m12 * x + m22 * y
    QTransform transform;

    switch (orientation)
    {
        case 2: // mirror horizontally
            transform = QTransform(-1, 0, 0, 1, 0, 0);
            break;
        case 3: // rotate 180°
            transform = QTransform(-1, 0, 0, -1, 0, 0);
            break;
        case 4: // mirror vertically
            transform = QTransform(1, 0, 0, -1, 0, 0);
            break;
        case 5: // transpose
            transform = QTransform(0, 1, 1, 0, 0, 0);
            break;
        case 6: // rotate 90° clockwise
            transform = QTransform(0, 1, -1, 0, 0, 0);
            break;
        case 7: // transverse
            transform = QTransform(0, -1, -1, 0, 0, 0);
            break;
        case 8: // rotate 90° counter-clockwise
            transform = QTransform(0, -1, 1, 0, 0, 0);
            break;
        default:
            return image;
    }

    return image.transformed(transform);
}

QImage loadOrientedImage(const QString &filename, QSize minSize)
{
    QImageReader reader(filename);
    reader.setAutoTransform(true);

    // Decode at the smallest size that still covers minSize. For JPEG this lets the
    // decoder skip most of the work (DCT scaling), much faster than decoding a camera
    // photo at full size and scaling it down afterwards.
    QSize size = reader.size();

    if (!minSize.isEmpty() && size.isValid())
    {
        // The scaled size applies to the image as stored, before the Exif rotation.
        if (reader.transformation() & QImageIOHandler::TransformationRotate90)
            minSize.transpose();

        double scale = qMax(double(minSize.width()) / size.width(), double(minSize.height()) / size.height());

        if (scale < 1.0)
            reader.setScaledSize(QSize(qCeil(size.width() * scale), qCeil(size.height() * scale)));
    }

    return reader.read();
}

namespace
{

// Camera RAW formats (as reported by Qt's image format plugins, e.g. kimageformats).
const QSet<QString> &rawSuffixes()
{
    static const QSet<QString> suffixes = {
        "3fr", "arw", "cr2", "cr3", "crw", "dcr", "dng", "erf", "fff", "iiq", "k25", "kdc", "mdc", "mef",
        "mos", "mrw", "nef", "nrw", "orf", "pef", "raf", "raw", "rw2", "rwl", "sr2", "srf", "srw", "x3f"};
    return suffixes;
}

} // namespace

QStringList supportedImageNameFilters()
{
    // Formats Qt can read that are no photos: vector graphics, icons and documents.
    static const QSet<QByteArray> excluded = {"svg", "svgz", "ico", "cur", "ani", "icns", "pdf", "eps", "epsf", "epsi"};

    QStringList filters;

    for (const QByteArray &format : QImageReader::supportedImageFormats())
    {
        QByteArray lower = format.toLower();

        if (!excluded.contains(lower))
            filters << QStringLiteral("*.") + QString::fromLatin1(lower);
    }

    filters.removeDuplicates();
    filters.sort();
    return filters;
}

QList<QString> applyRawFilesPolicy(const QList<QString> &files, RawFiles policy)
{
    switch (policy)
    {
        case RawFiles::Always:
            return files;

        case RawFiles::Never:
        {
            QList<QString> result;

            for (const QString &file : files)
                if (!rawSuffixes().contains(QFileInfo(file).suffix().toLower()))
                    result << file;

            return result;
        }

        case RawFiles::WithoutCopies:
        default:
            return withoutRawDuplicates(files);
    }
}

QList<QString> withoutRawDuplicates(const QList<QString> &files)
{
    // Photographers often keep a RAW file next to a JPEG (or HEIC, ...) of the same shot,
    // which would put every photo into the mosaic twice. Keep the RAW file only when
    // there is no other file with the same name in the same folder.
    QSet<QString> nonRawStems;

    for (const QString &file : files)
    {
        QFileInfo info(file);

        if (!rawSuffixes().contains(info.suffix().toLower()))
            nonRawStems.insert(info.dir().filePath(info.completeBaseName()).toLower());
    }

    QList<QString> result;

    for (const QString &file : files)
    {
        QFileInfo info(file);
        bool isRaw = rawSuffixes().contains(info.suffix().toLower());

        if (!isRaw || !nonRawStems.contains(info.dir().filePath(info.completeBaseName()).toLower()))
            result << file;
    }

    return result;
}

QString suggestedMosaicFileName(const QString &baseImagePath)
{
    if (baseImagePath.isEmpty())
        return QDir::home().filePath("mosaic.png");

    // Next to the base image, but never the base image itself: "IMG_1234_mosaic.png",
    // then "IMG_1234_mosaic_2.png" and so on if that file already exists.
    QFileInfo base(baseImagePath);
    QString stem = base.completeBaseName() + "_mosaic";
    QString candidate = base.dir().filePath(stem + ".png");

    for (int n = 2; QFileInfo::exists(candidate); ++n)
        candidate = base.dir().filePath(QString("%1_%2.png").arg(stem).arg(n));

    return candidate;
}

QImage extractThumbnail(const QString filename, QSize minSize)
{
    try
    {
        Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(filename.toStdString());

        if (image.get() == nullptr || !image.get()->good())
            return QImage();

        image->readMetadata();

        Exiv2::PreviewManager loader(*image);
        Exiv2::PreviewPropertiesList list = loader.getPreviewProperties();

        if (list.empty())
            return QImage();

        QSize bestSize(-1, -1);

        Exiv2::PreviewProperties previewProperty;
        for (const auto &previewItem : list)
        {
            if (previewItem.width_ < minSize.width() || previewItem.height_ < minSize.height())
                continue;

            if (previewItem.width_ < bestSize.width() || previewItem.height_ < bestSize.height())
            {
                previewProperty = previewItem;
                bestSize.setWidth(previewItem.width_);
                bestSize.setHeight(previewItem.height_);
            }
        }

        Exiv2::PreviewImage preview = loader.getPreviewImage(previewProperty);
        QImage thumbnail = QImage::fromData(preview.pData(), preview.size());

        // Embedded previews are stored like the main image, so the Exif orientation
        // of the file applies to them as well.
        Exiv2::ExifData &exifData = image->exifData();
        auto it = exifData.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));

        if (it != exifData.end() && it->count() > 0)
        {

#if EXIV2_TEST_VERSION(0,28,0)

            int orientation = static_cast<int>(it->toInt64());

#else

            int orientation = static_cast<int>(it->toLong());

#endif

            thumbnail = applyExifOrientation(thumbnail, orientation);
        }

        return thumbnail;
    }
    catch (Exiv2::Error &err)
    {
        qDebug() << "Error: " << err.what();
        return QImage();
    }
}
