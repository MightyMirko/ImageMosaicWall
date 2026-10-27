#ifndef TILECACHE_H
#define TILECACHE_H

#include <QHash>
#include <QList>
#include <QPair>
#include <QRgb>
#include <QSqlDatabase>
#include <QString>

/**
 * @brief Remembers the analysis results of tile images between runs.
 *
 * Results are stored in a small SQLite database, keyed by path, file size and
 * modification time, so only new or changed files have to be read again.
 * All methods must be called from the thread that created the object.
 * If the database can't be opened, the cache is simply empty.
 */
class TileCache
{
public:
    struct Entry
    {
        qint64 size;
        qint64 mtime;
        QRgb color;
    };

    /**
     * Bump when the stored values are computed differently, so old entries
     * are discarded instead of being mixed with new ones.
     */
    static const int Version = 1;

    explicit TileCache(const QString &fileName);
    ~TileCache();

    TileCache(const TileCache &) = delete;
    TileCache &operator=(const TileCache &) = delete;

    bool isOpen() const;
    QHash<QString, Entry> load();
    void store(const QList<QPair<QString, Entry>> &entries);

    static QString defaultFileName();

private:
    QString m_connectionName;
    QSqlDatabase m_db;
};

#endif // TILECACHE_H
