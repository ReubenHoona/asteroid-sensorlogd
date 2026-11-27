/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <sqlite3.h>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include "healthDb.h"

namespace Health {

namespace {

// how long a write waits for a reader's lock (a checkpoint) before it gives up for now; a reader for the writer
const int WRITE_BUSY_MS = 5000;
const int READ_BUSY_MS = 2000;

// the tables whose rows carry seq, and their key
struct Keyed {
    const char *table;
    const char *key;
};
const Keyed MUTABLE[] = {{"workout", "id"},     {"sleep_session", "id"}, {"daily", "date"}, {"eda_event", "id"},
                          {"skin_temp_night", "wake_date"}};
// the tables whose rows belong to a parent row: a change to them is a change of the parent
struct Child {
    const char *table;
    const char *parentKey;
    const char *parent;
};
const Child CHILDREN[] = {{"workout_track", "workout_id", "workout"},
                          {"workout_lap", "workout_id", "workout"},
                          {"workout_pause", "workout_id", "workout"},
                          {"sleep_stage", "session_id", "sleep_session"}};

const char *const SCHEMA_V1[] = {
    "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value)",
    "INSERT OR IGNORE INTO meta (key, value) VALUES ('seq', 0)",
    "CREATE TABLE IF NOT EXISTS workout (id INTEGER PRIMARY KEY, start_ms INTEGER NOT NULL UNIQUE, end_ms INTEGER, "
    "active_s INTEGER NOT NULL DEFAULT 0, type TEXT NOT NULL, distance_m REAL, kcal REAL, steps INTEGER, "
    "avg_bpm INTEGER, max_bpm INTEGER, light_s INTEGER, moderate_s INTEGER, vigorous_s INTEGER, peak_s INTEGER, "
    "azm INTEGER, elev_gain_m REAL, source TEXT NOT NULL DEFAULT 'manual', gps INTEGER NOT NULL DEFAULT 0, "
    "imported_from TEXT, seq INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS workout_seq ON workout (seq)",
    "CREATE TABLE IF NOT EXISTS workout_track (workout_id INTEGER NOT NULL, t_ms INTEGER NOT NULL, "
    "lat_e7 INTEGER NOT NULL, lon_e7 INTEGER NOT NULL, alt_dm INTEGER, hacc_dm INTEGER, speed_cms INTEGER, "
    "PRIMARY KEY (workout_id, t_ms)) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS workout_lap (workout_id INTEGER NOT NULL, n INTEGER NOT NULL, "
    "start_ms INTEGER NOT NULL, end_ms INTEGER NOT NULL, distance_m REAL, avg_bpm INTEGER, "
    "kind TEXT NOT NULL DEFAULT 'manual', PRIMARY KEY (workout_id, n))",
    "CREATE TABLE IF NOT EXISTS workout_pause (workout_id INTEGER NOT NULL, start_ms INTEGER NOT NULL, "
    "end_ms INTEGER, auto INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (workout_id, start_ms))",
    "CREATE TABLE IF NOT EXISTS sleep_session (id INTEGER PRIMARY KEY, wake_date TEXT NOT NULL, "
    "start_ms INTEGER NOT NULL, end_ms INTEGER NOT NULL, main INTEGER NOT NULL DEFAULT 0, asleep_min INTEGER, "
    "awake_min INTEGER, rem_min INTEGER, light_min INTEGER, deep_min INTEGER, score INTEGER, goal_min INTEGER, "
    "avg_hr REAL, spo2_avg REAL, spo2_min REAL, spo2_max REAL, temp_dev_c REAL, resp_rate REAL, hrv_rmssd REAL, "
    "algo TEXT, imported_from TEXT, seq INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS sleep_session_wake ON sleep_session (wake_date)",
    "CREATE INDEX IF NOT EXISTS sleep_session_seq ON sleep_session (seq)",
    "CREATE TABLE IF NOT EXISTS sleep_stage (session_id INTEGER NOT NULL, start_ms INTEGER NOT NULL, "
    "end_ms INTEGER NOT NULL, stage INTEGER NOT NULL, PRIMARY KEY (session_id, start_ms))",
    "CREATE TABLE IF NOT EXISTS daily (date TEXT PRIMARY KEY, resting_hr INTEGER, hrv_rmssd REAL, resp_rate REAL, "
    "temp_dev_c REAL, steps INTEGER, kcal REAL, distance_m REAL, floors INTEGER, azm INTEGER, hours_met INTEGER, "
    "cardio_load REAL, readiness INTEGER, closed_ms INTEGER, seq INTEGER NOT NULL DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS daily_seq ON daily (seq)",
    "CREATE TABLE IF NOT EXISTS spo2 (t_ms INTEGER PRIMARY KEY, pct REAL NOT NULL, confidence INTEGER)",
    "CREATE TABLE IF NOT EXISTS hrv (t_ms INTEGER PRIMARY KEY, rmssd_ms REAL NOT NULL)",
    "CREATE TABLE IF NOT EXISTS skin_temp (t_ms INTEGER PRIMARY KEY, c REAL NOT NULL)",
    "CREATE TABLE IF NOT EXISTS eda_event (id INTEGER PRIMARY KEY, start_ms INTEGER NOT NULL, end_ms INTEGER, "
    "level INTEGER, mood TEXT, seq INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS eda_event_seq ON eda_event (seq)",
};

// version 2: the snapshot of the workout that is on (WO-02); not a record, so no seq and no trigger
const char *const SCHEMA_V2[] = {
    "CREATE TABLE IF NOT EXISTS workout_state (workout_id INTEGER PRIMARY KEY, saved_ms INTEGER NOT NULL, "
    "state TEXT NOT NULL)",
};

// version 3: the night's skin temperature (WO-13); a record with seq (its triggers come with seqTriggers())
const char *const SCHEMA_V3[] = {
    "CREATE TABLE IF NOT EXISTS skin_temp_night (wake_date TEXT PRIMARY KEY, start_ms INTEGER NOT NULL, "
    "end_ms INTEGER NOT NULL, minutes INTEGER NOT NULL DEFAULT 0, mean_c REAL, nights INTEGER NOT NULL DEFAULT 0, "
    "baseline_c REAL, dev_c REAL, lo_c REAL, hi_c REAL, seq INTEGER NOT NULL DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS skin_temp_night_seq ON skin_temp_night (seq)",
};

// a snapshot is a few kB; anything far larger is not one
const int MAX_STATE_CHARS = 1 << 20;

// the triggers of the change sequence
QStringList seqTriggers()
{
    QStringList sql;
    const QString bump = QStringLiteral("UPDATE meta SET value = value + 1 WHERE key = 'seq'; "
                                        "UPDATE %1 SET seq = (SELECT value FROM meta WHERE key = 'seq') "
                                        "WHERE %2 = NEW.%2;");
    for (const Keyed &k : MUTABLE) {
        sql << QStringLiteral("CREATE TRIGGER IF NOT EXISTS %1_seq_insert AFTER INSERT ON %1 BEGIN %2 END")
                   .arg(k.table, bump.arg(k.table, k.key));
        // the inner UPDATE sets seq, so it does not come back here
        sql << QStringLiteral("CREATE TRIGGER IF NOT EXISTS %1_seq_update AFTER UPDATE ON %1 "
                              "WHEN NEW.seq IS OLD.seq BEGIN %2 END")
                   .arg(k.table, bump.arg(k.table, k.key));
    }
    for (const Child &c : CHILDREN) {
        // "SET seq = seq" is an update of the parent: its update trigger gives it the next seq
        for (const char *op : {"INSERT", "UPDATE", "DELETE"}) {
            const char *row = QByteArray(op) == "DELETE" ? "OLD" : "NEW";
            sql << QStringLiteral("CREATE TRIGGER IF NOT EXISTS %1_touch_%2 AFTER %3 ON %1 BEGIN "
                                  "UPDATE %4 SET seq = seq WHERE id = %5.%6; END")
                       .arg(c.table, QString::fromLatin1(op).toLower(), op, c.parent, row, c.parentKey);
        }
    }
    return sql;
}

// a prepared statement, finalized with the scope
class Stmt
{
public:
    Stmt(sqlite3 *db, const char *sql)
    {
        if (db && sqlite3_prepare_v2(db, sql, -1, &m_st, nullptr) != SQLITE_OK) {
            sqlite3_finalize(m_st);
            m_st = nullptr;
        }
    }
    ~Stmt() { sqlite3_finalize(m_st); }
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;

    bool ok() const { return m_st != nullptr; }
    Stmt &bind(int i, qint64 v) { sqlite3_bind_int64(m_st, i, v); return *this; }
    Stmt &bind(int i, int v) { sqlite3_bind_int(m_st, i, v); return *this; }
    Stmt &bind(int i, double v) { sqlite3_bind_double(m_st, i, v); return *this; }
    Stmt &bind(int i, const QString &v)
    {
        if (v.isNull()) {
            sqlite3_bind_null(m_st, i);
        } else {
            const QByteArray u = v.toUtf8();
            sqlite3_bind_text(m_st, i, u.constData(), u.size(), SQLITE_TRANSIENT);
        }
        return *this;
    }
    Stmt &bindNull(int i) { sqlite3_bind_null(m_st, i); return *this; }
    void reset()
    {
        sqlite3_reset(m_st);
        sqlite3_clear_bindings(m_st);
    }
    // a positive number, or NULL for 0 (an unknown value)
    Stmt &bindPositive(int i, qint64 v) { return v > 0 ? bind(i, v) : bindNull(i); }
    int step() { return m_st ? sqlite3_step(m_st) : SQLITE_MISUSE; }
    bool run()
    {
        const int rc = step();
        return rc == SQLITE_DONE || rc == SQLITE_ROW;
    }
    bool row() { return step() == SQLITE_ROW; }
    qint64 i64(int c) const { return sqlite3_column_int64(m_st, c); }
    int i(int c) const { return sqlite3_column_int(m_st, c); }
    double d(int c) const { return sqlite3_column_double(m_st, c); }
    bool isNull(int c) const { return sqlite3_column_type(m_st, c) == SQLITE_NULL; }
    QString s(int c) const
    {
        if (isNull(c))
            return QString();
        return QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(m_st, c)),
                                 sqlite3_column_bytes(m_st, c));
    }
    QVariant value(int c) const
    {
        switch (sqlite3_column_type(m_st, c)) {
        case SQLITE_INTEGER: return QVariant((qlonglong)i64(c));
        case SQLITE_FLOAT: return QVariant(d(c));
        case SQLITE_TEXT: return QVariant(s(c));
        default: return QVariant();
        }
    }
    int columns() const { return sqlite3_column_count(m_st); }
    QString name(int c) const { return QString::fromUtf8(sqlite3_column_name(m_st, c)); }

private:
    sqlite3_stmt *m_st = nullptr;
};

// A reader's connection: read only. A database in WAL mode is read through its -shm file, which a read-only
// connection cannot make when no writer has it open; then one that may write is opened, used only to read
// (query_only, and it does not checkpoint when it closes).
class ReadConn
{
public:
    explicit ReadConn(const QString &path)
    {
        if (!QFile::exists(path))
            return;
        const QByteArray name = QFile::encodeName(path);
        if (sqlite3_open_v2(name.constData(), &m_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) == SQLITE_OK) {
            sqlite3_busy_timeout(m_db, READ_BUSY_MS);
            if (readVersion())
                return;
        }
        sqlite3_close(m_db);
        m_db = nullptr;
        if (sqlite3_open_v2(name.constData(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) == SQLITE_OK) {
            sqlite3_busy_timeout(m_db, READ_BUSY_MS);
            sqlite3_db_config(m_db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, nullptr);
            sqlite3_exec(m_db, "PRAGMA query_only=1", nullptr, nullptr, nullptr);
            if (readVersion())
                return;
        }
        sqlite3_close(m_db);
        m_db = nullptr;
    }
    ~ReadConn() { sqlite3_close(m_db); }
    ReadConn(const ReadConn &) = delete;
    ReadConn &operator=(const ReadConn &) = delete;
    // a database with the schema
    sqlite3 *db() const { return m_version >= 1 ? m_db : nullptr; }

private:
    bool readVersion()
    {
        Stmt v(m_db, "PRAGMA user_version");
        if (!v.ok() || !v.row())
            return false;
        m_version = v.i(0);
        return true;
    }
    sqlite3 *m_db = nullptr;
    int m_version = 0;
};

const char *const WORKOUT_COLUMNS =
    "id, start_ms, end_ms, active_s, type, distance_m, kcal, steps, avg_bpm, max_bpm, light_s, moderate_s, "
    "vigorous_s, peak_s, azm, elev_gain_m, source, gps, imported_from, seq, deleted";

WorkoutRecord workoutFrom(const Stmt &q)
{
    WorkoutRecord w;
    w.id = q.i64(0);
    w.startMs = q.i64(1);
    w.endMs = q.isNull(2) ? 0 : q.i64(2);
    w.activeS = q.i(3);
    w.type = q.s(4);
    w.distanceM = q.d(5);
    w.kcal = q.d(6);
    w.steps = q.i(7);
    w.avgBpm = q.i(8);
    w.maxBpm = q.i(9);
    w.lightS = q.i(10);
    w.moderateS = q.i(11);
    w.vigorousS = q.i(12);
    w.peakS = q.i(13);
    w.azm = q.i(14);
    w.elevGainM = q.d(15);
    w.source = q.s(16);
    w.gps = q.i(17) != 0;
    w.importedFrom = q.s(18);
    w.seq = q.i64(19);
    w.deleted = q.i(20) != 0;
    return w;
}

const char *const DAILY_COLUMNS = "date, resting_hr, steps, kcal, distance_m, floors, azm, hours_met, closed_ms, seq";

DailyRecord dailyFrom(const Stmt &q)
{
    DailyRecord r;
    r.date = QDate::fromString(q.s(0), Qt::ISODate);
    r.restingHr = q.isNull(1) ? 0 : q.i(1);
    r.closed = !q.isNull(8);
    r.totals.steps = q.i(2);
    r.totals.kcal = q.d(3);
    r.totals.distanceM = q.d(4);
    r.totals.floors = q.i(5);
    r.totals.azm = q.i(6);
    r.totals.hoursMet = q.i(7);
    r.closedMs = q.isNull(8) ? 0 : q.i64(8);
    r.seq = q.i64(9);
    return r;
}

const char *const SKIN_NIGHT_COLUMNS =
    "wake_date, start_ms, end_ms, minutes, mean_c, nights, baseline_c, dev_c, lo_c, hi_c, seq";

SkinTempNight skinNightFrom(const Stmt &q)
{
    SkinTempNight n;
    n.wakeDate = QDate::fromString(q.s(0), Qt::ISODate);
    n.startMs = q.i64(1);
    n.endMs = q.i64(2);
    n.minutes = q.i(3);
    n.hasMean = !q.isNull(4);
    n.meanC = q.d(4);
    n.nights = q.i(5);
    n.hasVariation = !q.isNull(7) && !q.isNull(8) && !q.isNull(9);
    n.baselineC = q.d(6);
    n.devC = q.d(7);
    n.loC = q.d(8);
    n.hiC = q.d(9);
    n.seq = q.i64(10);
    return n;
}

bool validType(const QString &type)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,40}$"));
    return re.match(type).hasMatch();
}

} // namespace

QVariantMap WorkoutRecord::toMap() const
{
    return QVariantMap{{"id", id}, {"startMs", startMs}, {"endMs", endMs}, {"activeS", activeS}, {"type", type},
                       {"distanceM", distanceM}, {"kcal", kcal}, {"steps", steps}, {"avgBpm", avgBpm},
                       {"maxBpm", maxBpm}, {"lightS", lightS}, {"moderateS", moderateS}, {"vigorousS", vigorousS},
                       {"peakS", peakS}, {"azm", azm}, {"elevGainM", elevGainM}, {"source", source}, {"gps", gps},
                       {"imported", !importedFrom.isEmpty()}, {"ended", endMs > 0}, {"seq", seq}};
}

QVariantMap DailyRecord::toMap() const
{
    return QVariantMap{{"date", date}, {"restingHr", restingHr}, {"closed", closed}, {"steps", totals.steps},
                       {"kcal", totals.kcal}, {"distanceM", totals.distanceM}, {"floors", totals.floors},
                       {"azm", totals.azm}, {"hoursMet", totals.hoursMet}, {"closedMs", closedMs}, {"seq", seq}};
}

HealthDb::HealthDb(const QString &path) : m_path(path)
{
    open();
}

HealthDb::~HealthDb()
{
    close();
}

bool HealthDb::exec(const char *sql)
{
    return sqlite3_exec(m_db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

void HealthDb::warn(const char *what)
{
    if (!m_warned)
        qWarning() << "health database" << m_path << what << (m_db ? sqlite3_errmsg(m_db) : "");
    m_warned = true;
}

bool HealthDb::setUp()
{
    if (!exec("PRAGMA journal_mode=WAL") || !exec("PRAGMA synchronous=FULL"))
        return false;
    int version = -1;
    {
        Stmt v(m_db, "PRAGMA user_version");
        if (v.ok() && v.row())
            version = v.i(0);
    }
    if (version < 0)
        return false;
    if (version > SCHEMA_VERSION) {
        // made by a newer sensorlogd: its tables have what this one writes
        qWarning() << "health database" << m_path << "has schema" << version << "- this sensorlogd knows"
                   << SCHEMA_VERSION;
        return true;
    }
    if (version == SCHEMA_VERSION)
        return true;
    // a new database (0): the schema in one transaction, then its version; an older one: what the later versions add
    if (!exec("BEGIN IMMEDIATE"))
        return false;
    bool ok = true;
    if (version < 1) {
        for (const char *sql : SCHEMA_V1)
            ok = ok && exec(sql);
    }
    if (version < 2) {
        for (const char *sql : SCHEMA_V2)
            ok = ok && exec(sql);
    }
    if (version < 3) {
        for (const char *sql : SCHEMA_V3)
            ok = ok && exec(sql);
    }
    // the triggers of every table that has seq (IF NOT EXISTS: those there already stay)
    for (const QString &sql : seqTriggers())
        ok = ok && exec(sql.toUtf8().constData());
    ok = ok && exec(QByteArray("PRAGMA user_version=" + QByteArray::number(SCHEMA_VERSION)).constData());
    if (ok && exec("COMMIT"))
        return true;
    exec("ROLLBACK");
    return false;
}

bool HealthDb::open()
{
    if (m_db)
        return true;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    for (int attempt = 0; attempt < 2; attempt++) {
        const int rc = sqlite3_open_v2(QFile::encodeName(m_path).constData(), &m_db,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_busy_timeout(m_db, WRITE_BUSY_MS);
            if (setUp()) {
                if (m_warned)
                    qDebug() << "health database: usable again";
                m_warned = false;
                return true;
            }
        }
        const int err = m_db ? sqlite3_errcode(m_db) : rc;
        warn("cannot be used:");
        close();
        if (attempt > 0 || (err != SQLITE_NOTADB && err != SQLITE_CORRUPT))
            return false;
        // not a database (a damaged file): kept aside, as nothing is ever deleted, and a new one begun (the
        // one-time import then reads the old day files again)
        const QString aside = m_path + ".broken-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
        for (const char *suffix : {"", "-wal", "-shm"}) {
            if (QFile::exists(m_path + suffix))
                QFile::rename(m_path + suffix, aside + suffix);
        }
        qWarning() << "health database moved to" << aside << "- starting a new one";
        m_warned = false;
    }
    return false;
}

void HealthDb::close()
{
    if (m_db)
        sqlite3_close(m_db); // the last connection checkpoints the WAL
    m_db = nullptr;
}

bool HealthDb::isOpen()
{
    return open();
}

bool HealthDb::begin()
{
    if (!open())
        return false;
    if (exec("BEGIN IMMEDIATE"))
        return true;
    warn("cannot write:");
    return false;
}

bool HealthDb::commit(qint64 seqBefore)
{
    if (!exec("COMMIT")) {
        rollback(true);
        return false;
    }
    const qint64 after = seq();
    if (after != seqBefore) {
        if (m_held > 0)
            m_changedWhileHeld = true;
        else if (m_onChange)
            m_onChange(after);
    }
    return true;
}

void HealthDb::releaseChanges()
{
    if (m_held == 0 || --m_held > 0 || !m_changedWhileHeld)
        return;
    m_changedWhileHeld = false;
    if (m_onChange)
        m_onChange(seq());
}

void HealthDb::rollback(bool sqlError)
{
    const int err = sqlError ? sqlite3_extended_errcode(m_db) & 0xff : SQLITE_OK;
    if (sqlError && err != SQLITE_CONSTRAINT)
        warn("cannot write:");
    exec("ROLLBACK");
    if (sqlError && err != SQLITE_BUSY && err != SQLITE_LOCKED && err != SQLITE_CONSTRAINT)
        close(); // opened again for the next write
}

qint64 HealthDb::seq()
{
    if (!open())
        return 0;
    Stmt q(m_db, "SELECT value FROM meta WHERE key = 'seq'");
    return q.ok() && q.row() ? q.i64(0) : 0;
}

QVariant HealthDb::meta(const QString &key)
{
    if (!open())
        return QVariant();
    Stmt q(m_db, "SELECT value FROM meta WHERE key = ?");
    q.bind(1, key);
    return q.ok() && q.row() ? q.value(0) : QVariant();
}

bool HealthDb::setMeta(const QString &key, const QVariant &value)
{
    if (key == QLatin1String("seq") || !open())
        return false;
    Stmt q(m_db, "INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)");
    q.bind(1, key);
    if (value.typeId() == QMetaType::LongLong || value.typeId() == QMetaType::Int || value.typeId() == QMetaType::Bool)
        q.bind(2, (qint64)value.toLongLong());
    else
        q.bind(2, value.toString());
    return q.ok() && q.run();
}

bool HealthDb::workoutIsOpen(qint64 id, bool *exists)
{
    Stmt q(m_db, "SELECT end_ms IS NULL FROM workout WHERE id = ? AND deleted = 0");
    q.bind(1, id);
    const bool found = q.ok() && q.row();
    if (exists)
        *exists = found;
    return found && q.i(0) != 0;
}

qint64 HealthDb::beginWorkout(qint64 startMs, const QString &type, const QString &source, bool gps)
{
    const QString src = source.isEmpty() ? QStringLiteral("manual") : source;
    if (startMs <= 0 || !validType(type) || (src != QLatin1String("manual") && src != QLatin1String("auto")))
        return 0;
    const qint64 before = seq();
    if (!begin())
        return 0;
    Stmt q(m_db, "INSERT INTO workout (start_ms, type, source, gps) VALUES (?, ?, ?, ?)");
    q.bind(1, startMs).bind(2, type).bind(3, src).bind(4, gps ? 1 : 0);
    if (!q.ok() || !q.run()) {
        rollback(true);
        return 0;
    }
    const qint64 id = sqlite3_last_insert_rowid(m_db);
    return commit(before) ? id : 0;
}

bool HealthDb::addTrackPoints(qint64 id, const QVector<TrackPoint> &points)
{
    const qint64 before = seq();
    if (!begin())
        return false;
    bool exists = false;
    workoutIsOpen(id, &exists);
    if (!exists) {
        rollback();
        return false;
    }
    Stmt q(m_db, "INSERT OR REPLACE INTO workout_track (workout_id, t_ms, lat_e7, lon_e7, alt_dm, hacc_dm, speed_cms) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?)");
    bool ok = q.ok();
    for (const TrackPoint &p : points) {
        if (!ok)
            break;
        if (p.tMs <= 0 || qAbs((qint64)p.latE7) > 900000000LL || qAbs((qint64)p.lonE7) > 1800000000LL)
            continue; // not a position
        q.bind(1, id).bind(2, p.tMs).bind(3, (qint64)p.latE7).bind(4, (qint64)p.lonE7).bind(5, p.altDm)
                .bind(6, p.haccDm).bind(7, p.speedCms);
        ok = q.run();
        q.reset();
    }
    if (!ok) {
        rollback(true);
        return false;
    }
    return commit(before);
}

bool HealthDb::addLap(qint64 id, const LapRecord &lap)
{
    static const QSet<QString> kinds = {QStringLiteral("split-km"), QStringLiteral("split-mile"),
                                        QStringLiteral("manual")};
    if (lap.n < 1 || lap.endMs < lap.startMs || !kinds.contains(lap.kind))
        return false;
    const qint64 before = seq();
    if (!begin())
        return false;
    bool exists = false;
    workoutIsOpen(id, &exists);
    Stmt q(m_db, "INSERT OR REPLACE INTO workout_lap (workout_id, n, start_ms, end_ms, distance_m, avg_bpm, kind) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?)");
    q.bind(1, id).bind(2, lap.n).bind(3, lap.startMs).bind(4, lap.endMs).bind(5, qMax(0.0, lap.distanceM))
            .bind(6, qMax(0, lap.avgBpm)).bind(7, lap.kind);
    if (!exists || !q.ok() || !q.run()) {
        rollback(exists);
        return false;
    }
    return commit(before);
}

bool HealthDb::setPaused(qint64 id, qint64 atMs, bool paused, bool automatic)
{
    const qint64 before = seq();
    if (atMs <= 0 || !begin())
        return false;
    if (!workoutIsOpen(id)) {
        rollback();
        return false;
    }
    bool ok;
    if (paused) {
        Stmt open(m_db, "SELECT 1 FROM workout_pause WHERE workout_id = ? AND end_ms IS NULL");
        open.bind(1, id);
        Stmt q(m_db, "INSERT INTO workout_pause (workout_id, start_ms, auto) VALUES (?, ?, ?)");
        q.bind(1, id).bind(2, atMs).bind(3, automatic ? 1 : 0);
        ok = open.ok() && !open.row() && q.ok() && q.run();
    } else {
        Stmt q(m_db, "UPDATE workout_pause SET end_ms = ? WHERE workout_id = ? AND end_ms IS NULL AND start_ms <= ?");
        q.bind(1, atMs).bind(2, id).bind(3, atMs);
        ok = q.ok() && q.run() && sqlite3_changes(m_db) == 1;
    }
    if (!ok) {
        rollback();
        return false;
    }
    return commit(before);
}

bool HealthDb::endWorkout(qint64 id, qint64 endMs, const WorkoutRecord &s)
{
    const qint64 before = seq();
    if (!begin())
        return false;
    bool ok = workoutIsOpen(id);
    if (ok) {
        Stmt pause(m_db, "UPDATE workout_pause SET end_ms = MAX(start_ms, ?) WHERE workout_id = ? AND end_ms IS NULL");
        pause.bind(1, endMs).bind(2, id);
        Stmt q(m_db, "UPDATE workout SET end_ms = ?, active_s = ?, distance_m = ?, kcal = ?, steps = ?, avg_bpm = ?, "
                     "max_bpm = ?, light_s = ?, moderate_s = ?, vigorous_s = ?, peak_s = ?, azm = ?, elev_gain_m = ? "
                     "WHERE id = ? AND start_ms <= ?");
        q.bind(1, endMs).bind(2, qMax(0, s.activeS)).bind(3, qMax(0.0, s.distanceM)).bind(4, qMax(0.0, s.kcal))
                .bind(5, qMax(0, s.steps)).bind(6, qMax(0, s.avgBpm)).bind(7, qMax(0, s.maxBpm))
                .bind(8, qMax(0, s.lightS)).bind(9, qMax(0, s.moderateS)).bind(10, qMax(0, s.vigorousS))
                .bind(11, qMax(0, s.peakS)).bind(12, qMax(0, s.azm)).bind(13, qMax(0.0, s.elevGainM)).bind(14, id)
                .bind(15, endMs);
        ok = pause.ok() && pause.run() && q.ok() && q.run() && sqlite3_changes(m_db) == 1;
        // the snapshot of the workout that was on is not needed any more
        Stmt state(m_db, "DELETE FROM workout_state WHERE workout_id = ?");
        state.bind(1, id);
        ok = ok && state.ok() && state.run();
    }
    if (!ok) {
        rollback();
        return false;
    }
    return commit(before);
}

bool HealthDb::deleteWorkout(qint64 id)
{
    const qint64 before = seq();
    if (!begin())
        return false;
    // the tombstone stays (a sync learns of the deletion); what was recorded in the workout goes with it
    Stmt q(m_db, "UPDATE workout SET deleted = 1 WHERE id = ? AND deleted = 0");
    q.bind(1, id);
    bool ok = q.ok() && q.run() && sqlite3_changes(m_db) == 1;
    for (const char *sql : {"DELETE FROM workout_track WHERE workout_id = ?", "DELETE FROM workout_lap WHERE workout_id = ?",
                            "DELETE FROM workout_pause WHERE workout_id = ?", "DELETE FROM workout_state WHERE workout_id = ?"}) {
        if (!ok)
            break;
        Stmt d(m_db, sql);
        d.bind(1, id);
        ok = d.ok() && d.run();
    }
    if (!ok) {
        rollback();
        return false;
    }
    return commit(before);
}

bool HealthDb::saveWorkoutState(qint64 id, qint64 savedMs, const QString &state)
{
    if (savedMs <= 0 || state.isEmpty() || state.size() > MAX_STATE_CHARS)
        return false;
    const qint64 before = seq();
    if (!begin())
        return false;
    if (!workoutIsOpen(id)) {
        rollback();
        return false;
    }
    Stmt q(m_db, "INSERT OR REPLACE INTO workout_state (workout_id, saved_ms, state) VALUES (?, ?, ?)");
    q.bind(1, id).bind(2, savedMs).bind(3, state);
    if (!q.ok() || !q.run()) {
        rollback(true);
        return false;
    }
    return commit(before);
}

int HealthDb::openWorkoutCount()
{
    if (!open())
        return 0;
    Stmt q(m_db, "SELECT COUNT(*) FROM workout WHERE end_ms IS NULL AND deleted = 0");
    return q.ok() && q.row() ? q.i(0) : 0;
}

bool HealthDb::importWorkout(const WorkoutRecord &w)
{
    if (w.startMs <= 0 || w.endMs < w.startMs || !validType(w.type))
        return false;
    const qint64 before = seq();
    if (!begin())
        return false;
    Stmt q(m_db, "INSERT OR IGNORE INTO workout (start_ms, end_ms, active_s, type, distance_m, kcal, steps, avg_bpm, "
                 "max_bpm, source, gps, imported_from) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    q.bind(1, w.startMs).bind(2, w.endMs).bind(3, qMax(0, w.activeS)).bind(4, w.type).bind(5, qMax(0.0, w.distanceM))
            .bind(6, qMax(0.0, w.kcal)).bind(7, qMax(0, w.steps)).bind(8, qMax(0, w.avgBpm)).bind(9, qMax(0, w.maxBpm))
            .bind(10, w.source.isEmpty() ? QStringLiteral("manual") : w.source).bind(11, w.gps ? 1 : 0)
            .bind(12, w.importedFrom);
    if (!q.ok() || !q.run()) {
        rollback(true);
        return false;
    }
    const bool added = sqlite3_changes(m_db) == 1;
    return commit(before) && added;
}

bool HealthDb::storeNight(const SleepNight &night, const QString &algo, const QString &importedFrom, bool onlyIfNone)
{
    // a night, or a day's naps without one (R5 E-30)
    if ((!night.valid && night.naps.isEmpty()) || !night.wakeDate.isValid())
        return false;
    const QString wake = night.wakeDate.toString(Qt::ISODate);
    const qint64 before = seq();
    if (!begin())
        return false;
    bool ok = true;
    if (onlyIfNone) {
        Stmt any(m_db, "SELECT 1 FROM sleep_session WHERE wake_date = ? LIMIT 1");
        any.bind(1, wake);
        if (!any.ok() || any.row()) {
            exec("ROLLBACK");
            return false;
        }
    } else {
        Stmt old(m_db, "UPDATE sleep_session SET deleted = 1 WHERE wake_date = ? AND deleted = 0");
        old.bind(1, wake);
        ok = old.ok() && old.run();
    }
    auto insert = [&](const SleepSession &s, bool main) {
        Stmt q(m_db, "INSERT INTO sleep_session (wake_date, start_ms, end_ms, main, asleep_min, awake_min, rem_min, "
                     "light_min, deep_min, score, goal_min, avg_hr, algo, imported_from) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        q.bind(1, wake).bind(2, s.startMs).bind(3, s.endMs).bind(4, main ? 1 : 0).bind(5, s.asleepMin)
                .bind(6, s.awakeMin).bind(7, s.remMin).bind(8, s.lightMin).bind(9, s.deepMin);
        if (main)
            q.bind(10, night.score).bind(11, night.goalMin);
        else
            q.bindNull(10).bindNull(11);
        q.bind(12, s.avgHr).bind(13, algo).bind(14, importedFrom);
        if (!q.ok() || !q.run())
            return false;
        // the stage timeline (version 2 of the estimate; a night of version 1 or of the old files has none)
        const qint64 id = sqlite3_last_insert_rowid(m_db);
        Stmt st(m_db, "INSERT INTO sleep_stage (session_id, start_ms, end_ms, stage) VALUES (?, ?, ?, ?)");
        for (const SleepStageSpan &span : s.stages) {
            if (span.endMs <= span.startMs || span.stage < StageAwake || span.stage > StageRem)
                return false;
            st.reset();
            st.bind(1, id).bind(2, span.startMs).bind(3, span.endMs).bind(4, span.stage);
            if (!st.ok() || !st.run())
                return false;
        }
        return true;
    };
    if (night.valid)
        ok = ok && insert(night.main, true);
    for (const SleepSession &nap : night.naps)
        ok = ok && insert(nap, false);
    if (!ok) {
        rollback(true);
        return false;
    }
    return commit(before);
}

bool HealthDb::setDailyTotals(const QDate &date, const DailyTotals &t, qint64 closedMs)
{
    if (!date.isValid())
        return false;
    const qint64 before = seq();
    if (!begin())
        return false;
    Stmt q(m_db, "INSERT INTO daily (date, steps, kcal, distance_m, floors, azm, hours_met, closed_ms) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (date) DO UPDATE SET steps = excluded.steps, "
                 "kcal = excluded.kcal, distance_m = excluded.distance_m, floors = excluded.floors, "
                 "azm = excluded.azm, hours_met = excluded.hours_met, closed_ms = excluded.closed_ms "
                 "WHERE steps IS NOT excluded.steps OR kcal IS NOT excluded.kcal OR distance_m IS NOT excluded.distance_m "
                 "OR floors IS NOT excluded.floors OR azm IS NOT excluded.azm OR hours_met IS NOT excluded.hours_met "
                 "OR closed_ms IS NULL");
    q.bind(1, date.toString(Qt::ISODate)).bind(2, qMax(0, t.steps)).bind(3, qMax(0.0, t.kcal))
            .bind(4, qMax(0.0, t.distanceM)).bind(5, qMax(0, t.floors)).bind(6, qMax(0, t.azm))
            .bind(7, qMax(0, t.hoursMet)).bind(8, closedMs);
    if (!q.ok() || !q.run()) {
        rollback(true);
        return false;
    }
    return commit(before);
}

bool HealthDb::setDailyRestingHr(const QDate &date, int bpm)
{
    if (!date.isValid())
        return false;
    const qint64 before = seq();
    if (!begin())
        return false;
    Stmt q(m_db, "INSERT INTO daily (date, resting_hr) VALUES (?, ?) ON CONFLICT (date) DO UPDATE SET "
                 "resting_hr = excluded.resting_hr WHERE resting_hr IS NOT excluded.resting_hr");
    q.bind(1, date.toString(Qt::ISODate)).bindPositive(2, bpm);
    if (!q.ok() || !q.run()) {
        rollback(true);
        return false;
    }
    return commit(before);
}

bool HealthDb::addSkinTemp(const QVector<QPair<qint64, double>> &minutes)
{
    if (minutes.isEmpty())
        return true;
    const qint64 before = seq();
    if (!begin())
        return false;
    Stmt q(m_db, "INSERT OR IGNORE INTO skin_temp (t_ms, c) VALUES (?, ?)");
    for (const QPair<qint64, double> &m : minutes) {
        q.reset();
        q.bind(1, m.first).bind(2, m.second);
        if (!q.ok() || !q.run()) {
            rollback(true);
            return false;
        }
    }
    return commit(before);
}

bool HealthDb::storeSkinTempNight(const SkinTempNight &n)
{
    if (!n.wakeDate.isValid())
        return false;
    const QString date = n.wakeDate.toString(Qt::ISODate);
    const qint64 before = seq();
    if (!begin())
        return false;
    Stmt q(m_db, "INSERT INTO skin_temp_night (wake_date, start_ms, end_ms, minutes, mean_c, nights, baseline_c, dev_c, "
                 "lo_c, hi_c) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (wake_date) DO UPDATE SET "
                 "start_ms = excluded.start_ms, end_ms = excluded.end_ms, minutes = excluded.minutes, "
                 "mean_c = excluded.mean_c, nights = excluded.nights, baseline_c = excluded.baseline_c, "
                 "dev_c = excluded.dev_c, lo_c = excluded.lo_c, hi_c = excluded.hi_c "
                 "WHERE start_ms IS NOT excluded.start_ms OR end_ms IS NOT excluded.end_ms "
                 "OR minutes IS NOT excluded.minutes OR mean_c IS NOT excluded.mean_c OR nights IS NOT excluded.nights "
                 "OR baseline_c IS NOT excluded.baseline_c OR dev_c IS NOT excluded.dev_c OR lo_c IS NOT excluded.lo_c "
                 "OR hi_c IS NOT excluded.hi_c");
    q.bind(1, date).bind(2, n.startMs).bind(3, n.endMs).bind(4, qMax(0, n.minutes)).bind(6, qMax(0, n.nights));
    if (n.hasMean)
        q.bind(5, n.meanC);
    else
        q.bindNull(5);
    if (n.hasVariation)
        q.bind(7, n.baselineC).bind(8, n.devC).bind(9, n.loC).bind(10, n.hiC);
    else
        q.bindNull(7).bindNull(8).bindNull(9).bindNull(10);
    bool ok = q.ok() && q.run();
    // the day's row has the morning's variation (none: a row is not made for it)
    if (ok && n.hasVariation) {
        Stmt d(m_db, "INSERT INTO daily (date, temp_dev_c) VALUES (?, ?) ON CONFLICT (date) DO UPDATE SET "
                     "temp_dev_c = excluded.temp_dev_c WHERE temp_dev_c IS NOT excluded.temp_dev_c");
        d.bind(1, date).bind(2, n.devC);
        ok = d.ok() && d.run();
    } else if (ok) {
        Stmt d(m_db, "UPDATE daily SET temp_dev_c = NULL WHERE date = ? AND temp_dev_c IS NOT NULL");
        d.bind(1, date);
        ok = d.ok() && d.run();
    }
    if (!ok) {
        rollback(true);
        return false;
    }
    return commit(before);
}

QDate HealthDb::lastClosedDay()
{
    if (!open())
        return QDate();
    Stmt q(m_db, "SELECT MAX(date) FROM daily WHERE closed_ms IS NOT NULL");
    return q.ok() && q.row() ? QDate::fromString(q.s(0), Qt::ISODate) : QDate();
}

// ---- readers ----

qint64 HealthDb::readSeq(const QString &path)
{
    ReadConn c(path);
    Stmt q(c.db(), "SELECT value FROM meta WHERE key = 'seq'");
    return q.ok() && q.row() ? q.i64(0) : 0;
}

QVariant HealthDb::readMeta(const QString &path, const QString &key)
{
    ReadConn c(path);
    Stmt q(c.db(), "SELECT value FROM meta WHERE key = ?");
    q.bind(1, key);
    return q.ok() && q.row() ? q.value(0) : QVariant();
}

QVariantMap HealthDb::readCounts(const QString &path)
{
    QVariantMap out;
    ReadConn c(path);
    if (!c.db())
        return out;
    static const char *const queries[][2] = {
        {"schema", "PRAGMA user_version"},
        {"seq", "SELECT value FROM meta WHERE key = 'seq'"},
        {"workouts", "SELECT COUNT(*) FROM workout WHERE deleted = 0"},
        {"workoutsDeleted", "SELECT COUNT(*) FROM workout WHERE deleted = 1"},
        {"workoutsImported", "SELECT COUNT(*) FROM workout WHERE imported_from IS NOT NULL"},
        {"workoutsOpen", "SELECT COUNT(*) FROM workout WHERE end_ms IS NULL AND deleted = 0"},
        {"trackPoints", "SELECT COUNT(*) FROM workout_track"},
        {"laps", "SELECT COUNT(*) FROM workout_lap"},
        {"pauses", "SELECT COUNT(*) FROM workout_pause"},
        {"nights", "SELECT COUNT(DISTINCT wake_date) FROM sleep_session WHERE deleted = 0 AND main = 1"},
        {"sleepSessions", "SELECT COUNT(*) FROM sleep_session WHERE deleted = 0"},
        {"sleepImported", "SELECT COUNT(*) FROM sleep_session WHERE imported_from IS NOT NULL"},
        {"naps", "SELECT COUNT(*) FROM sleep_session WHERE deleted = 0 AND main = 0"},
        {"nightsV2", "SELECT COUNT(DISTINCT wake_date) FROM sleep_session WHERE deleted = 0 AND main = 1 "
                     "AND algo = 'eos-motion-hr-2'"},
        {"sleepStages", "SELECT COUNT(*) FROM sleep_stage JOIN sleep_session "
                        "ON sleep_session.id = sleep_stage.session_id WHERE sleep_session.deleted = 0"},
        {"days", "SELECT COUNT(*) FROM daily"},
        {"daysClosed", "SELECT COUNT(*) FROM daily WHERE closed_ms IS NOT NULL"},
        {"daysResting", "SELECT COUNT(*) FROM daily WHERE resting_hr IS NOT NULL"},
        {"skinTempMinutes", "SELECT COUNT(*) FROM skin_temp"},
        {"skinTempNights", "SELECT COUNT(*) FROM skin_temp_night"},
        {"skinTempValues", "SELECT COUNT(*) FROM skin_temp_night WHERE mean_c IS NOT NULL"},
        {"skinTempVariations", "SELECT COUNT(*) FROM skin_temp_night WHERE dev_c IS NOT NULL"},
        {"daysTempDev", "SELECT COUNT(*) FROM daily WHERE temp_dev_c IS NOT NULL"},
        {"import.sleep", "SELECT value FROM meta WHERE key = 'import.sleep'"},
        {"import.exercise", "SELECT value FROM meta WHERE key = 'import.exercise'"},
        {"import.resting", "SELECT value FROM meta WHERE key = 'import.resting'"},
        {"import.daily", "SELECT value FROM meta WHERE key = 'import.daily'"},
    };
    for (const auto &q : queries) {
        Stmt s(c.db(), q[1]);
        out.insert(QString::fromLatin1(q[0]), s.ok() && s.row() ? (qlonglong)s.i64(0) : 0LL);
    }
    return out;
}

QList<WorkoutRecord> HealthDb::readWorkouts(const QString &path, qint64 fromMs, qint64 toMs, int limit)
{
    QList<WorkoutRecord> out;
    ReadConn c(path);
    const QByteArray sql = QByteArray("SELECT ") + WORKOUT_COLUMNS
            + " FROM workout WHERE deleted = 0 AND start_ms >= ? AND start_ms < ? ORDER BY start_ms DESC LIMIT ?";
    Stmt q(c.db(), sql.constData());
    q.bind(1, fromMs).bind(2, toMs).bind(3, limit);
    while (q.ok() && q.row())
        out.append(workoutFrom(q));
    return out;
}

bool HealthDb::readWorkout(const QString &path, qint64 id, WorkoutRecord *workout, QVector<LapRecord> *laps,
                           QVector<PauseRecord> *pauses)
{
    ReadConn c(path);
    const QByteArray sql = QByteArray("SELECT ") + WORKOUT_COLUMNS + " FROM workout WHERE id = ?";
    Stmt q(c.db(), sql.constData());
    q.bind(1, id);
    if (!q.ok() || !q.row())
        return false;
    if (workout)
        *workout = workoutFrom(q);
    if (laps) {
        laps->clear();
        Stmt l(c.db(), "SELECT n, start_ms, end_ms, distance_m, avg_bpm, kind FROM workout_lap WHERE workout_id = ? "
                       "ORDER BY n");
        l.bind(1, id);
        while (l.ok() && l.row()) {
            LapRecord r;
            r.n = l.i(0);
            r.startMs = l.i64(1);
            r.endMs = l.i64(2);
            r.distanceM = l.d(3);
            r.avgBpm = l.i(4);
            r.kind = l.s(5);
            laps->append(r);
        }
    }
    if (pauses) {
        pauses->clear();
        Stmt p(c.db(), "SELECT start_ms, end_ms, auto FROM workout_pause WHERE workout_id = ? ORDER BY start_ms");
        p.bind(1, id);
        while (p.ok() && p.row())
            pauses->append({p.i64(0), p.isNull(1) ? 0 : p.i64(1), p.i(2) != 0});
    }
    return true;
}

bool HealthDb::readOpenWorkout(const QString &path, qint64 beforeMs, WorkoutRecord *workout, qint64 *savedMs,
                               QString *state, bool *paused)
{
    ReadConn c(path);
    const QByteArray sql = QByteArray("SELECT ") + WORKOUT_COLUMNS
            + ", (SELECT saved_ms FROM workout_state WHERE workout_id = workout.id)"
              ", (SELECT state FROM workout_state WHERE workout_id = workout.id)"
              ", EXISTS (SELECT 1 FROM workout_pause WHERE workout_id = workout.id AND end_ms IS NULL)"
              " FROM workout WHERE end_ms IS NULL AND deleted = 0 AND start_ms < ? ORDER BY start_ms DESC LIMIT 1";
    Stmt q(c.db(), sql.constData());
    q.bind(1, beforeMs);
    if (!q.ok() || !q.row())
        return false;
    if (workout)
        *workout = workoutFrom(q);
    if (savedMs)
        *savedMs = q.isNull(21) ? 0 : q.i64(21);
    if (state)
        *state = q.s(22);
    if (paused)
        *paused = q.i(23) != 0;
    return true;
}

QVector<TrackPoint> HealthDb::readTrack(const QString &path, qint64 id)
{
    QVector<TrackPoint> out;
    ReadConn c(path);
    Stmt q(c.db(), "SELECT t_ms, lat_e7, lon_e7, alt_dm, hacc_dm, speed_cms FROM workout_track WHERE workout_id = ? "
                   "ORDER BY t_ms");
    q.bind(1, id);
    while (q.ok() && q.row()) {
        TrackPoint p;
        p.tMs = q.i64(0);
        p.latE7 = (qint32)q.i64(1);
        p.lonE7 = (qint32)q.i64(2);
        p.altDm = q.i(3);
        p.haccDm = q.i(4);
        p.speedCms = q.i(5);
        out.append(p);
    }
    return out;
}

bool HealthDb::readNight(const QString &path, const QDate &wakeDate, SleepNight *night)
{
    ReadConn c(path);
    Stmt q(c.db(), "SELECT main, start_ms, end_ms, asleep_min, awake_min, rem_min, light_min, deep_min, avg_hr, score, "
                   "goal_min, id, algo FROM sleep_session WHERE wake_date = ? AND deleted = 0 "
                   "ORDER BY main DESC, start_ms");
    Stmt st(c.db(), "SELECT start_ms, end_ms, stage FROM sleep_stage WHERE session_id = ? ORDER BY start_ms");
    q.bind(1, wakeDate.toString(Qt::ISODate));
    SleepNight n;
    n.wakeDate = wakeDate;
    while (q.ok() && q.row()) {
        SleepSession s;
        s.startMs = q.i64(1);
        s.endMs = q.i64(2);
        s.asleepMin = q.i(3);
        s.awakeMin = q.i(4);
        s.remMin = q.i(5);
        s.lightMin = q.i(6);
        s.deepMin = q.i(7);
        s.avgHr = q.d(8);
        if (st.ok()) {
            st.reset();
            st.bind(1, q.i64(11));
            while (st.row()) {
                SleepStageSpan span;
                span.startMs = st.i64(0);
                span.endMs = st.i64(1);
                span.stage = st.i(2);
                s.stages.append(span);
            }
        }
        if (q.i(0) != 0 && !n.valid) {
            n.valid = true;
            n.main = s;
            n.score = q.i(9);
            n.goalMin = q.isNull(10) ? 480 : q.i(10);
            n.algo = q.s(12);
        } else {
            n.naps.append(s);
            n.napMin += s.asleepMin;
        }
    }
    if (!n.valid) {
        // naps without a night (R5 E-30): no night, but its naps for a caller that asks
        if (night)
            *night = n;
        return false;
    }
    // what the estimator derives from these (sleepEstimator.cpp)
    n.scoreLabel = sleepScoreLabel(n.score);
    n.goalMet = n.main.asleepMin >= n.goalMin;
    if (night)
        *night = n;
    return true;
}

bool HealthDb::readDaily(const QString &path, const QDate &date, DailyRecord *record)
{
    ReadConn c(path);
    const QByteArray sql = QByteArray("SELECT ") + DAILY_COLUMNS + " FROM daily WHERE date = ?";
    Stmt q(c.db(), sql.constData());
    q.bind(1, date.toString(Qt::ISODate));
    if (!q.ok() || !q.row())
        return false;
    if (record)
        *record = dailyFrom(q);
    return true;
}

QList<DailyRecord> HealthDb::readDailyRange(const QString &path, const QDate &from, const QDate &to)
{
    QList<DailyRecord> out;
    ReadConn c(path);
    const QByteArray sql = QByteArray("SELECT ") + DAILY_COLUMNS + " FROM daily WHERE date >= ? AND date <= ? ORDER BY date";
    Stmt q(c.db(), sql.constData());
    q.bind(1, from.toString(Qt::ISODate)).bind(2, to.toString(Qt::ISODate));
    while (q.ok() && q.row())
        out.append(dailyFrom(q));
    return out;
}

QVector<QPair<qint64, double>> HealthDb::readSkinTemp(const QString &path, qint64 fromMs, qint64 toMs)
{
    QVector<QPair<qint64, double>> out;
    ReadConn c(path);
    Stmt q(c.db(), "SELECT t_ms, c FROM skin_temp WHERE t_ms >= ? AND t_ms < ? ORDER BY t_ms");
    q.bind(1, fromMs).bind(2, toMs);
    while (q.ok() && q.row())
        out.append(qMakePair(q.i64(0), q.d(1)));
    return out;
}

QList<SkinTempNight> HealthDb::readSkinTempNights(const QString &path, const QDate &from, const QDate &to)
{
    QList<SkinTempNight> out;
    ReadConn c(path);
    const QByteArray sql = QByteArray("SELECT ") + SKIN_NIGHT_COLUMNS +
                           " FROM skin_temp_night WHERE wake_date >= ? AND wake_date <= ? ORDER BY wake_date";
    Stmt q(c.db(), sql.constData());
    q.bind(1, from.toString(Qt::ISODate)).bind(2, to.toString(Qt::ISODate));
    while (q.ok() && q.row())
        out.append(skinNightFrom(q));
    return out;
}

ChangePage HealthDb::changes(const QString &path, const QString &table, qint64 sinceSeq, int max)
{
    ChangePage page;
    page.lastSeq = sinceSeq;
    bool known = false;
    for (const Keyed &k : MUTABLE)
        known = known || table == QLatin1String(k.table);
    if (!known || max < 1)
        return page;
    ReadConn c(path);
    const QByteArray sql = "SELECT * FROM " + table.toLatin1() + " WHERE seq > ? ORDER BY seq LIMIT ?";
    Stmt q(c.db(), sql.constData());
    q.bind(1, sinceSeq).bind(2, max + 1);
    while (q.ok() && q.row()) {
        if (page.rows.size() == max) {
            page.more = true;
            break;
        }
        QVariantMap row;
        for (int col = 0; col < q.columns(); col++)
            row.insert(q.name(col), q.value(col));
        page.lastSeq = row.value("seq").toLongLong();
        page.rows.append(row);
    }
    return page;
}

} // namespace Health
