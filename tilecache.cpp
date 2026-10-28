#include "tilecache.h"

#include <QAtomicInt>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

namespace
{

QAtomicInt s_connectionCounter;

} // namespace

TileCache::TileCache(const QString &fileName)
    : m_connectionName(QString("imagemosaicwall-tilecache-%1").arg(s_connectionCounter.fetchAndAddRelaxed(1)))
{
    if (!QSqlDatabase::isDriverAvailable("QSQLITE"))
    {
        qWarning() << "Tile cache disabled, the Qt SQLite driver is not available";
        return;
    }

    QDir().mkpath(QFileInfo(fileName).absolutePath());

    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    m_db.setDatabaseName(fileName);

    if (!m_db.open())
    {
        qWarning() << "Tile cache disabled, could not open" << fileName << m_db.lastError().text();
        return;
    }

    QSqlQuery query(m_db);
    query.exec("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)");

    // Discard entries written by a version that computed the values differently.
    int storedVersion = 0;
    if (query.exec("SELECT value FROM meta WHERE key = 'version'") && query.next())
        storedVersion = query.value(0).toInt();

    if (storedVersion != Version)
    {
        query.exec("DROP TABLE IF EXISTS tiles");
        query.prepare("INSERT OR REPLACE INTO meta (key, value) VALUES ('version', ?)");
        query.addBindValue(QString::number(Version));
        query.exec();
    }

    if (!query.exec("CREATE TABLE IF NOT EXISTS tiles ("
                    "path TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime INTEGER NOT NULL, color INTEGER NOT NULL, "
                    "aspect REAL NOT NULL, layout BLOB NOT NULL)"))
    {
        qWarning() << "Tile cache disabled:" << query.lastError().text();
        m_db.close();
    }
}

TileCache::~TileCache()
{
    if (m_db.isValid())
    {
        m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool TileCache::isOpen() const
{
    return m_db.isOpen();
}

QHash<QString, TileCache::Entry> TileCache::load()
{
    QHash<QString, Entry> entries;

    if (!isOpen())
        return entries;

    QSqlQuery query(m_db);
    query.setForwardOnly(true);

    if (!query.exec("SELECT path, size, mtime, color, aspect, layout FROM tiles"))
        return entries;

    while (query.next())
    {
        Entry entry;
        entry.size = query.value(1).toLongLong();
        entry.mtime = query.value(2).toLongLong();
        entry.color = static_cast<QRgb>(query.value(3).toLongLong());
        entry.aspect = query.value(4).toFloat();
        entry.layout = query.value(5).toByteArray();
        entries.insert(query.value(0).toString(), entry);
    }

    return entries;
}

void TileCache::store(const QList<QPair<QString, Entry>> &entries)
{
    if (!isOpen() || entries.isEmpty())
        return;

    // One transaction for all rows, otherwise SQLite syncs to disk for every insert.
    m_db.transaction();

    QSqlQuery query(m_db);
    query.prepare("INSERT OR REPLACE INTO tiles (path, size, mtime, color, aspect, layout) VALUES (?, ?, ?, ?, ?, ?)");

    for (const auto &entry : entries)
    {
        query.addBindValue(entry.first);
        query.addBindValue(entry.second.size);
        query.addBindValue(entry.second.mtime);
        query.addBindValue(static_cast<qint64>(entry.second.color));
        query.addBindValue(entry.second.aspect);
        query.addBindValue(entry.second.layout);

        if (!query.exec())
        {
            qWarning() << "Tile cache: could not store" << entry.first << query.lastError().text();
            m_db.rollback();
            return;
        }
    }

    m_db.commit();
}

QString TileCache::defaultFileName()
{
    // Inside digiKam this is ~/.cache/digikam/imagemosaicwall/tiles.sqlite on Linux.
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/imagemosaicwall/tiles.sqlite";
}
