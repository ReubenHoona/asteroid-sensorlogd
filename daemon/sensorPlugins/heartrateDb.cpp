/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>

#include <sqlite3.h>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "heartrateDb.h"

// how long a write waits for a reader's lock (a checkpoint) before it gives up for now
static const int BUSY_TIMEOUT_MS = 5000;

HeartrateDb::HeartrateDb(const QString &path) : m_path(path)
{
    open();
}

HeartrateDb::~HeartrateDb()
{
    close();
}

bool HeartrateDb::exec(const char *sql)
{
    return sqlite3_exec(m_db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool HeartrateDb::setUp()
{
    if (!exec("PRAGMA journal_mode=WAL") || !exec("PRAGMA synchronous=NORMAL")
            || !exec("CREATE TABLE IF NOT EXISTS heartrate (time INTEGER PRIMARY KEY, "
                     "bpm INTEGER NOT NULL, accuracy INTEGER NOT NULL)"))
        return false;
    // the schema's version, for a later change of it
    sqlite3_stmt *version = nullptr;
    int current = -1;
    if (sqlite3_prepare_v2(m_db, "PRAGMA user_version", -1, &version, nullptr) == SQLITE_OK
            && sqlite3_step(version) == SQLITE_ROW)
        current = sqlite3_column_int(version, 0);
    sqlite3_finalize(version);
    if (current == 0 && !exec("PRAGMA user_version=1"))
        return false;
    return sqlite3_prepare_v2(m_db, "INSERT OR IGNORE INTO heartrate (time, bpm, accuracy) VALUES (?, ?, ?)",
                              -1, &m_insert, nullptr) == SQLITE_OK;
}

bool HeartrateDb::open()
{
    if (m_db)
        return true;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    for (int attempt = 0; attempt < 2; attempt++) {
        const int rc = sqlite3_open_v2(QFile::encodeName(m_path).constData(), &m_db,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_busy_timeout(m_db, BUSY_TIMEOUT_MS);
            if (setUp())
                return true;
        }
        const int err = m_db ? sqlite3_errcode(m_db) : rc;
        if (!m_warned)
            qWarning() << "heart rate database" << m_path << "cannot be used:"
                       << (m_db ? sqlite3_errmsg(m_db) : sqlite3_errstr(rc));
        m_warned = true;
        close();
        if (attempt > 0 || (err != SQLITE_NOTADB && err != SQLITE_CORRUPT))
            return false;
        // not a database (a damaged file): kept aside, as nothing is ever deleted, and a new one begun
        const QString aside = m_path + ".broken-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
        for (const char *suffix : {"", "-wal", "-shm"}) {
            if (QFile::exists(m_path + suffix))
                QFile::rename(m_path + suffix, aside + suffix);
        }
        qWarning() << "heart rate database moved to" << aside << "- starting a new one";
    }
    return false;
}

void HeartrateDb::close()
{
    sqlite3_finalize(m_insert);
    m_insert = nullptr;
    if (m_db)
        sqlite3_close(m_db); // the last connection checkpoints the WAL
    m_db = nullptr;
}

bool HeartrateDb::add(const QVector<Reading> &readings)
{
    m_lastAdded = 0;
    if (readings.isEmpty())
        return true;
    if (!open())
        return false;
    // in time order the rows go to the end of the table's b-tree
    QVector<Reading> sorted = readings;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Reading &a, const Reading &b) { return a.takenMs < b.takenMs; });

    bool ok = exec("BEGIN IMMEDIATE");
    int added = 0;
    for (int i = 0; ok && i < sorted.size(); i++) {
        sqlite3_bind_int64(m_insert, 1, sorted[i].takenMs);
        sqlite3_bind_int(m_insert, 2, sorted[i].bpm);
        sqlite3_bind_int(m_insert, 3, sorted[i].accuracy);
        ok = sqlite3_step(m_insert) == SQLITE_DONE;
        added += sqlite3_changes(m_db);
        sqlite3_reset(m_insert);
    }
    if (ok)
        ok = exec("COMMIT");
    if (!ok) {
        const int err = sqlite3_errcode(m_db);
        if (!m_warned)
            qWarning() << "heart rate database: cannot write" << sorted.size() << "readings:" << sqlite3_errmsg(m_db);
        m_warned = true;
        exec("ROLLBACK");
        if (err != SQLITE_BUSY && err != SQLITE_LOCKED)
            close(); // opened again for the next batch
        return false;
    }
    if (m_warned)
        qDebug() << "heart rate database: writing again";
    m_warned = false;
    m_lastAdded = added;
    return true;
}
