#include "imageprocessing.h"
#include "tiffwriter.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <numeric>
#include <queue>
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

double labDistance(const ColorLab &x, const ColorLab &y)
{
    return std::abs(x.L - y.L) + std::abs(x.a - y.a) + std::abs(x.b - y.b);
}

// The average of a layout's colors. The distance of two layouts is never smaller
// than the distance of their averages, which makes it a cheap lower bound.
ColorLab averageColor(const LabLayout &layout)
{
    ColorLab sum{0.0, 0.0, 0.0};

    for (const ColorLab &c : layout)
    {
        sum.L += c.L;
        sum.a += c.a;
        sum.b += c.b;
    }

    int n = int(layout.size());
    return ColorLab{sum.L / n, sum.a / n, sum.b / n};
}

// Searches items for the count best matches of one query, best first; equal to
// computing distance() for every item and keeping the count smallest, but faster:
// items are visited outwards from the query's lightness in byLightness (indexes
// sorted by keys[i].L), and an item whose key is already further away from the
// query's key than the worst kept match is skipped without computing distance().
// That requires distance(i) >= labDistance(query, keys[i]) for every item.
template <typename Distance>
std::vector<Candidate> bestMatches(int count, const ColorLab &query, const std::vector<ColorLab> &keys,
                                   const std::vector<int> &byLightness, Distance &&distance)
{
    // Rounding can make a bound a tiny bit larger than the distance it bounds.
    const double tolerance = 1e-9;

    std::vector<Candidate> best;
    best.reserve(count + 1);

    auto full = [&]() { return int(best.size()) == count; };

    auto consider = [&](int item) {
        if (full() && labDistance(query, keys[item]) > best.back().distance + tolerance)
            return;

        Candidate candidate{distance(item), item};

        if (full())
        {
            if (!(candidate < best.back()))
                return;

            best.pop_back();
        }

        best.insert(std::upper_bound(best.begin(), best.end(), candidate), candidate);
    };

    int n = int(byLightness.size());
    auto above = std::lower_bound(byLightness.begin(), byLightness.end(), query.L,
                                  [&](int item, double L) { return keys[item].L < L; });
    int up = int(above - byLightness.begin());
    int down = up - 1;

    // The lightness difference alone is a lower bound too; once it exceeds the worst
    // kept match in one direction, everything further in that direction does as well.
    while (up < n || down >= 0)
    {
        double upGap = up < n ? keys[byLightness[up]].L - query.L : std::numeric_limits<double>::max();
        double downGap = down >= 0 ? query.L - keys[byLightness[down]].L : std::numeric_limits<double>::max();
        double gap = std::min(upGap, downGap);

        if (full() && gap > best.back().distance + tolerance)
            break;

        if (upGap <= downGap)
            consider(byLightness[up++]);
        else
            consider(byLightness[down--]);
    }

    return best;
}

// One cell of the mosaic: where it is drawn in the output and which part of the base
// image it shows, the slot of the grid it belongs to, and the group of images it takes.
struct MosaicCell
{
    QRect out;
    QRect base;
    int slot;
    int group;
};

// The mean color of the part of rect inside image (ARGB32); black if there is none.
QColor meanColor(const QImage &image, const QRect &rect)
{
    QRect inside = rect.intersected(image.rect());
    qint64 r = 0, g = 0, b = 0, count = 0;

    for (int y = inside.top(); y <= inside.bottom(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));

        for (int x = inside.left(); x <= inside.right(); ++x)
        {
            r += qRed(line[x]);
            g += qGreen(line[x]);
            b += qBlue(line[x]);
            ++count;
        }
    }

    if (count == 0)
        return QColor(Qt::black);

    return QColor(int(r / count), int(g / count), int(b / count));
}

// Item indexes sorted by the lightness of their keys.
std::vector<int> sortedByLightness(const std::vector<ColorLab> &keys)
{
    std::vector<int> order(keys.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return keys[a].L < keys[b].L; });
    return order;
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
                                    Matching matching, bool useEveryImage, Pattern pattern)
{
    if (!isReady())
        return false;

    int numSlots = gridSize.width() * gridSize.height();

    // The cell colors must have been calculated for this exact grid, otherwise
    // generateImage() reads past the end of m_gridColorMap.
    if (m_gridColorMap.size() != static_cast<size_t>(numSlots))
        return false;

    m_outputImage.reset();
    m_placedCells.clear();

    int gridWidth = gridSize.width();
    int numImages = int(m_imagePaths.size());
    bool parquet = pattern == Pattern::Parquet;

    // The grid divides the output into slots. A grid slot is one cell; a parquet slot
    // can be one portrait cell or two landscape cells, so all three are set up here
    // and the slot's cells are chosen once the matches are known.

    QSize slotSize(qCeil(outputSize.width() / double(gridWidth)), qCeil(outputSize.height() / double(gridSize.height())));
    QSize baseSlot(qCeil(m_baseImage.width() / double(gridWidth)), qCeil(m_baseImage.height() / double(gridSize.height())));

    // Images can only go into cells of their group: all images in the grid, landscape
    // images (group 0) and portrait images (group 1) in the parquet pattern.
    std::vector<int> imageGroup(numImages, 0);
    if (parquet)
        for (int i = 0; i < numImages; ++i)
            imageGroup[i] = m_imageAspects[i] < 1.0f ? 1 : 0;

    // The size tiles of a group are loaded at; cells at the edge of a stacked pair
    // can be a pixel smaller and are cropped when drawn.
    QSize groupTileSize[2] = {slotSize, slotSize};
    if (parquet)
        groupTileSize[0] = QSize(slotSize.width(), qCeil(slotSize.height() / 2.0));

    std::vector<MosaicCell> cells;
    cells.reserve(parquet ? 3 * numSlots : numSlots);

    for (int slot = 0; slot < numSlots; ++slot)
    {
        QPoint outOrigin((slot % gridWidth) * slotSize.width(), (slot / gridWidth) * slotSize.height());
        QPoint baseOrigin((slot % gridWidth) * baseSlot.width(), (slot / gridWidth) * baseSlot.height());

        if (!parquet)
        {
            cells.push_back({QRect(outOrigin, slotSize), QRect(baseOrigin, baseSlot), slot, 0});
            continue;
        }

        int outTop = slotSize.height() / 2;
        int baseTop = baseSlot.height() / 2;

        cells.push_back({QRect(outOrigin, slotSize), QRect(baseOrigin, baseSlot), slot, 1});
        cells.push_back({QRect(outOrigin, QSize(slotSize.width(), outTop)),
                         QRect(baseOrigin, QSize(baseSlot.width(), baseTop)), slot, 0});
        cells.push_back({QRect(outOrigin + QPoint(0, outTop), QSize(slotSize.width(), slotSize.height() - outTop)),
                         QRect(baseOrigin + QPoint(0, baseTop), QSize(baseSlot.width(), baseSlot.height() - baseTop)),
                         slot, 0});
    }

    int numCells = int(cells.size());
    std::vector<int> cellIndexes(numCells);
    std::iota(cellIndexes.begin(), cellIndexes.end(), 0);

    // 1. In parallel: the best matching images for every cell.

    // The target color of each cell; for the grid the one calculated by processGrid().
    std::vector<QColor> cellColors(numCells);
    QtConcurrent::blockingMap(cellIndexes, [&](int cell) {
        cellColors[cell] = parquet ? meanColor(m_baseImage, cells[cell].base) : m_gridColorMap[cells[cell].slot];
    });

    std::vector<ColorLab> cellLabs(numCells);
    for (int cell = 0; cell < numCells; ++cell)
        cellLabs[cell] = toLab(cellColors[cell]);

    // With Matching::ColorLayout, cells and tiles are compared as 4 x 4 grids of colors,
    // so a tile that is dark on the left goes where the base image is dark on the left.
    // The tiles' grids are taken from the part that is visible in the cells of their group.
    bool layoutMatching = matching == Matching::ColorLayout && !m_baseImage.isNull();
    std::vector<LabLayout> cellLayouts;
    std::vector<LabLayout> tileLayouts;

    if (layoutMatching)
    {
        cellLayouts.resize(numCells);

        QtConcurrent::blockingMap(cellIndexes, [&](int cell) {
            cellLayouts[cell] = cellLayout(m_baseImage, cells[cell].base, cellColors[cell]);
        });

        tileLayouts.resize(numImages);

        for (int i = 0; i < numImages; ++i)
        {
            QSize tileSize = groupTileSize[imageGroup[i]];
            tileLayouts[i] = visibleTileLayout(m_imageLayouts[i], m_imageAspects[i],
                                               float(tileSize.width()) / tileSize.height());
        }
    }

    auto distance = [&](int cell, int image) {
        if (!layoutMatching)
            return calculateDistance(cellLabs[cell], m_imageLabs[image]);

        double sum = 0.0;
        for (int k = 0; k < CellLayoutSize * CellLayoutSize; ++k)
            sum += calculateDistance(cellLayouts[cell][k], tileLayouts[image][k]);

        return sum / (CellLayoutSize * CellLayoutSize);
    };

    // One color per cell and image that bounds the distance from below: the mean color
    // itself, or with layouts the average of the layout's colors. See bestMatches().
    std::vector<ColorLab> cellKeys(numCells);
    std::vector<ColorLab> imageKeys(numImages);

    for (int cell = 0; cell < numCells; ++cell)
        cellKeys[cell] = layoutMatching ? averageColor(cellLayouts[cell]) : cellLabs[cell];

    for (int i = 0; i < numImages; ++i)
        imageKeys[i] = layoutMatching ? averageColor(tileLayouts[i]) : m_imageLabs[i];

    std::vector<int> groupImages[2];
    for (int image : sortedByLightness(imageKeys))
        groupImages[imageGroup[image]].push_back(image);

    std::vector<std::vector<Candidate>> candidates(numCells);

    QtConcurrent::blockingMap(cellIndexes, [&](int cell) {
        if (!m_skipBackgroundProcess)
            candidates[cell] = bestMatches(CandidatesPerCell, cellKeys[cell], imageKeys, groupImages[cells[cell].group],
                                           [&](int image) { return distance(cell, image); });
    });

    if (m_skipBackgroundProcess)
        return false;

    // Parquet: choose for every slot between one portrait and two landscape cells.
    // How many slots get a portrait follows the share of portrait images, so images of
    // both orientations are used about equally often; which ones, follows where a
    // portrait image fits best compared to the two landscape images it replaces.
    std::vector<bool> active(numCells, true);

    if (parquet)
    {
        double portraits = double(groupImages[1].size());
        double landscapes = double(groupImages[0].size());
        int portraitSlots = qRound(numSlots * 2.0 * portraits / (landscapes + 2.0 * portraits));

        auto bestDistance = [&](int cell) {
            return candidates[cell].empty() ? std::numeric_limits<double>::max() : candidates[cell].front().distance;
        };

        std::vector<double> advantage(numSlots);
        for (int slot = 0; slot < numSlots; ++slot)
        {
            double portrait = bestDistance(3 * slot);
            double pair = (bestDistance(3 * slot + 1) + bestDistance(3 * slot + 2)) / 2.0;
            advantage[slot] = pair - portrait;
        }

        // Slots are chosen one after another, best advantage first. A slot loses a little
        // for every neighbour that already holds a portrait, so in even areas portraits
        // and pairs alternate like parquet instead of filling whole rows; clear
        // differences in the base image still decide.
        std::vector<double> finite;
        for (double a : advantage)
            if (std::isfinite(a))
                finite.push_back(a);

        double penalty = 1.0;
        if (finite.size() >= 4)
        {
            std::sort(finite.begin(), finite.end());
            double spread = finite[finite.size() * 3 / 4] - finite[finite.size() / 4];
            if (spread > 0.0)
                penalty = spread / 2.0;
        }

        int gridHeight = gridSize.height();
        std::vector<int> portraitNeighbours(numSlots, 0);
        std::vector<bool> portrait(numSlots, false);

        // Highest score first; equal scores in slot order.
        struct Entry
        {
            double score;
            int slot;
            int neighbours;   // portraitNeighbours[slot] when the score was computed
            bool operator<(const Entry &other) const
            {
                return score < other.score || (score == other.score && slot > other.slot);
            }
        };

        std::priority_queue<Entry> queue;
        for (int slot = 0; slot < numSlots; ++slot)
            queue.push({advantage[slot], slot, 0});

        for (int chosen = 0; chosen < portraitSlots && !queue.empty();)
        {
            Entry entry = queue.top();
            queue.pop();

            // Scores only go down, so an outdated entry is recomputed and queued again.
            if (entry.neighbours != portraitNeighbours[entry.slot])
            {
                int n = portraitNeighbours[entry.slot];
                queue.push({advantage[entry.slot] - penalty * n, entry.slot, n});
                continue;
            }

            portrait[entry.slot] = true;
            ++chosen;

            int x = entry.slot % gridWidth;
            int y = entry.slot / gridWidth;
            if (x > 0)
                ++portraitNeighbours[entry.slot - 1];
            if (x < gridWidth - 1)
                ++portraitNeighbours[entry.slot + 1];
            if (y > 0)
                ++portraitNeighbours[entry.slot - gridWidth];
            if (y < gridHeight - 1)
                ++portraitNeighbours[entry.slot + gridWidth];
        }

        for (int slot = 0; slot < numSlots; ++slot)
        {
            active[3 * slot] = portrait[slot];
            active[3 * slot + 1] = !portrait[slot];
            active[3 * slot + 2] = !portrait[slot];
        }
    }

    // The cells of every slot that are used, for the neighbourhood of variation.
    std::vector<std::vector<int>> slotCells(numSlots);
    std::vector<int> activeCells;

    for (int cell = 0; cell < numCells; ++cell)
    {
        if (!active[cell])
            continue;

        slotCells[cells[cell].slot].push_back(cell);
        activeCells.push_back(cell);
    }

    // 2. One cell after the other, so every choice sees all choices made before it.
    // Cells whose best match is worst go first: they depend most on their few good
    // images, which easier cells would otherwise take. No randomness is involved, so
    // the same settings always give the same mosaic, whatever the number of threads.

    std::vector<int> order = activeCells;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return candidates[a].front().distance > candidates[b].front().distance;
    });

    std::vector<int> assigned(numCells, -1);
    std::vector<int> blockedAt(numImages, -1);   // step at which an image was last seen nearby
    std::vector<int> uses(numImages, 0);

    // Images that go into cells of a group, per group.
    std::vector<int> groupCells[2];
    for (int cell : activeCells)
        groupCells[cells[cell].group].push_back(cell);

    // With more images than cells, not every image can be used; each one is then
    // used at most once instead.
    for (int group = 0; group < 2; ++group)
    {
        if (useEveryImage && groupImages[group].size() > groupCells[group].size())
        {
            useEveryImage = false;
            maxUses = 1;
        }
    }

    auto underLimit = [&](int image) { return maxUses <= 0 || uses[image] < maxUses; };

    // Every image first gets the free cell it fits best; the remaining cells are
    // filled as usual below. Images that fit nowhere well go first, so they don't
    // end up with leftovers, and they take few cells away from the others.
    if (useEveryImage)
    {
        std::vector<int> images(numImages);
        std::iota(images.begin(), images.end(), 0);
        std::vector<std::vector<Candidate>> bestCells(numImages);   // Candidate::image is a cell here

        std::vector<int> cellsByLightness[2];
        for (int cell : sortedByLightness(cellKeys))
            if (active[cell])
                cellsByLightness[cells[cell].group].push_back(cell);

        QtConcurrent::blockingMap(images, [&](int image) {
            if (!m_skipBackgroundProcess)
                bestCells[image] = bestMatches(CandidatesPerCell, imageKeys[image], cellKeys,
                                               cellsByLightness[imageGroup[image]],
                                               [&](int cell) { return distance(cell, image); });
        });

        if (m_skipBackgroundProcess)
            return false;

        std::stable_sort(images.begin(), images.end(), [&](int a, int b) {
            return bestCells[a].front().distance > bestCells[b].front().distance;
        });

        for (int image : images)
        {
            int chosen = -1;

            for (const Candidate &candidate : bestCells[image])
            {
                if (assigned[candidate.image] < 0)
                {
                    chosen = candidate.image;
                    break;
                }
            }

            // All close cells are taken: the best free one of the whole grid. There is
            // one, since there are at least as many cells as images in every group.
            if (chosen < 0)
            {
                double best = std::numeric_limits<double>::max();

                for (int cell : groupCells[imageGroup[image]])
                {
                    if (assigned[cell] >= 0)
                        continue;

                    double d = distance(cell, image);

                    if (d < best)
                    {
                        best = d;
                        chosen = cell;
                    }
                }
            }

            assigned[chosen] = image;
            ++uses[image];
        }
    }

    // The images of each group in index order, so ties resolve to the lowest index.
    std::vector<int> groupImagesByIndex[2];
    for (int image = 0; image < numImages; ++image)
        groupImagesByIndex[imageGroup[image]].push_back(image);

    // Best image of the cell's group that passes the given test, or -1.
    auto bestOf = [&](int cell, auto &&allowed) {
        int chosen = -1;
        double best = std::numeric_limits<double>::max();

        for (int i : groupImagesByIndex[cells[cell].group])
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

    for (int step = 0; step < int(order.size()); ++step)
    {
        int cell = order[step];

        // Already taken by an image that had to be placed somewhere.
        if (assigned[cell] >= 0)
            continue;

        int cx = cells[cell].slot % gridWidth;
        int cy = cells[cell].slot / gridWidth;

        // Images already used within the variation radius are not allowed here.
        for (int y = qMax(0, cy - history); y <= qMin(gridSize.height() - 1, cy + history); ++y)
        {
            for (int x = qMax(0, cx - history); x <= qMin(gridWidth - 1, cx + history); ++x)
            {
                for (int neighbour : slotCells[y * gridWidth + x])
                {
                    int used = assigned[neighbour];
                    if (used >= 0)
                        blockedAt[used] = step;
                }
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

    // 3. Keep where every image goes, and draw the mosaic unless it is too large to be
    // kept in memory as a whole; it is then drawn in parts when it is saved.

    for (int cell : activeCells)
    {
        int slot = cells[cell].slot;
        m_placedCells.push_back({cells[cell].out, assigned[cell], groupTileSize[imageGroup[assigned[cell]]],
                                 cellColors[cell], GridPoint(slot % gridWidth, slot / gridWidth)});
    }

    m_outputSize = outputSize;
    m_tint = tint;

    if (qint64(outputSize.width()) * outputSize.height() * 4 > m_maxImageBytes)
        return !m_skipBackgroundProcess;

    auto image = std::make_shared<QImage>(outputSize, QImage::Format_ARGB32);
    if (image->isNull())
        return false;

    if (!render(*image, QRect(QPoint(0, 0), outputSize), 1.0, true))
        return false;

    m_outputImage = image;
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

QImage ImageProcessing::loadTile(const QString &imagePath, QSize cellSize) const
{
    QImage image = extractThumbnail(imagePath, cellSize);
    if (image.isNull())
        image = loadOrientedImage(imagePath, cellSize);

    // Fill the cell and crop the overflow evenly from both sides, keeping the center.
    QImage scaled = image.scaled(cellSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPoint cropOrigin((scaled.width() - cellSize.width()) / 2, (scaled.height() - cellSize.height()) / 2);
    return scaled.copy(QRect(cropOrigin, cellSize)).convertToFormat(QImage::Format_ARGB32);
}

bool ImageProcessing::render(QImage &target, const QRect &region, double scale, bool reportProgress)
{
    target.fill(Qt::transparent);

    if (m_dotsPerMeter > 0)
    {
        target.setDotsPerMeterX(m_dotsPerMeter);
        target.setDotsPerMeterY(m_dotsPerMeter);
    }

    // Output coordinates are mapped to target pixels by scaling and rounding down, so
    // neighbouring cells meet exactly also in a scaled preview.
    auto toTarget = [&](int v) { return int(std::floor(v * scale)); };
    QRect bounds = QRect(QPoint(0, 0), m_outputSize).intersected(region);
    QPoint origin(toTarget(region.x()), toTarget(region.y()));

    // Cells of each image within the region, so every image is loaded only once.
    std::map<int, std::vector<const PlacedCell *>> cellsOfImage;
    for (const PlacedCell &cell : m_placedCells)
        if (cell.out.intersects(bounds))
            cellsOfImage[cell.image].push_back(&cell);

    std::vector<int> images;
    for (const auto &entry : cellsOfImage)
        images.push_back(entry.first);

    uchar *bits = target.bits();
    qsizetype bytesPerLine = target.bytesPerLine();
    int strength = qBound(0, qRound(m_tint * 256), 256);

    // In parallel per image; cells never overlap, so threads never write the same pixels.
    QtConcurrent::blockingMap(images, [&](int image) {
        if (m_skipBackgroundProcess)
            return;

        const std::vector<const PlacedCell *> &imageCells = cellsOfImage.at(image);
        QSize tileSize = imageCells.front()->tileSize;
        QSize scaledTile(qMax(1, qCeil(tileSize.width() * scale)), qMax(1, qCeil(tileSize.height() * scale)));
        QImage tile = loadTile(m_imagePaths[image], scaledTile);

        for (const PlacedCell *cell : imageCells)
        {
            // The cell in target pixels, and the part of it inside the region.
            QRect cellRect(QPoint(toTarget(cell->out.left()), toTarget(cell->out.top())),
                           QPoint(toTarget(cell->out.right() + 1) - 1, toTarget(cell->out.bottom() + 1) - 1));
            QRect visible = cellRect.intersected(QRect(QPoint(toTarget(bounds.left()), toTarget(bounds.top())),
                                                       QPoint(toTarget(bounds.right() + 1) - 1,
                                                              toTarget(bounds.bottom() + 1) - 1)));
            visible = visible.intersected(QRect(cellRect.topLeft(), tile.size()));

            if (visible.isEmpty())
                continue;

            if (reportProgress)
                emit mosaicGenerated(cell->slot);

            // Blend the cell's color in the base image into the tile, as the mosaic reads
            // better from a distance: pixel = (1 - tint) * pixel + tint * target.
            int tr = cell->color.red() * strength;
            int tg = cell->color.green() * strength;
            int tb = cell->color.blue() * strength;

            for (int y = visible.top(); y <= visible.bottom(); ++y)
            {
                const QRgb *from = reinterpret_cast<const QRgb *>(tile.constScanLine(y - cellRect.top())) +
                                   (visible.left() - cellRect.left());
                QRgb *to = reinterpret_cast<QRgb *>(bits + (y - origin.y()) * bytesPerLine) + (visible.left() - origin.x());

                if (strength == 0)
                {
                    memcpy(to, from, visible.width() * 4);
                    continue;
                }

                for (int x = 0; x < visible.width(); ++x)
                {
                    QRgb c = from[x];
                    to[x] = qRgba((qRed(c) * (256 - strength) + tr) >> 8, (qGreen(c) * (256 - strength) + tg) >> 8,
                                  (qBlue(c) * (256 - strength) + tb) >> 8, qAlpha(c));
                }
            }
        }
    });

    return !m_skipBackgroundProcess;
}

bool ImageProcessing::hasMosaic() const
{
    return !m_placedCells.empty();
}

QSize ImageProcessing::mosaicSize() const
{
    return hasMosaic() ? m_outputSize : QSize();
}

QImage ImageProcessing::renderRegion(const QRect &region)
{
    if (!hasMosaic() || region.isEmpty())
        return QImage();

    QImage image(region.size(), QImage::Format_ARGB32);
    if (image.isNull() || !render(image, region, 1.0, false))
        return QImage();

    return image;
}

QImage ImageProcessing::renderPreview(QSize maxSize)
{
    if (!hasMosaic())
        return QImage();

    double scale = qMin(1.0, qMin(maxSize.width() / double(m_outputSize.width()),
                                  maxSize.height() / double(m_outputSize.height())));
    QSize size(qMax(1, int(std::floor(m_outputSize.width() * scale))), qMax(1, int(std::floor(m_outputSize.height() * scale))));

    QImage image(size, QImage::Format_ARGB32);
    if (image.isNull() || !render(image, QRect(QPoint(0, 0), m_outputSize), scale, false))
        return QImage();

    return image;
}

void ImageProcessing::setDotsPerInch(int dotsPerInch)
{
    m_dotsPerMeter = qRound(dotsPerInch / 0.0254);
}

qint64 ImageProcessing::maxImageBytes() const
{
    return m_maxImageBytes;
}

void ImageProcessing::setMaxImageBytes(qint64 bytes)
{
    m_maxImageBytes = bytes;
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

int rowsForTileAspect(QSize imageSize, int columns, double tileAspect)
{
    if (imageSize.isEmpty() || columns <= 0 || tileAspect <= 0.0)
        return 1;

    // Cells are imageWidth / columns wide and that divided by tileAspect high.
    return qMax(1, qRound(imageSize.height() * columns * tileAspect / imageSize.width()));
}

QList<QRect> posterRegions(QSize size, int columns, int rows, int overlap)
{
    QList<QRect> parts;

    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < columns; ++c)
        {
            int left = int(qint64(size.width()) * c / columns);
            int right = int(qint64(size.width()) * (c + 1) / columns);
            int top = int(qint64(size.height()) * r / rows);
            int bottom = int(qint64(size.height()) * (r + 1) / rows);

            QRect part(QPoint(left - overlap, top - overlap), QPoint(right - 1 + overlap, bottom - 1 + overlap));
            parts << part.intersected(QRect(QPoint(0, 0), size));
        }
    }

    return parts;
}

bool saveMosaicPart(ImageProcessing &processing, const QImage *whole, const MosaicPart &part, int dotsPerInch,
                    QString &error)
{
    QString suffix = QFileInfo(part.fileName).suffix().toLower();

    if (suffix == "tif" || suffix == "tiff")
    {
        TiffWriter writer;
        if (!writer.open(part.fileName, part.region.size(), dotsPerInch))
        {
            error = writer.errorString();
            return false;
        }

        // Strips of about 64 MB while drawing.
        int stripRows = qMax(1, int((qint64(64) << 20) / (qint64(part.region.width()) * 4)));

        for (int y = part.region.top(); y <= part.region.bottom(); y += stripRows)
        {
            QRect strip(part.region.left(), y, part.region.width(), qMin(stripRows, part.region.bottom() + 1 - y));
            QImage rows = whole ? whole->copy(strip) : processing.renderRegion(strip);

            if (rows.isNull() || !writer.writeRows(rows))
            {
                error = rows.isNull() ? QObject::tr("The mosaic could not be drawn.") : writer.errorString();
                return false;
            }
        }

        if (!writer.close())
        {
            error = writer.errorString();
            return false;
        }

        return true;
    }

    if (!whole && qint64(part.region.width()) * part.region.height() * 4 > processing.maxImageBytes())
    {
        error = QObject::tr("It is too large for PNG or JPEG. Please save it as TIFF, or in more poster parts.");
        return false;
    }

    // The drawn images carry the print resolution already; setting it here would copy
    // the whole mosaic.
    QImage image = !whole ? processing.renderRegion(part.region)
                          : part.region == whole->rect() ? *whole : whole->copy(part.region);

    if (image.isNull())
    {
        error = QObject::tr("The mosaic could not be drawn.");
        return false;
    }

    if (!image.save(part.fileName))
    {
        error = QObject::tr("The file could not be written.");
        return false;
    }

    return true;
}

QString suggestedMosaicFileName(const QString &baseImagePath, const QString &folder)
{
    QFileInfo base(baseImagePath);
    QDir dir = !folder.isEmpty() ? QDir(folder) : baseImagePath.isEmpty() ? QDir::home() : base.dir();

    // Never the base image itself: "IMG_1234_mosaic.png", then "IMG_1234_mosaic_2.png"
    // and so on if that file already exists.
    QString stem = baseImagePath.isEmpty() ? QString("mosaic") : base.completeBaseName() + "_mosaic";
    QString candidate = dir.filePath(stem + ".png");

    for (int n = 2; QFileInfo::exists(candidate); ++n)
        candidate = dir.filePath(QString("%1_%2.png").arg(stem).arg(n));

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
