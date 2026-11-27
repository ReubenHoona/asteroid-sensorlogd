/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HEALTHIMPORT_H
#define HEALTHIMPORT_H

#include <functional>

#include <QDate>
#include <QString>
#include <QStringList>

#include "healthDb.h"

namespace Health {

/*
 * What was written before health.db, into it: once, and safe to interrupt. The files stay as they are (they are
 * never deleted) and the old readers keep reading them.
 *
 * Phases, in this order, each marked done in meta ('import.sleep', 'import.exercise', 'import.resting',
 * 'import.daily') once its last unit is in:
 *   sleep     sleep/<wake date>.json, the nights sensorlogd stored before (SleepNight v1): one file, one night:
 *             its sessions (main and naps), algo SLEEP_ALGO, imported_from "sleep/<file>". A morning that has
 *             sessions already (also deleted ones) is left as it is.
 *   exercise  exercise/<day>.log, the Exercise app's workouts ("start s:active s:type:distance m:kcal:avg bpm:
 *             max bpm:steps"): one line, one workout, imported_from "exercise/<file>"; the log has no end time,
 *             so end = start + the active time. A workout with that start (also a deleted one) is left as it is;
 *             a torn or malformed line is skipped.
 *   resting   every day with heart rate from the first one to yesterday that has no resting heart rate: its
 *             estimate (RestingFn, restingHr.h), before the totals, which use the rate in force
 *   daily     every day with steps or heart rate from the first one to yesterday: its row with the day's totals,
 *             closed (DayTotals: what the daemon closes a day with); a closed day is left as it is.
 * A unit (one file, one day) is one transaction: a stop between units loses nothing, a stop inside one loses
 * that unit's transaction only, and the next start goes through the phase again, skipping what is in.
 *
 * The Exercise app writes its workouts through sensorlogd's D-Bus methods since WO-02: its old log files are read
 * here once, new lines are not taken in (it writes none).
 */
class HealthImport
{
public:
    // the totals of a past day; false if the day has no data (no row then)
    using DayTotalsFn = std::function<bool(const QDate &day, DailyTotals *totals)>;
    // the resting heart rate of a past day; 0: none
    using RestingFn = std::function<int(const QDate &day)>;

    HealthImport(HealthDb *db, const QString &root, DayTotalsFn dayTotals, RestingFn resting,
                 const QDate &today = QDate::currentDate());

    bool done();
    // one unit of work; false when nothing is left (done() then)
    bool step();
    // step() until done; the units done
    int runAll();

    struct Counts {
        int nights = 0;   // sleep files imported
        int workouts = 0; // workout lines imported
        int resting = 0;  // days given a resting heart rate
        int days = 0;     // days closed
        int skipped = 0;  // malformed lines and files
    };
    const Counts &counts() const { return m_counts; }

    static int importSleepFile(HealthDb *db, const QString &root, const QString &fileName, int *skipped = nullptr);
    static int importExerciseFile(HealthDb *db, const QString &root, const QString &fileName, int *skipped = nullptr);

    static const char *const SLEEP_ALGO;

private:
    enum Phase { Sleep, Exercise, Resting, Daily, Done };
    void enterPhase(Phase phase);
    QString phaseKey(Phase phase) const;

    HealthDb *m_db;
    QString m_root;
    DayTotalsFn m_dayTotals;
    RestingFn m_resting;
    QDate m_today;
    Phase m_phase = Sleep;
    bool m_listed = false;
    QStringList m_files;
    QDate m_day, m_lastDay;
    int m_index = 0;
    Counts m_counts;
};

} // namespace Health

#endif // HEALTHIMPORT_H
