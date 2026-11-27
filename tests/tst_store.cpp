// The health database (store/healthDb.*) and its one-time import (store/healthImport.*): the schema and its
// version, the change sequence (every change counted, also across restarts; children change their parent),
// tombstones, the workout, night and day writers, the readers (never make the file, see the last commit while a
// write is open, do not block one), a process killed inside a transaction, and the import of the old files:
// every record in, the same answers out, twice is once, an interrupted import resumes; version 2: the snapshot
// of a workout that is on (WO-02) and the upgrade of a database of version 1. All data is invented.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sqlite3.h>

#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTest>

#include "../common.h"
#include "../daemon/sensorPlugins/heartrateDb.h"
#include "../metrics/healthData.h"
#include "../metrics/restingHr.h"
#include "../store/healthDb.h"
#include "../store/healthImport.h"
#include "testenv.h"

using namespace Health;

namespace {

const QDate MON(2026, 11, 9);

qint64 ms(const QDate &d, int h, int m = 0, int s = 0) { return QDateTime(d, QTime(h, m, s)).toMSecsSinceEpoch(); }

// one number from a connection of the test's own
qint64 sqlNumber(const QString &path, const char *sql)
{
    sqlite3 *db = nullptr;
    qint64 v = -1;
    if (sqlite3_open_v2(QFile::encodeName(path).constData(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return v;
}

QString sqlText(const QString &path, const char *sql)
{
    sqlite3 *db = nullptr;
    QString v;
    if (sqlite3_open_v2(QFile::encodeName(path).constData(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            v = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(st, 0)));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return v;
}

SleepSession session(qint64 startMs, int asleep, int awake, int rem, int light, int deep, double avgHr)
{
    SleepSession s;
    s.startMs = startMs;
    s.endMs = startMs + (qint64)(asleep + awake) * 60000;
    s.asleepMin = asleep;
    s.awakeMin = awake;
    s.remMin = rem;
    s.lightMin = light;
    s.deepMin = deep;
    s.avgHr = avgHr;
    return s;
}

// a night as the estimator makes one (its derived fields set as it sets them)
SleepNight night(const QDate &wake, int asleep, int score, bool nap = false)
{
    SleepNight n;
    n.valid = true;
    n.wakeDate = wake;
    n.main = session(ms(wake.addDays(-1), 23, 10), asleep, 25, asleep / 5, asleep * 3 / 5, asleep / 5, 55.25);
    if (nap) {
        n.naps.append(session(ms(wake, 14, 0), 40, 5, 0, 30, 10, 58.5));
        n.napMin = 40;
    }
    n.score = score;
    n.scoreLabel = sleepScoreLabel(score);
    n.goalMin = 480;
    n.goalMet = asleep >= 480;
    return n;
}

// the nights of a sensorlogd from before health.db: sleep/<wake date>.json, as it wrote them
void writeSleepFile(const SleepNight &n)
{
    QDir().mkpath(testenv::root() + "sleep");
    QFile f(testenv::root() + "sleep/" + n.wakeDate.toString("yyyy-MM-dd") + ".json");
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(n.toJson()).toJson(QJsonDocument::Compact));
}

// the Exercise app's log line: start s:active s:type:distance m:kcal:avg bpm:max bpm:steps
QByteArray workoutLine(qint64 startMs, int activeS, const char *type, int distanceM, int kcal, int avg, int max, int steps)
{
    return QByteArray::number(startMs / 1000) + ":" + QByteArray::number(activeS) + ":" + type + ":"
            + QByteArray::number(distanceM) + ":" + QByteArray::number(kcal) + ":" + QByteArray::number(avg) + ":"
            + QByteArray::number(max) + ":" + QByteArray::number(steps) + "\n";
}

void appendExercise(const QDate &day, const QByteArray &lines)
{
    QDir().mkpath(testenv::root() + "exercise");
    QFile f(testenv::root() + "exercise/" + day.toString("yyyy-MM-dd") + ".log");
    QVERIFY(f.open(QIODevice::Append));
    f.write(lines);
}

void addHeart(const QDate &d, int h, int m, int minutes, int bpm)
{
    HeartrateDb db(heartrateDbPath());
    QVector<HeartrateDb::Reading> rows;
    for (int i = 0; i < minutes; i++)
        rows.append({ms(d, h, m) + (qint64)i * 60000 + 30000, bpm, 4});
    QVERIFY(db.add(rows));
}

QByteArray rec(qint64 timeMs, int value) { return QByteArray::number(timeMs / 1000) + ":" + QByteArray::number(value) + "\n"; }

// what the daemon closes a day with, for the import (a fixed profile and resting rate)
bool totalsOf(const QDate &d, DailyTotals *t)
{
    if (!hasDataOn(d))
        return false;
    const DaySummary s = summarizeDay(d, HealthProfile(), 60, ms(MON.addDays(30), 12));
    t->steps = s.steps;
    t->kcal = s.energyKcal;
    t->distanceM = s.distanceM;
    t->floors = s.floors;
    t->azm = s.zones.azm;
    t->hoursMet = s.hourly.hoursMet;
    return true;
}

// the resting rate of a past day, as the daemon estimates it (the night's window if a night is stored for it)
int restingOf(const QDate &d)
{
    SleepNight n;
    return estimateRestingHrForDay(d, HealthDb::readNight(healthDbPath(), d, &n) ? n.main.endMs : 0);
}

// the old files of a watch: 3 nights (one with a nap), one torn night file, 4 workouts on 3 days (one torn line),
// steps on 3 days and heart rate on 3 (one resting rate in them)
void makeOldFiles()
{
    writeSleepFile(night(MON, 420, 78));
    writeSleepFile(night(MON.addDays(1), 485, 91, true));
    writeSleepFile(night(MON.addDays(3), 300, 55));
    QFile torn(testenv::root() + "sleep/" + MON.addDays(2).toString("yyyy-MM-dd") + ".json");
    QVERIFY(torn.open(QIODevice::WriteOnly));
    torn.write("{\"valid\":true,\"wakeDa");
    torn.close();
    appendExercise(MON, workoutLine(ms(MON, 7, 30), 1800, "walk", 2400, 160, 112, 131, 3300)
                        + workoutLine(ms(MON, 18, 0), 2700, "run", 6100, 420, 151, 172, 7600));
    appendExercise(MON.addDays(1), workoutLine(ms(MON.addDays(1), 12, 5), 900, "bike", 0, 95, 125, 140, 0));
    appendExercise(MON.addDays(2), workoutLine(ms(MON.addDays(2), 6, 45), 1200, "walk", 1500, 90, 98, 110, 2000)
                                    + QByteArray("1762800000:12"));
    for (int i = 0; i < 3; i++) {
        const QDate d = MON.addDays(i);
        testenv::writeDay("stepCounter", d, rec(ms(d, 9, 9), 300 + i) + rec(ms(d, 14, 10), 1100 + 10 * i));
    }
    addHeart(MON, 7, 0, 30, 118);
    addHeart(MON.addDays(1), 1, 0, 120, 54);
    addHeart(MON.addDays(4), 10, 0, 20, 70);
}

}

class TestStore : public QObject
{
    Q_OBJECT

    QString path() const { return healthDbPath(); }

private slots:
    void init() { testenv::clean(); }

    // ---- the schema ----
    void theSchemaIsMadeOnceWithItsVersion()
    {
        QVERIFY(!QFile::exists(path()));
        {
            HealthDb db(path());
            QVERIFY(db.isOpen());
            QCOMPARE(db.seq(), qint64(0));
        }
        QCOMPARE(sqlNumber(path(), "PRAGMA user_version"), qint64(HealthDb::SCHEMA_VERSION));
        QCOMPARE(sqlText(path(), "PRAGMA journal_mode"), QString("wal"));
        for (const char *table : {"meta", "workout", "workout_track", "workout_lap", "workout_pause", "workout_state",
                                  "sleep_session", "sleep_stage", "daily", "spo2", "hrv", "skin_temp", "eda_event",
                                  "skin_temp_night"}) {
            const QByteArray q = QByteArray("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = '")
                    + table + "'";
            QVERIFY2(sqlNumber(path(), q.constData()) == 1, table);
        }
        const qint64 triggers = sqlNumber(path(), "SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger'");
        QCOMPARE(triggers, qint64(5 * 2 + 4 * 3));
        // opened again: nothing is made twice
        const QString before = sqlText(path(), "SELECT group_concat(name) FROM sqlite_master");
        { HealthDb again(path()); QVERIFY(again.isOpen()); }
        QCOMPARE(sqlText(path(), "SELECT group_concat(name) FROM sqlite_master"), before);
    }

    void aNewerSchemaIsLeftAsItIs()
    {
        { HealthDb db(path()); QVERIFY(db.beginWorkout(ms(MON, 8), "walk") > 0); }
        sqlite3 *raw = nullptr;
        QCOMPARE(sqlite3_open(QFile::encodeName(path()).constData(), &raw), SQLITE_OK);
        QCOMPARE(sqlite3_exec(raw, "PRAGMA user_version=4", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("has schema 4"));
        HealthDb db(path());
        QVERIFY(db.isOpen());
        QCOMPARE(sqlNumber(path(), "PRAGMA user_version"), qint64(4));
        QVERIFY(db.beginWorkout(ms(MON, 9), "run") > 0); // still written
    }

    void aDamagedFileIsKeptAsideAndANewOneBegun()
    {
        QDir().mkpath(testenv::root());
        QFile bad(path());
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write(QByteArray(8192, 'x'));
        bad.close();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("cannot be used"));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("moved to"));
        HealthDb db(path());
        QVERIFY(db.isOpen());
        QVERIFY(db.beginWorkout(ms(MON, 8), "walk") > 0);
        const QStringList aside = QDir(testenv::root()).entryList({"health.db.broken-*"});
        QCOMPARE(aside.size(), 1);
        QFile kept(testenv::root() + aside.first());
        QCOMPARE(kept.size(), qint64(8192));
    }

    // ---- the change sequence ----
    void theSequenceCountsEveryChange()
    {
        HealthDb db(path());
        QList<qint64> told;
        db.setChangeHandler([&](qint64 s) { told << s; });
        const qint64 id = db.beginWorkout(ms(MON, 8), "walk", "manual", true);
        QVERIFY(id > 0);
        QCOMPARE(db.seq(), qint64(1));
        QCOMPARE(HealthDb::readSeq(path()), qint64(1));
        WorkoutRecord w;
        QVERIFY(HealthDb::readWorkout(path(), id, &w));
        QCOMPARE(w.seq, qint64(1));
        // a child row changes its parent
        LapRecord lap;
        lap.n = 1;
        lap.startMs = ms(MON, 8);
        lap.endMs = ms(MON, 8, 6);
        lap.distanceM = 1000;
        lap.kind = "split-km";
        QVERIFY(db.addLap(id, lap));
        QVERIFY(HealthDb::readWorkout(path(), id, &w));
        QCOMPARE(w.seq, db.seq());
        const qint64 afterLap = db.seq();
        QVERIFY(afterLap > 1);
        QVERIFY(db.setPaused(id, ms(MON, 8, 10), true));
        QVERIFY(db.setPaused(id, ms(MON, 8, 12), false));
        WorkoutRecord summary;
        summary.activeS = 1500;
        summary.kcal = 120;
        QVERIFY(db.endWorkout(id, ms(MON, 8, 30), summary));
        QVERIFY(HealthDb::readWorkout(path(), id, &w));
        QCOMPARE(w.seq, db.seq());
        QVERIFY(w.seq > afterLap);
        // the other tables count on the same counter
        const qint64 s1 = db.seq();
        QVERIFY(db.storeNight(night(MON, 420, 78), "t"));
        QVERIFY(db.seq() > s1);
        const qint64 s2 = db.seq();
        DailyTotals t;
        t.steps = 5000;
        QVERIFY(db.setDailyTotals(MON, t, ms(MON.addDays(1), 0, 30)));
        QVERIFY(db.seq() > s2);
        // nothing changed: nothing counted, nobody told
        const qint64 s3 = db.seq();
        const int toldBefore = told.size();
        QVERIFY(db.setDailyTotals(MON, t, ms(MON.addDays(1), 1, 0)));
        QCOMPARE(db.seq(), s3);
        QCOMPARE(told.size(), toldBefore);
        // every write that changed something told the counter after it, in order
        QCOMPARE(told.last(), db.seq());
        for (int i = 1; i < told.size(); i++)
            QVERIFY(told[i] > told[i - 1]);
        // the counter is the highest seq of any row
        QCOMPARE(sqlNumber(path(), "SELECT MAX(m) FROM (SELECT MAX(seq) m FROM workout UNION ALL SELECT MAX(seq) FROM "
                                   "sleep_session UNION ALL SELECT MAX(seq) FROM daily)"), db.seq());
    }

    void aHoldTellsOnceAtTheEnd()
    {
        HealthDb db(path());
        QList<qint64> told;
        db.setChangeHandler([&](qint64 s) { told << s; });
        db.holdChanges();
        db.holdChanges(); // nested
        QVERIFY(db.beginWorkout(ms(MON, 8), "walk") > 0);
        QVERIFY(db.setDailyRestingHr(MON, 55));
        db.releaseChanges();
        QVERIFY(told.isEmpty());
        db.releaseChanges();
        QCOMPARE(told, QList<qint64>({2}));
        db.releaseChanges(); // not held: nothing
        db.holdChanges();
        db.releaseChanges(); // nothing written: nobody told
        QCOMPARE(told.size(), 1);
    }

    void theSequenceSurvivesARestart()
    {
        qint64 last = 0;
        {
            HealthDb db(path());
            for (int i = 0; i < 5; i++)
                QVERIFY(db.beginWorkout(ms(MON, 8 + i), "walk") > 0);
            last = db.seq();
        }
        QCOMPARE(last, qint64(5));
        HealthDb again(path());
        QCOMPARE(again.seq(), last);
        const qint64 id = again.beginWorkout(ms(MON, 20), "run");
        WorkoutRecord w;
        QVERIFY(HealthDb::readWorkout(path(), id, &w));
        QCOMPARE(w.seq, last + 1);
    }

    void changesArePagedInSeqOrder()
    {
        HealthDb db(path());
        QList<qint64> ids;
        for (int i = 0; i < 5; i++)
            ids << db.beginWorkout(ms(MON, 8 + i), "walk");
        ChangePage p = HealthDb::changes(path(), "workout", 0, 3);
        QCOMPARE(p.rows.size(), 3);
        QVERIFY(p.more);
        QCOMPARE(p.rows[0]["id"].toLongLong(), ids[0]);
        QCOMPARE(p.lastSeq, p.rows[2]["seq"].toLongLong());
        p = HealthDb::changes(path(), "workout", p.lastSeq, 3);
        QCOMPARE(p.rows.size(), 2);
        QVERIFY(!p.more);
        const qint64 seen = p.lastSeq;
        QCOMPARE(HealthDb::changes(path(), "workout", seen).rows.size(), 0);
        // a change brings the row back once, with its new seq
        QVERIFY(db.deleteWorkout(ids[1]));
        p = HealthDb::changes(path(), "workout", seen);
        QCOMPARE(p.rows.size(), 1);
        QCOMPARE(p.rows[0]["id"].toLongLong(), ids[1]);
        QCOMPARE(p.rows[0]["deleted"].toLongLong(), qint64(1));
        QVERIFY(p.rows[0]["seq"].toLongLong() > seen);
        // other tables, and none that is not one of them
        QVERIFY(db.setDailyRestingHr(MON, 58));
        QCOMPARE(HealthDb::changes(path(), "daily", 0).rows.size(), 1);
        QCOMPARE(HealthDb::changes(path(), "daily", 0).rows[0]["date"].toString(), MON.toString(Qt::ISODate));
        QCOMPARE(HealthDb::changes(path(), "meta", 0).rows.size(), 0);
        QCOMPARE(HealthDb::changes(path(), "workout; DROP TABLE workout", 0).rows.size(), 0);
    }

    // ---- workouts ----
    void aDeletedWorkoutIsATombstone()
    {
        HealthDb db(path());
        const qint64 id = db.beginWorkout(ms(MON, 8), "run", "manual", true);
        QVERIFY(db.addTrackPoints(id, {{ms(MON, 8, 0, 1), 123456789, -45678901, 120, 50, 280},
                                       {ms(MON, 8, 0, 2), 123456799, -45678911, 121, 50, 290}}));
        LapRecord lap;
        lap.n = 1;
        lap.startMs = ms(MON, 8);
        lap.endMs = ms(MON, 8, 5);
        QVERIFY(db.addLap(id, lap));
        QCOMPARE(HealthDb::readTrack(path(), id).size(), 2);
        const qint64 before = db.seq();
        QVERIFY(db.deleteWorkout(id));
        QVERIFY(db.seq() > before);
        QVERIFY(!db.deleteWorkout(id)); // once
        // the row stays, marked, for a sync; what was recorded in it goes
        WorkoutRecord w;
        QVERIFY(HealthDb::readWorkout(path(), id, &w));
        QVERIFY(w.deleted);
        QCOMPARE(HealthDb::readTrack(path(), id).size(), 0);
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM workout_lap"), qint64(0));
        QCOMPARE(HealthDb::readWorkouts(path(), 0, ms(MON.addDays(1), 0)).size(), 0);
        // the old log's line of the same workout does not bring it back
        WorkoutRecord old;
        old.startMs = ms(MON, 8);
        old.endMs = ms(MON, 9);
        old.type = "run";
        QVERIFY(!db.importWorkout(old));
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM workout"), qint64(1));
        // nothing more can be added to it
        QVERIFY(!db.addTrackPoints(id, {{ms(MON, 8, 1), 1, 1, 0, 0, 0}}));
        QVERIFY(!db.setPaused(id, ms(MON, 8, 2), true));
    }

    void aWorkoutBeginsPausesAndEnds()
    {
        HealthDb db(path());
        const qint64 id = db.beginWorkout(ms(MON, 17), "run", "auto", true);
        QVERIFY(id > 0);
        WorkoutRecord w;
        QVERIFY(HealthDb::readWorkout(path(), id, &w));
        QCOMPARE(w.endMs, qint64(0)); // on
        QCOMPARE(w.source, QString("auto"));
        QVERIFY(w.gps);
        QVERIFY(db.setPaused(id, ms(MON, 17, 10), true, true));
        QVERIFY(!db.setPaused(id, ms(MON, 17, 11), true)); // paused already
        QVERIFY(db.setPaused(id, ms(MON, 17, 12), false));
        QVERIFY(!db.setPaused(id, ms(MON, 17, 13), false)); // not paused
        QVERIFY(db.setPaused(id, ms(MON, 17, 40), true));
        // fixes: the ones that are no position are left out
        QVERIFY(db.addTrackPoints(id, {{ms(MON, 17, 1), 100000000, 200000000, 15, 30, 300},
                                       {ms(MON, 17, 2), 950000000, 0, 0, 0, 0},
                                       {ms(MON, 17, 3), 100000100, 200000100, 16, 30, 310}}));
        QCOMPARE(HealthDb::readTrack(path(), id).size(), 2);
        LapRecord lap;
        lap.n = 1;
        lap.startMs = ms(MON, 17);
        lap.endMs = ms(MON, 17, 5);
        lap.distanceM = 1000;
        lap.avgBpm = 150;
        lap.kind = "split-km";
        QVERIFY(db.addLap(id, lap));
        lap.kind = "lap";
        QVERIFY(!db.addLap(id, lap)); // not a kind
        WorkoutRecord s;
        s.activeS = 2400;
        s.distanceM = 6100.5;
        s.kcal = 412.25;
        s.steps = 7400;
        s.avgBpm = 151;
        s.maxBpm = 175;
        s.moderateS = 600;
        s.vigorousS = 1500;
        s.peakS = 300;
        s.azm = 70;
        s.elevGainM = 12.5;
        QVERIFY(!db.endWorkout(id, ms(MON, 16), s)); // before its start
        QVERIFY(db.endWorkout(id, ms(MON, 17, 45), s));
        QVERIFY(!db.endWorkout(id, ms(MON, 17, 50), s)); // once
        QVector<LapRecord> laps;
        QVector<PauseRecord> pauses;
        QVERIFY(HealthDb::readWorkout(path(), id, &w, &laps, &pauses));
        QCOMPARE(w.endMs, ms(MON, 17, 45));
        QCOMPARE(w.activeS, 2400);
        QCOMPARE(w.distanceM, 6100.5);
        QCOMPARE(w.kcal, 412.25);
        QCOMPARE(w.azm, 70);
        QCOMPARE(w.elevGainM, 12.5);
        QCOMPARE(laps.size(), 1);
        QCOMPARE(laps[0].kind, QString("split-km"));
        QCOMPARE(pauses.size(), 2);
        QVERIFY(pauses[0].automatic);
        QCOMPARE(pauses[0].endMs, ms(MON, 17, 12));
        QCOMPARE(pauses[1].endMs, ms(MON, 17, 45)); // the open pause ended with the workout
        QVERIFY(!db.setPaused(id, ms(MON, 17, 46), true)); // over
        QCOMPARE(HealthDb::readWorkouts(path(), ms(MON, 0), ms(MON.addDays(1), 0)).size(), 1);
    }

    void badWorkoutValuesAreRefused()
    {
        HealthDb db(path());
        QCOMPARE(db.beginWorkout(0, "walk"), qint64(0));
        QCOMPARE(db.beginWorkout(ms(MON, 8), ""), qint64(0));
        QCOMPARE(db.beginWorkout(ms(MON, 8), "walk:1"), qint64(0));
        QCOMPARE(db.beginWorkout(ms(MON, 8), "walk", "phone"), qint64(0));
        QVERIFY(db.beginWorkout(ms(MON, 8), "walk") > 0);
        QCOMPARE(db.beginWorkout(ms(MON, 8), "run"), qint64(0)); // that start is taken
        QVERIFY(!db.addTrackPoints(999, {}));
        QVERIFY(!db.endWorkout(999, ms(MON, 9), WorkoutRecord()));
        QVERIFY(!db.deleteWorkout(999));
        QCOMPARE(db.seq(), qint64(1));
    }

    // ---- nights and days ----
    void aNightReplacesTheMorningsSessions()
    {
        HealthDb db(path());
        const SleepNight first = night(MON, 420, 78, true);
        QVERIFY(db.storeNight(first, "eos-steps-hr-1"));
        SleepNight read;
        QVERIFY(HealthDb::readNight(path(), MON, &read));
        QCOMPARE(read.toMap(), first.toMap());
        // estimated again: the morning's sessions are tombstoned, the new ones in
        const SleepNight second = night(MON, 455, 84);
        QVERIFY(db.storeNight(second, "eos-steps-hr-2"));
        QVERIFY(HealthDb::readNight(path(), MON, &read));
        QCOMPARE(read.toMap(), second.toMap());
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM sleep_session WHERE deleted = 1"), qint64(2));
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM sleep_session WHERE deleted = 0"), qint64(1));
        // onlyIfNone (the import): a morning with any session is left alone
        QVERIFY(!db.storeNight(first, "x", "sleep/f.json", true));
        QVERIFY(HealthDb::readNight(path(), MON, &read));
        QCOMPARE(read.main.asleepMin, 455);
        QVERIFY(!HealthDb::readNight(path(), MON.addDays(1), &read));
        QVERIFY(!db.storeNight(SleepNight(), "x")); // not a night
    }

    void aDayIsClosedOnce()
    {
        HealthDb db(path());
        QVERIFY(!db.lastClosedDay().isValid());
        // the morning's resting rate may come first, the totals at the close
        QVERIFY(db.setDailyRestingHr(MON, 57));
        DailyRecord r;
        QVERIFY(HealthDb::readDaily(path(), MON, &r));
        QVERIFY(!r.closed);
        QCOMPARE(r.restingHr, 57);
        DailyTotals t;
        t.steps = 8200;
        t.kcal = 2210.5;
        t.distanceM = 5950.25;
        t.floors = 4;
        t.azm = 22;
        t.hoursMet = 7;
        QVERIFY(db.setDailyTotals(MON, t, ms(MON.addDays(1), 0, 40)));
        QVERIFY(HealthDb::readDaily(path(), MON, &r));
        QVERIFY(r.closed);
        QCOMPARE(r.restingHr, 57);
        QCOMPARE(r.totals.steps, 8200);
        QCOMPARE(r.totals.kcal, 2210.5);
        QCOMPARE(r.totals.distanceM, 5950.25);
        QCOMPARE(r.totals.hoursMet, 7);
        QCOMPARE(r.closedMs, ms(MON.addDays(1), 0, 40));
        QCOMPARE(db.lastClosedDay(), MON);
        QVERIFY(db.setDailyTotals(MON.addDays(2), t, ms(MON.addDays(3), 1)));
        QCOMPARE(db.lastClosedDay(), MON.addDays(2));
        QCOMPARE(HealthDb::readDailyRange(path(), MON, MON.addDays(6)).size(), 2);
        // a resting rate of 0 is none
        QVERIFY(db.setDailyRestingHr(MON, 0));
        QVERIFY(HealthDb::readDaily(path(), MON, &r));
        QCOMPARE(r.restingHr, 0);
    }

    // ---- readers ----
    void readersNeverMakeTheFile()
    {
        QCOMPARE(HealthDb::readSeq(path()), qint64(0));
        QCOMPARE(HealthDb::readWorkouts(path(), 0, ms(MON, 0)).size(), 0);
        QVERIFY(!HealthDb::readNight(path(), MON, nullptr));
        QVERIFY(!HealthDb::readDaily(path(), MON, nullptr));
        QCOMPARE(HealthDb::changes(path(), "workout", 0).rows.size(), 0);
        QVERIFY(!QFile::exists(path()));
        // a file without the schema: nothing either, and it is not given one
        sqlite3 *raw = nullptr;
        QDir().mkpath(testenv::root());
        QCOMPARE(sqlite3_open(QFile::encodeName(path()).constData(), &raw), SQLITE_OK);
        QCOMPARE(sqlite3_exec(raw, "CREATE TABLE other (x)", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);
        QCOMPARE(HealthDb::readWorkouts(path(), 0, ms(MON, 0)).size(), 0);
        QCOMPARE(sqlNumber(path(), "PRAGMA user_version"), qint64(0));
    }

    void aReaderDuringAWriteSeesTheLastCommit()
    {
        HealthDb db(path());
        QVERIFY(db.beginWorkout(ms(MON, 8), "walk") > 0);
        // another writer's transaction is open (as the daemon's is while it writes a night)
        sqlite3 *w = nullptr;
        QCOMPARE(sqlite3_open(QFile::encodeName(path()).constData(), &w), SQLITE_OK);
        QCOMPARE(sqlite3_exec(w, "BEGIN IMMEDIATE; INSERT INTO workout (start_ms, type) VALUES (1, 'x')", nullptr,
                              nullptr, nullptr), SQLITE_OK);
        QElapsedTimer t;
        t.start();
        QCOMPARE(HealthDb::readWorkouts(path(), 0, ms(MON.addDays(1), 0)).size(), 1); // the committed one only
        QCOMPARE(HealthDb::readSeq(path()), qint64(1));
        QVERIFY(t.elapsed() < 1500); // not waiting for the writer
        QCOMPARE(sqlite3_exec(w, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(w);
        QCOMPARE(HealthDb::readWorkouts(path(), 0, ms(MON.addDays(1), 0)).size(), 2);
        // a reader as asteroid-btsyncd opens one (read-write, no create) holds a read transaction: writes go on
        sqlite3 *r = nullptr;
        QCOMPARE(sqlite3_open_v2(QFile::encodeName(path()).constData(), &r, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
        sqlite3_stmt *st = nullptr;
        QCOMPARE(sqlite3_exec(r, "BEGIN", nullptr, nullptr, nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_prepare_v2(r, "SELECT COUNT(*) FROM workout", -1, &st, nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_step(st), SQLITE_ROW);
        QCOMPARE(sqlite3_column_int(st, 0), 2);
        t.restart();
        QVERIFY(db.beginWorkout(ms(MON, 9), "run") > 0);
        QVERIFY(t.elapsed() < 1500);
        sqlite3_reset(st);
        QCOMPARE(sqlite3_step(st), SQLITE_ROW);
        QCOMPARE(sqlite3_column_int(st, 0), 2); // its snapshot
        sqlite3_finalize(st);
        sqlite3_exec(r, "COMMIT", nullptr, nullptr, nullptr);
        sqlite3_close(r);
        QCOMPARE(HealthDb::readWorkouts(path(), 0, ms(MON.addDays(1), 0)).size(), 3);
    }

    void aKillInsideATransactionLosesOnlyIt()
    {
        { HealthDb db(path()); QVERIFY(db.beginWorkout(ms(MON, 8), "walk") > 0); }
        const pid_t child = fork();
        QVERIFY(child >= 0);
        if (child == 0) {
            // the daemon, killed while it writes: one workout committed, then a transaction half done
            HealthDb db(healthDbPath());
            db.beginWorkout(ms(MON, 9), "run");
            sqlite3 *raw = nullptr;
            sqlite3_open(QFile::encodeName(healthDbPath()).constData(), &raw);
            sqlite3_exec(raw, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
            for (int i = 0; i < 200; i++) {
                const QByteArray sql = "INSERT INTO workout (start_ms, type) VALUES (" + QByteArray::number(1000 + i)
                        + ", 'x')";
                sqlite3_exec(raw, sql.constData(), nullptr, nullptr, nullptr);
            }
            raise(SIGKILL);
            _exit(1);
        }
        int status = 0;
        QCOMPARE(waitpid(child, &status, 0), child);
        QVERIFY(WIFSIGNALED(status));
        HealthDb db(path()); // WAL recovery
        QVERIFY(db.isOpen());
        QCOMPARE(sqlText(path(), "PRAGMA integrity_check"), QString("ok"));
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM workout"), qint64(2));
        QCOMPARE(db.seq(), qint64(2));
        QVERIFY(db.beginWorkout(ms(MON, 10), "bike") > 0);
        QCOMPARE(db.seq(), qint64(3));
    }

    // ---- the one-time import ----
    void theImportTakesEveryOldRecordWithTheSameAnswers()
    {
        makeOldFiles();
        // the answers before: from the files
        QMap<QDate, QVariantMap> nightsBefore;
        for (const QDate &d : {MON, MON.addDays(1), MON.addDays(3)}) {
            SleepNight n;
            QVERIFY(loadStoredSleep(d, &n));
            nightsBefore[d] = n.toMap();
        }
        HealthDb db(path());
        HealthImport import(&db, testenv::root(), totalsOf, restingOf, MON.addDays(5));
        QVERIFY(!import.done());
        QTest::ignoreMessage(QtInfoMsg, QRegularExpression("one-time import is done: 3 nights, 4 workouts, 1 resting "
                                                           "rates, 4 days \\(2 skipped\\)"));
        const int units = import.runAll();
        QVERIFY(import.done());
        QCOMPARE(units, 4 + 3 + 3 + 4); // 4 night files (the torn one too), 3 day logs, 3 days with heart rate, 4 with data
        QCOMPARE(import.counts().nights, 3);
        QCOMPARE(import.counts().workouts, 4);
        QCOMPARE(import.counts().days, 4);
        QCOMPARE(import.counts().resting, 1);
        QCOMPARE(import.counts().skipped, 2);
        for (const char *phase : {"import.sleep", "import.exercise", "import.resting", "import.daily"})
            QCOMPARE(db.meta(phase).toLongLong(), qint64(1));
        // the same answers, now from health.db (the files are still there, untouched)
        for (auto it = nightsBefore.constBegin(); it != nightsBefore.constEnd(); ++it) {
            SleepNight n;
            QVERIFY(HealthDb::readNight(path(), it.key(), &n));
            QCOMPARE(n.toMap(), it.value());
            QVERIFY(QFile::exists(sleepFilePath(it.key())));
        }
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM sleep_session WHERE imported_from LIKE 'sleep/%.json'"),
                 qint64(4)); // 3 main sessions and a nap
        const QList<WorkoutRecord> ws = HealthDb::readWorkouts(path(), 0, ms(MON.addDays(7), 0));
        QCOMPARE(ws.size(), 4);
        const WorkoutRecord &run = ws[2]; // newest first: day 2, day 1, the run, the walk
        QCOMPARE(run.startMs, ms(MON, 18, 0));
        QCOMPARE(run.endMs, ms(MON, 18, 45)); // start + the active time
        QCOMPARE(run.activeS, 2700);
        QCOMPARE(run.type, QString("run"));
        QCOMPARE(run.distanceM, 6100.0);
        QCOMPARE(run.kcal, 420.0);
        QCOMPARE(run.avgBpm, 151);
        QCOMPARE(run.maxBpm, 172);
        QCOMPARE(run.steps, 7600);
        QCOMPARE(run.importedFrom, QString("exercise/%1.log").arg(MON.toString("yyyy-MM-dd")));
        QCOMPARE(run.source, QString("manual"));
        // the days: the steps of three, the heart rate of one more, each with the day's summary
        const QList<DailyRecord> days = HealthDb::readDailyRange(path(), MON, MON.addDays(6));
        QCOMPARE(days.size(), 4);
        for (const DailyRecord &r : days) {
            QVERIFY(r.closed);
            DailyTotals t;
            QVERIFY(totalsOf(r.date, &t));
            QCOMPARE(r.totals.steps, t.steps);
            QCOMPARE(r.totals.kcal, t.kcal);
            QCOMPARE(r.totals.azm, t.azm);
        }
        QCOMPARE(days[0].totals.steps, 1100);
        QCOMPARE(days[3].date, MON.addDays(4));
        QCOMPARE(days[3].totals.steps, 0);
        // the resting rate of the day after the second night: the 24 hours up to its end hold 2 still hours at 54
        QCOMPARE(days[1].restingHr, 54);
        QCOMPARE(days[0].restingHr, 0); // half an hour of readings: none
        QCOMPARE(days[3].restingHr, 0);
    }

    void importingTwiceIsImportingOnce()
    {
        makeOldFiles();
        HealthDb db(path());
        {
            HealthImport first(&db, testenv::root(), totalsOf, restingOf, MON.addDays(5));
            first.runAll();
        }
        const qint64 seq = db.seq();
        const qint64 rows = sqlNumber(path(), "SELECT (SELECT COUNT(*) FROM workout) + (SELECT COUNT(*) FROM "
                                              "sleep_session) + (SELECT COUNT(*) FROM daily)");
        // the markers: a new start does nothing
        HealthImport again(&db, testenv::root(), totalsOf, restingOf, MON.addDays(5));
        QVERIFY(again.done());
        QCOMPARE(again.runAll(), 0);
        // without them (as after a crash before the last mark): every unit again, and nothing new
        QVERIFY(db.setMeta("import.sleep", 0));
        QVERIFY(db.setMeta("import.exercise", 0));
        QVERIFY(db.setMeta("import.resting", 0));
        QVERIFY(db.setMeta("import.daily", 0));
        HealthImport third(&db, testenv::root(), totalsOf, restingOf, MON.addDays(5));
        QVERIFY(!third.done());
        QTest::ignoreMessage(QtInfoMsg, QRegularExpression("0 nights, 0 workouts, 0 resting rates, 0 days"));
        third.runAll();
        QVERIFY(third.done());
        QCOMPARE(db.seq(), seq);
        QCOMPARE(sqlNumber(path(), "SELECT (SELECT COUNT(*) FROM workout) + (SELECT COUNT(*) FROM sleep_session) + "
                                   "(SELECT COUNT(*) FROM daily)"), rows);
    }

    void anInterruptedImportResumes()
    {
        makeOldFiles();
        HealthDb db(path());
        {
            HealthImport part(&db, testenv::root(), totalsOf, restingOf, MON.addDays(5));
            QVERIFY(part.step());
            QVERIFY(part.step()); // two nights in, then the daemon stops
        }
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(DISTINCT wake_date) FROM sleep_session"), qint64(2));
        QVERIFY(db.meta("import.sleep").isNull());
        // killed inside a unit: the next night's transaction half written, then gone
        const pid_t child = fork();
        QVERIFY(child >= 0);
        if (child == 0) {
            sqlite3 *raw = nullptr;
            sqlite3_open(QFile::encodeName(healthDbPath()).constData(), &raw);
            const QByteArray sql = "BEGIN IMMEDIATE; INSERT INTO sleep_session (wake_date, start_ms, end_ms, main) "
                                   "VALUES ('" + MON.addDays(3).toString(Qt::ISODate).toLatin1() + "', 1, 2, 1)";
            sqlite3_exec(raw, sql.constData(), nullptr, nullptr, nullptr);
            raise(SIGKILL);
            _exit(1);
        }
        int status = 0;
        QCOMPARE(waitpid(child, &status, 0), child);
        // the next start: through every phase, the units that are in are skipped
        HealthImport rest(&db, testenv::root(), totalsOf, restingOf, MON.addDays(5));
        QVERIFY(!rest.done());
        QTest::ignoreMessage(QtInfoMsg, QRegularExpression("1 nights, 4 workouts, 1 resting rates, 4 days"));
        rest.runAll();
        QVERIFY(rest.done());
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(DISTINCT wake_date) FROM sleep_session"), qint64(3));
        SleepNight n;
        QVERIFY(HealthDb::readNight(path(), MON.addDays(3), &n));
        QCOMPARE(n.main.asleepMin, 300); // the file's, not the half-written row
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM workout"), qint64(4));
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM daily WHERE closed_ms IS NOT NULL"), qint64(4));
    }

    // ---- version 2: the snapshot of a workout that is on (WO-02) ----
    void theSnapshotOfAWorkoutThatIsOn()
    {
        HealthDb db(path());
        QList<qint64> told;
        db.setChangeHandler([&told](qint64 seq) { told << seq; });
        const qint64 id = db.beginWorkout(ms(MON, 7, 0), "Run");
        QVERIFY(id > 0);
        QCOMPARE(told.size(), 1);
        const qint64 seq = db.seq();
        // none saved yet: on, without a snapshot
        WorkoutRecord w;
        qint64 saved = -1;
        QString state = "x";
        bool paused = true;
        QVERIFY(HealthDb::readOpenWorkout(path(), ms(MON, 12, 0), &w, &saved, &state, &paused));
        QCOMPARE(w.id, id);
        QCOMPARE(w.type, QString("Run"));
        QCOMPARE(saved, qint64(0));
        QVERIFY(state.isEmpty());
        QVERIFY(!paused);
        // saved, then replaced: no change of seq, nobody told (a sync never reads it)
        QVERIFY(db.saveWorkoutState(id, ms(MON, 7, 5), "{\"w\":1}"));
        QVERIFY(db.saveWorkoutState(id, ms(MON, 7, 9), "{\"w\":2}"));
        QCOMPARE(db.seq(), seq);
        QCOMPARE(told.size(), 1);
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM workout_state"), qint64(1));
        QVERIFY(db.setPaused(id, ms(MON, 7, 9), true));
        QVERIFY(HealthDb::readOpenWorkout(path(), ms(MON, 12, 0), &w, &saved, &state, &paused));
        QCOMPARE(saved, ms(MON, 7, 9));
        QCOMPARE(state, QString("{\"w\":2}"));
        QVERIFY(paused);
        // only workouts that began before the time asked for (a run of the app asks for the ones before it)
        QVERIFY(!HealthDb::readOpenWorkout(path(), ms(MON, 7, 0), &w));
        QCOMPARE(HealthDb::readCounts(path())["workoutsOpen"].toLongLong(), 1LL);
        QCOMPARE(db.openWorkoutCount(), 1);
        // bad values are refused
        QVERIFY(!db.saveWorkoutState(id, 0, "{}"));
        QVERIFY(!db.saveWorkoutState(id, ms(MON, 7, 10), QString()));
        QVERIFY(!db.saveWorkoutState(id + 100, ms(MON, 7, 10), "{}"));
        QVERIFY(!db.saveWorkoutState(id, ms(MON, 7, 10), QString(2 * 1024 * 1024, 'x')));
        // the end takes the snapshot with it; an ended workout has none and takes none
        WorkoutRecord s;
        s.activeS = 540;
        QVERIFY(db.endWorkout(id, ms(MON, 7, 30), s));
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM workout_state"), qint64(0));
        QVERIFY(!db.saveWorkoutState(id, ms(MON, 7, 31), "{}"));
        QVERIFY(!HealthDb::readOpenWorkout(path(), ms(MON, 12, 0), &w));
        QCOMPARE(db.openWorkoutCount(), 0);
        // a deletion too
        const qint64 other = db.beginWorkout(ms(MON, 9, 0), "Walk");
        QVERIFY(db.saveWorkoutState(other, ms(MON, 9, 1), "{}"));
        QVERIFY(db.deleteWorkout(other));
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM workout_state"), qint64(0));
        QVERIFY(!HealthDb::readOpenWorkout(path(), ms(MON, 12, 0), &w));
        QCOMPARE(HealthDb::readCounts(path())["workoutsOpen"].toLongLong(), 0LL);
    }

    void aDatabaseOfVersion1IsUpgraded()
    {
        qint64 id = 0, seq = 0;
        {
            HealthDb db(path());
            id = db.beginWorkout(ms(MON, 6, 0), "Walk");
            QVERIFY(db.endWorkout(id, ms(MON, 6, 40), WorkoutRecord()));
            QVERIFY(db.beginWorkout(ms(MON, 8, 0), "Run") > 0);
            seq = db.seq();
        }
        // as WO-01 made it: no workout_state, no skin_temp_night, version 1
        sqlite3 *raw = nullptr;
        QCOMPARE(sqlite3_open(QFile::encodeName(path()).constData(), &raw), SQLITE_OK);
        QCOMPARE(sqlite3_exec(raw, "DROP TABLE workout_state; DROP TABLE skin_temp_night; PRAGMA user_version=1", nullptr,
                              nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);
        QCOMPARE(sqlNumber(path(), "PRAGMA user_version"), qint64(1));
        // a reader of the old version finds no snapshot, and no error
        WorkoutRecord w;
        QVERIFY(!HealthDb::readOpenWorkout(path(), ms(MON, 12, 0), &w));
        HealthDb db(path());
        QVERIFY(db.isOpen());
        QCOMPARE(sqlNumber(path(), "PRAGMA user_version"), qint64(3));
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM sqlite_master WHERE name = 'workout_state'"), qint64(1));
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM sqlite_master WHERE name = 'skin_temp_night'"), qint64(1));
        QCOMPARE(sqlNumber(path(), "SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger'"), qint64(5 * 2 + 4 * 3));
        QCOMPARE(db.seq(), seq);
        QCOMPARE(HealthDb::readWorkouts(path(), 0, ms(MON.addDays(1), 0)).size(), 2);
        QVERIFY(HealthDb::readOpenWorkout(path(), ms(MON, 12, 0), &w));
        QVERIFY(db.saveWorkoutState(w.id, ms(MON, 8, 1), "{}"));
        QVERIFY(w.id != id);
    }
};

SENSORLOGD_TEST_MAIN(TestStore)
#include "tst_store.moc"
