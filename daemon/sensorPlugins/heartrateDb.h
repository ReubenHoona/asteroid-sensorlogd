/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HEARTRATEDB_H
#define HEARTRATEDB_H

#include <QString>
#include <QVector>

struct sqlite3;
struct sqlite3_stmt;

/*
 * Every heart rate reading, in an SQLite database next to the day files
 * (<loggerRootPath>/heartrate.db, see heartrateDbPath()):
 *
 *   CREATE TABLE heartrate (time INTEGER PRIMARY KEY, bpm INTEGER NOT NULL,
 *                           accuracy INTEGER NOT NULL)
 *
 * time: when the reading was taken, milliseconds since the epoch (UTC);
 * bpm: beats per minute (above 0); accuracy: QHrmSensor::Status, 1
 * unreliable, 2 low, 3 medium, 4 high (readings without contact are not
 * kept). The time is the key: a reading that comes twice (a batch that
 * overlaps the display-on flush, sensorfw's replay of the last reading at a
 * start) is kept once, and the time ranges a reader asks for are a range of
 * the table's own b-tree.
 *
 * Readings are added in batches, one transaction each. The database is in
 * WAL mode, so readers (asteroid-btsyncd's health sync) read while a batch
 * is written, and with synchronous=NORMAL a batch costs no fsync (the WAL is
 * synced at its checkpoints, about once a day): a power loss can lose the
 * last batches, never the database. Nothing is ever deleted.
 */
class HeartrateDb
{
public:
    struct Reading {
        qint64 takenMs; // wall clock of the reading
        int bpm;
        int accuracy;   // QHrmSensor::Status
    };

    explicit HeartrateDb(const QString &path);
    ~HeartrateDb();

    // Adds the readings (any order) in one transaction; a reading whose time
    // is in the database already is left out. False if they could not be
    // written (the caller keeps them for the next try).
    bool add(const QVector<Reading> &readings);
    // readings the last add() put in the database (not the ones it left out)
    int lastAdded() const { return m_lastAdded; }

    const QString &path() const { return m_path; }

private:
    bool open();
    bool setUp();
    void close();
    bool exec(const char *sql);

    QString m_path;
    sqlite3 *m_db = nullptr;
    sqlite3_stmt *m_insert = nullptr;
    int m_lastAdded = 0;
    bool m_warned = false;
};

#endif // HEARTRATEDB_H
