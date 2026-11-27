/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HEALTHDB_H
#define HEALTHDB_H

#include <functional>

#include <QDate>
#include <QList>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include "../metrics/skinTemp.h"
#include "../metrics/sleepEstimator.h"

struct sqlite3;

namespace Health {

/*
 * The health database, <loggerRootPath>/health.db (healthDbPath()), next to heartrate.db: the records that are
 * made once and may change later (workouts with their track, laps and pauses, sleep sessions with their stages,
 * one row a day) and the hub's sample streams that have no other home. Stock keeps the same in the Fitbit app's
 * Room databases with one owner (passive_stats.db: ExerciseSummary, ExerciseGps, ExerciseSplitAnnotation,
 * LocalSleepPeriods, ...); here the owner, and the only writer, is sensorlogd. Everybody else reads.
 *
 * Schema (PRAGMA user_version 3; README.md has the columns):
 *   meta          key/value: 'seq' (the change counter), 'import.<phase>' (the one-time import, healthImport.h)
 *   workout       one row a workout: start_ms (unique), end_ms (NULL while it is on), active_s, type, distance_m,
 *                 kcal, steps, avg_bpm, max_bpm, light_s, moderate_s, vigorous_s, peak_s, azm, elev_gain_m,
 *                 source (manual, auto), gps, imported_from (exercise/<day>.log of the old log), seq, deleted
 *   workout_track (workout_id, t_ms, lat_e7, lon_e7, alt_dm, hacc_dm, speed_cms)
 *   workout_lap   (workout_id, n, start_ms, end_ms, distance_m, avg_bpm, kind: split-km, split-mile, manual)
 *   workout_pause (workout_id, start_ms, end_ms, auto)
 *   workout_state (workout_id, saved_ms, state)  version 2: the snapshot of a workout that is on, as the Exercise
 *                 app saved it (its own text, for its recovery after a crash, a kill or a restart); gone at the
 *                 workout's end; not a record (no seq, a sync never reads it)
 *   sleep_session one row a session: wake_date (the morning of the night, local ISO date), start_ms, end_ms,
 *                 main, asleep_min, awake_min, rem_min, light_min, deep_min, score, goal_min, avg_hr, spo2_avg,
 *                 spo2_min, spo2_max, temp_dev_c, resp_rate, hrv_rmssd, algo, imported_from, seq, deleted
 *   sleep_stage   (session_id, start_ms, end_ms, stage: 0 awake, 1 light, 2 deep, 3 rem)
 *   daily         one row a local day: date (ISO), resting_hr, hrv_rmssd, resp_rate, temp_dev_c, steps, kcal,
 *                 distance_m, floors, azm, hours_met, cardio_load, readiness, closed_ms, seq
 *   skin_temp     (t_ms, c): the hub's skin temperature, a row a minute (its start; the mean in deg C), taken only
 *                 inside a night's sleep (M4 WO-13); a sample table: no seq
 *   skin_temp_night  version 3 (M4 WO-13): one row a morning (wake_date) once its night is through: the main
 *                 sleep (start_ms, end_ms), minutes (asleep, worn, with a reading), mean_c (NULL: too few), nights
 *                 (with a value before it in the baseline's 30 days), baseline_c, dev_c (the variation), lo_c, hi_c
 *                 (the personal range of dev_c; NULL: no baseline yet), seq; daily.temp_dev_c gets the variation
 *   spo2, hrv, eda_event   for the hub streams of later work orders (no writer yet)
 *
 * The change sequence: meta 'seq' counts every change. A row of workout, sleep_session, daily, eda_event and
 * skin_temp_night
 * takes the next value in `seq` when it is inserted or updated (triggers, so no write can forget it), and a
 * change to a workout's track, laps or pauses, or to a session's stages, gives its parent the next one. Rows are
 * never deleted: `deleted` = 1 is the tombstone. A reader that has seen everything up to seq N asks for
 * "seq > N" (changes()), table by table; the counter survives restarts and never goes back.
 *
 * Writes are one transaction each (WAL; synchronous FULL: these are a few records a day, each worth an fsync),
 * so a reader (the QML plugin, asteroid-btsyncd) never sees half a workout and a write is never blocked by one.
 * The readers below open a connection of their own, read only, and never make the file or change the schema.
 */

struct WorkoutRecord {
    qint64 id = 0;
    qint64 startMs = 0;
    qint64 endMs = 0;      // 0: on
    int activeS = 0;
    QString type;
    double distanceM = 0;
    double kcal = 0;
    int steps = 0;
    int avgBpm = 0;
    int maxBpm = 0;
    int lightS = 0, moderateS = 0, vigorousS = 0, peakS = 0;
    int azm = 0;
    double elevGainM = 0;
    QString source = QStringLiteral("manual");
    bool gps = false;
    QString importedFrom;
    qint64 seq = 0;
    bool deleted = false;

    QVariantMap toMap() const;
};

struct TrackPoint {
    qint64 tMs = 0;
    qint32 latE7 = 0, lonE7 = 0;
    int altDm = 0, haccDm = 0, speedCms = 0;
};

struct LapRecord {
    int n = 0;
    qint64 startMs = 0, endMs = 0;
    double distanceM = 0;
    int avgBpm = 0;
    QString kind = QStringLiteral("manual");
};

struct PauseRecord {
    qint64 startMs = 0;
    qint64 endMs = 0; // 0: still paused
    bool automatic = false;
};

struct DailyTotals {
    int steps = 0;
    double kcal = 0;
    double distanceM = 0;
    int floors = 0;
    int azm = 0;
    int hoursMet = 0;
};

struct DailyRecord {
    QDate date;
    int restingHr = 0;     // 0: none
    bool closed = false;   // the totals are the whole day's
    DailyTotals totals;
    qint64 closedMs = 0;
    qint64 seq = 0;

    QVariantMap toMap() const;
};

// a page of changes of one table: its rows (column -> value) in seq order
struct ChangePage {
    QList<QVariantMap> rows;
    qint64 lastSeq = 0; // the seq of the last row (the next "since")
    bool more = false;
};

class HealthDb
{
public:
    static const int SCHEMA_VERSION = 3;

    // The writer (sensorlogd): opens or makes the database and its schema
    explicit HealthDb(const QString &path);
    ~HealthDb();
    HealthDb(const HealthDb &) = delete;
    HealthDb &operator=(const HealthDb &) = delete;

    bool isOpen();
    const QString &path() const { return m_path; }
    // called after every write that changed something, with the counter after it
    void setChangeHandler(std::function<void(qint64)> handler) { m_onChange = handler; }
    // While held (a run of many writes: the one-time import), writes do not call the handler; the last release
    // calls it once if anything changed meanwhile. Holds nest.
    void holdChanges() { m_held++; }
    void releaseChanges();

    qint64 seq();
    QVariant meta(const QString &key);
    bool setMeta(const QString &key, const QVariant &value);

    // ---- workouts ----
    // a workout begins: its id, 0 if it could not be stored (bad values, a workout with that start exists)
    qint64 beginWorkout(qint64 startMs, const QString &type, const QString &source = QStringLiteral("manual"),
                        bool gps = false);
    bool addTrackPoints(qint64 id, const QVector<TrackPoint> &points);
    bool addLap(qint64 id, const LapRecord &lap);
    // paused: a pause begins at atMs (if none is open); else the open one ends there
    bool setPaused(qint64 id, qint64 atMs, bool paused, bool automatic = false);
    // the end and the summary numbers of a workout that is on (an open pause ends with it)
    bool endWorkout(qint64 id, qint64 endMs, const WorkoutRecord &summary);
    bool deleteWorkout(qint64 id);
    // the snapshot of a workout that is on (the Exercise app's own text): replaced at every save, gone at the end or
    // the deletion; seq does not change (nothing a sync reads). False if the workout is not on.
    bool saveWorkoutState(qint64 id, qint64 savedMs, const QString &state);
    // the workouts that are on (no end, not deleted)
    int openWorkoutCount();
    // a finished workout of the old log: false if one with that start is there (also a deleted one)
    bool importWorkout(const WorkoutRecord &w);

    // ---- sleep ----
    // The night that ended on night.wakeDate: its sessions (main and naps) with their stage timelines replace the
    // ones stored for that morning (tombstoned). onlyIfNone: nothing is written if the morning has any session, also
    // a deleted one.
    bool storeNight(const SleepNight &night, const QString &algo, const QString &importedFrom = QString(),
                    bool onlyIfNone = false);

    // ---- one row a day ----
    bool setDailyTotals(const QDate &date, const DailyTotals &totals, qint64 closedMs);
    bool setDailyRestingHr(const QDate &date, int bpm);
    // the latest day with closed totals (invalid: none)
    QDate lastClosedDay();

    // ---- skin temperature (M4 WO-13) ----
    // the minute rows (start ms, deg C); a minute that has one keeps it. seq does not change (a sample table).
    bool addSkinTemp(const QVector<QPair<qint64, double>> &minutes);
    // the night of a morning (replaced if there is one), and its variation in daily.temp_dev_c
    bool storeSkinTempNight(const SkinTempNight &night);

    // ---- readers: a connection of their own, read only (a database that is not there gives nothing) ----
    static qint64 readSeq(const QString &path);
    static QVariant readMeta(const QString &path, const QString &key);
    // How many rows of each kind, no values (for checks on the watch): schema, seq, workouts, workoutsDeleted,
    // workoutsImported, workoutsOpen, trackPoints, laps, pauses, nights, sleepSessions, sleepImported, naps, nightsV2
    // (the estimate of version 2), sleepStages (the stage spans of the sessions in force), days, daysClosed,
    // daysResting, skinTempMinutes, skinTempNights (the mornings done), skinTempValues (with a mean),
    // skinTempVariations, daysTempDev, and import.sleep, import.exercise, import.resting, import.daily (1: done). {}
    // without a database.
    static QVariantMap readCounts(const QString &path);
    // the workouts that began in [fromMs, toMs), newest first (deleted ones left out)
    static QList<WorkoutRecord> readWorkouts(const QString &path, qint64 fromMs, qint64 toMs, int limit = -1);
    static bool readWorkout(const QString &path, qint64 id, WorkoutRecord *workout, QVector<LapRecord> *laps = nullptr,
                            QVector<PauseRecord> *pauses = nullptr);
    static QVector<TrackPoint> readTrack(const QString &path, qint64 id);
    // The newest workout that is still on and began before beforeMs, with its snapshot (savedMs 0 and an empty state:
    // none was saved; paused: a pause of it is open); false if there is none
    static bool readOpenWorkout(const QString &path, qint64 beforeMs, WorkoutRecord *workout, qint64 *savedMs = nullptr,
                                QString *state = nullptr, bool *paused = nullptr);
    // the night that ended on wakeDate, as the sleep estimator made it (with the stage timelines and the main
    // session's algo); false if none is stored
    static bool readNight(const QString &path, const QDate &wakeDate, SleepNight *night);
    static bool readDaily(const QString &path, const QDate &date, DailyRecord *record);
    static QList<DailyRecord> readDailyRange(const QString &path, const QDate &from, const QDate &to);
    // the skin temperature's minute rows in [fromMs, toMs), in time order
    static QVector<QPair<qint64, double>> readSkinTemp(const QString &path, qint64 fromMs, qint64 toMs);
    // the skin temperature nights of the mornings from .. to, in date order (none on a database before version 3)
    static QList<SkinTempNight> readSkinTempNights(const QString &path, const QDate &from, const QDate &to);
    // The rows of a table (workout, sleep_session, daily, eda_event) changed after sinceSeq, in seq order, at
    // most max, tombstones included: what a sync asks for (asteroid-btsyncd reads the same with its own SQL)
    static ChangePage changes(const QString &path, const QString &table, qint64 sinceSeq, int max = 1000);

private:
    bool open();
    bool setUp();
    void close();
    bool exec(const char *sql);
    bool begin();
    bool commit(qint64 seqBefore);
    void rollback(bool sqlError = false);
    void warn(const char *what);
    bool workoutIsOpen(qint64 id, bool *exists = nullptr);

    QString m_path;
    sqlite3 *m_db = nullptr;
    bool m_warned = false;
    std::function<void(qint64)> m_onChange;
    int m_held = 0;
    bool m_changedWhileHeld = false;
};

} // namespace Health

#endif // HEALTHDB_H
