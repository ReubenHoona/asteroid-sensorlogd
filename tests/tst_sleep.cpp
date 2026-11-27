// The sleep estimate of version 2 (metrics/sleepEstimator.*, M4 WO-06) on invented nights written as sensorlogd
// writes them (heartrate.db, the stepCounter/, stepCadence/, motion/ and offBody/ day files): a typical night with
// an evening on the sofa, a short night, naps, the watch taken off in the night, a night on the charger, a gap in the
// heart rate readings while worn, the watch used in bed, the two nights of a daylight saving change; the motion and
// off-body records read per minute; the stage timeline stored in health.db (sleep_session, sleep_stage) and read back;
// the day's naps stored once they are over. Every value is invented.
#include <functional>

#include <QTest>

#include "peek.h"
#include "../common.h"
#include "../daemon/sensorPlugins/heartrateDb.h"
#include "../metrics/healthData.h"
#include "../qmlplugin/sleepDataLoader.h"
#include "../store/healthDb.h"
#include "testenv.h"

using namespace Health;

namespace {

const QDate THU(2026, 11, 12);
const QDate WED = THU.addDays(-1);

qint64 ms(const QDate &d, int h, int m = 0, int s = 0) { return QDateTime(d, QTime(h, m, s)).toMSecsSinceEpoch(); }
const qint64 MIN = 60000;

HealthProfile profile()
{
    HealthProfile p;
    p.sex = HealthProfile::Male;
    p.heightCm = 175;
    p.weightKg = 70;
    p.age = 30; // maximum 190
    return p;
}

int sine(int i) { return 56 + (int)std::lround(6 * std::sin(i / 45.0)); } // 50..62, a slow wave

// what sensorlogd would have logged, written at once
class Logs
{
public:
    QMap<QString, QMap<QDate, QList<QPair<qint64, int>>>> files;
    QMap<QDate, int> dayTotal;
    QVector<HeartrateDb::Reading> readings;

    void rec(const QString &prefix, qint64 t, int v) { files[prefix][QDateTime::fromMSecsSinceEpoch(t).date()].append({t, v}); }
    // a heart rate reading a minute (20 s into it)
    void heart(qint64 from, int minutes, const std::function<int(int)> &bpm)
    {
        for (int i = 0; i < minutes; i++)
            readings.append({from + i * MIN + 20000, bpm(i), 4});
    }
    void heart(qint64 from, int minutes, int bpm) { heart(from, minutes, [bpm](int) { return bpm; }); }
    // steps every minute, with the hub's cadence minutes
    void walk(qint64 from, int minutes, int perMinute, bool cadence = true)
    {
        for (int i = 0; i < minutes; i++) {
            const qint64 t = from + i * MIN;
            const QDate d = QDateTime::fromMSecsSinceEpoch(t).date();
            dayTotal[d] += perMinute;
            rec("stepCounter", t + 59000, dayTotal[d]);
            if (cadence)
                rec("stepCadence", t, perMinute);
        }
    }
    void wrist(qint64 t, int state) { rec("offBody", t, state); }
    // the hub's motion records with the screen off: armed again at every 45-minute batch from `from` to `to`; still
    // soon after the arming unless a movement is right at it; the first movement of a window is seen, later ones not
    void batchMotion(qint64 from, qint64 to, const QList<qint64> &moves = {})
    {
        for (qint64 r = from; r < to; r += 45 * MIN) {
            rec("motion", r, 2);
            const qint64 end = qMin(to, r + 45 * MIN);
            qint64 first = -1;
            for (qint64 m : moves)
                if (m >= r && m < end && (first < 0 || m < first))
                    first = m;
            if (first < 0 || first > r + 5000)
                rec("motion", r + 5000, 0);
            if (first >= 0)
                rec("motion", qMax(first, r + 1000), 1);
            // moving at the arming: the stationary detector is still armed and reports once it is still again
            if (first >= 0 && first <= r + 5000)
                rec("motion", qMax(first, r + 1000) + 30000, 0);
        }
    }
    // the screen on, the watch in use: the detectors report and are armed again within seconds
    void inUse(qint64 from, int minutes)
    {
        int k = 0;
        for (qint64 t = from; t < from + minutes * MIN; t += 20000, k++)
            rec("motion", t, k % 3 == 0 ? 1 : k % 3 == 1 ? 2 : 0);
    }
    void write()
    {
        for (auto p = files.constBegin(); p != files.constEnd(); ++p) {
            for (auto d = p->constBegin(); d != p->constEnd(); ++d) {
                QList<QPair<qint64, int>> list = d.value();
                std::stable_sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
                QByteArray out;
                for (const auto &r : list)
                    out += QByteArray::number(r.first / 1000) + ":" + QByteArray::number(r.second) + "\n";
                testenv::writeDay(p.key(), d.key(), out);
            }
        }
        if (!readings.isEmpty()) {
            HeartrateDb db(heartrateDbPath());
            QVERIFY(db.add(readings));
        }
    }
};

// the typical night: an evening on the sofa (72 bpm, no steps), teeth, asleep 22:40 to about 06:50 with a trip to
// the bathroom at 03:10, awake in bed, a walk, the watch off for a shower
void typicalNight(Logs &l)
{
    l.wrist(ms(WED, 7, 0), 0);
    l.heart(ms(WED, 19, 0), 210, 72);
    l.batchMotion(ms(WED, 19, 0), ms(WED, 22, 30),
                  {ms(WED, 19, 10), ms(WED, 19, 50), ms(WED, 20, 40), ms(WED, 21, 20), ms(WED, 22, 10)});
    l.walk(ms(WED, 22, 30), 10, 15);
    l.heart(ms(WED, 22, 30), 10, 90);
    l.heart(ms(WED, 22, 40), 490, sine);
    l.batchMotion(ms(WED, 22, 45), ms(THU, 6, 50),
                  {ms(WED, 23, 20), ms(THU, 0, 50), ms(THU, 2, 0), ms(THU, 3, 10), ms(THU, 4, 40), ms(THU, 5, 30),
                   ms(THU, 6, 20)});
    // the bathroom: 4 minutes of steps, the heart rate up (the readings of those minutes replaced)
    l.walk(ms(THU, 3, 10), 4, 20);
    for (HeartrateDb::Reading &r : l.readings)
        if (r.takenMs >= ms(THU, 3, 10) && r.takenMs < ms(THU, 3, 14))
            r.bpm = 78;
    l.heart(ms(THU, 6, 50), 10, 68); // awake in bed
    l.walk(ms(THU, 7, 0), 40, 60);
    l.heart(ms(THU, 7, 0), 40, 88);
    l.batchMotion(ms(THU, 6, 50), ms(THU, 9, 0), {ms(THU, 6, 52), ms(THU, 7, 45), ms(THU, 8, 30)});
    l.wrist(ms(THU, 7, 40), 1);
    l.wrist(ms(THU, 8, 0), 0);
    l.heart(ms(THU, 8, 0), 60, 78);
}

// one line for the log: the session's numbers (invented data)
void describe(const char *what, const SleepNight &n)
{
    const SleepSession &m = n.main;
    qInfo("%s: %s-%s asleep %d awake %d light %d deep %d rem %d, %d spans, score %d %s, naps %d (%d min)", what,
          qPrintable(QDateTime::fromMSecsSinceEpoch(m.startMs).toString("HH:mm")),
          qPrintable(QDateTime::fromMSecsSinceEpoch(m.endMs).toString("HH:mm")), m.asleepMin, m.awakeMin, m.lightMin,
          m.deepMin, m.remMin, (int)m.stages.size(), n.score, qPrintable(n.scoreLabel), (int)n.naps.size(), n.napMin);
}

int stageAt(const SleepSession &s, qint64 t)
{
    for (const SleepStageSpan &span : s.stages)
        if (t >= span.startMs && t < span.endMs)
            return span.stage;
    return -1;
}

// the timeline covers the session without gaps and adds up to its totals
void checkTimeline(const SleepSession &s)
{
    QVERIFY(!s.stages.isEmpty());
    QCOMPARE(s.stages.first().startMs, s.startMs);
    QCOMPARE(s.stages.last().endMs, s.endMs);
    int minutes[4] = {0, 0, 0, 0};
    for (int i = 0; i < s.stages.size(); i++) {
        const SleepStageSpan &span = s.stages[i];
        QVERIFY(span.endMs > span.startMs);
        QVERIFY(span.stage >= StageAwake && span.stage <= StageRem);
        if (i > 0) {
            QCOMPARE(span.startMs, s.stages[i - 1].endMs);
            QVERIFY(span.stage != s.stages[i - 1].stage); // one span a run
        }
        minutes[span.stage] += (int)((span.endMs - span.startMs) / MIN);
    }
    QCOMPARE(minutes[StageAwake], s.awakeMin);
    QCOMPARE(minutes[StageLight], s.lightMin);
    QCOMPARE(minutes[StageDeep], s.deepMin);
    QCOMPARE(minutes[StageRem], s.remMin);
    QCOMPARE(s.asleepMin, s.lightMin + s.deepMin + s.remMin);
    QCOMPARE(s.asleepMin + s.awakeMin, (int)((s.endMs - s.startMs) / MIN));
}

class FakePoster : public NotificationPoster
{
public:
    uint post(const HealthNotification &) override { return 1; }
};

}

class TestSleep : public QObject
{
    Q_OBJECT
    FakePoster poster;
    QSettings *settings = nullptr;
    qint64 clock = 0;
    QScopedPointer<HealthMonitor> monitor;

    HealthMonitor *makeMonitor(qint64 at)
    {
        monitor.reset();
        clock = at;
        monitor.reset(new HealthMonitor(settings, &poster));
        monitor->setClock([this] { return clock; });
        monitor->start();
        return monitor.data();
    }
    SleepNight night(const QDate &wake = THU) { return estimateNight(wake, profile(), 56); }

private slots:
    void initTestCase() { settings = new QSettings("asteroid", "sensorlogd"); }
    void cleanupTestCase() { delete settings; }
    void init()
    {
        testenv::clean();
        settings->sync();
        MGConfItem::clearTestValues();
    }
    void cleanup() { monitor.reset(); }

    // ---- the records per minute ----
    void motionRecordsPerMinute()
    {
        const qint64 t0 = ms(THU, 1, 0);
        const qint64 from = MinuteSeries::minuteOf(t0);
        auto at = [&](const QList<QPair<qint64, int>> &r) {
            QList<QPointF> p;
            for (const auto &x : r)
                p << QPointF(x.first / 1000, x.second);
            return motionMinutes(p, from, 100);
        };
        // armed, still soon after, a movement at 20 min (the second event: then not watched), armed at 45 min
        QVector<char> m = at({{t0, 2}, {t0 + 5000, 0}, {t0 + 20 * MIN, 1}, {t0 + 45 * MIN, 2}, {t0 + 45 * MIN + 5000, 0}});
        QCOMPARE((int)m[0], (int)MotionStill);
        QCOMPARE((int)m[19], (int)MotionStill);
        QCOMPARE((int)m[20], (int)MotionMoved);
        QCOMPARE((int)m[21], (int)MotionUnknown);
        QCOMPARE((int)m[44], (int)MotionUnknown);
        QCOMPARE((int)m[45], (int)MotionStill);
        QCOMPARE((int)m[99], (int)MotionStill); // the last record, the first since its arming: it holds
        // no movement in the window: the motion detector was armed and silent, still to the next arming
        m = at({{t0, 2}, {t0 + 5000, 0}, {t0 + 45 * MIN, 2}});
        QCOMPARE((int)m[44], (int)MotionStill);
        QCOMPARE((int)m[45], (int)MotionUnknown);
        // moving at the arming, still from 10 min: the stationary detector armed and silent until then
        m = at({{t0, 2}, {t0 + 3000, 1}, {t0 + 10 * MIN, 0}, {t0 + 45 * MIN, 2}});
        QCOMPARE((int)m[0], (int)MotionMoved);
        QCOMPARE((int)m[9], (int)MotionMoved);
        QCOMPARE((int)m[11], (int)MotionUnknown); // the second event: only its moment
        // the first record of all: not known to follow an arming, only its moment
        m = at({{t0 + 5000, 0}, {t0 + 30 * MIN, 1}});
        QCOMPARE((int)m[0], (int)MotionStill); // stillness to the movement (the other detector reported)
        QCOMPARE((int)m[30], (int)MotionMoved);
        QCOMPARE((int)m[31], (int)MotionUnknown);
        // records thick and fast: the watch in use
        m = at({{t0 + 50 * MIN, 1}, {t0 + 50 * MIN + 20000, 2}, {t0 + 50 * MIN + 40000, 0}, {t0 + 51 * MIN, 2},
                {t0 + 51 * MIN + 20000, 1}});
        QCOMPARE((int)m[50], (int)MotionBusy);
        QCOMPARE((int)m[51], (int)MotionBusy);
        QVERIFY(motionMinutes({}, from, 10) == QVector<char>(10, MotionUnknown));
    }

    void offBodyRecordsPerMinute()
    {
        const qint64 t0 = ms(THU, 1, 0);
        const qint64 from = MinuteSeries::minuteOf(t0);
        const QList<QPointF> r = {QPointF((t0 - 3600000) / 1000, 0), QPointF((t0 + 10 * MIN + 30000) / 1000, 1),
                                  QPointF((t0 + 20 * MIN) / 1000, -1), QPointF((t0 + 30 * MIN) / 1000, 0)};
        const QVector<signed char> o = offBodyMinutes(r, from, 40);
        QCOMPARE((int)o[0], 0);
        QCOMPARE((int)o[9], 0);
        QCOMPARE((int)o[10], 1); // off in part of it
        QCOMPARE((int)o[19], 1);
        QCOMPARE((int)o[20], -1);
        QCOMPARE((int)o[30], 0);
        QCOMPARE((int)offBodyMinutes({}, from, 5)[4], -1);
    }

    // ---- invented nights ----
    void aTypicalNight()
    {
        Logs l;
        typicalNight(l);
        l.write();
        const SleepNight n = night();
        QVERIFY(n.valid);
        const SleepSession &m = n.main;
        describe("typical night", n);
        // the sofa (a heart rate well above the night's) and the teeth are not sleep; awake in bed is cut off
        QVERIFY2(m.startMs >= ms(WED, 22, 40) && m.startMs <= ms(WED, 22, 55), qPrintable(QDateTime::fromMSecsSinceEpoch(m.startMs).toString()));
        QVERIFY2(m.endMs >= ms(THU, 6, 45) && m.endMs <= ms(THU, 7, 0), qPrintable(QDateTime::fromMSecsSinceEpoch(m.endMs).toString()));
        checkTimeline(m);
        QVERIFY(m.awakeMin >= 4); // the bathroom
        QCOMPARE(stageAt(m, ms(THU, 3, 11)), (int)StageAwake);
        QVERIFY(m.deepMin >= 0.08 * m.asleepMin && m.deepMin <= 0.25 * m.asleepMin);
        QVERIFY(m.remMin >= 0.05 * m.asleepMin && m.remMin <= 0.25 * m.asleepMin);
        QVERIFY(m.lightMin > m.deepMin && m.lightMin > m.remMin);
        // no REM in the first hour, no Deep or REM at a movement
        for (const SleepStageSpan &s : m.stages)
            if (s.stage == StageRem)
                QVERIFY(s.startMs >= m.startMs + 60 * MIN);
        for (qint64 t : {ms(WED, 23, 20), ms(THU, 0, 50), ms(THU, 2, 0), ms(THU, 4, 40), ms(THU, 5, 30), ms(THU, 6, 20)}) {
            const int st = stageAt(m, t);
            QVERIFY2(st == StageLight || st == StageAwake, qPrintable(QDateTime::fromMSecsSinceEpoch(t).toString()));
        }
        // short runs of a sleep stage were merged
        int shortRuns = 0;
        for (const SleepStageSpan &s : m.stages)
            if (s.stage != StageAwake && s.endMs - s.startMs < 3 * MIN)
                shortRuns++;
        QVERIFY(shortRuns <= 2);
        QVERIFY(n.score >= 60 && n.score <= 100);
        QCOMPARE(n.scoreLabel, sleepScoreLabel(n.score));
        QCOMPARE(n.goalMet, m.asleepMin >= 480);
        QCOMPARE(n.naps.size(), 0);
    }

    void aShortNight()
    {
        Logs l;
        l.wrist(ms(WED, 7, 0), 0);
        l.heart(ms(WED, 20, 0), 360, 82); // up late, walking about now and then
        for (int h = 20; h < 26; h++)
            l.walk(ms(WED, 0, 0) + h * 3600000LL + 15 * MIN, 5, 40);
        l.heart(ms(THU, 2, 0), 210, sine);
        l.batchMotion(ms(THU, 2, 0), ms(THU, 5, 30), {ms(THU, 3, 40)});
        l.walk(ms(THU, 5, 30), 30, 70);
        l.heart(ms(THU, 5, 30), 30, 90);
        l.write();
        const SleepNight n = night();
        QVERIFY(n.valid);
        describe("short night", n);
        QVERIFY(qAbs(n.main.asleepMin - 210) <= 10);
        checkTimeline(n.main);
        QVERIFY(!n.goalMet);
        QVERIFY(n.score < 75); // the duration gives 50 x 210 / 480 at most
        QVERIFY(n.score >= 0);
        QCOMPARE(n.goalMin, 480);
        // the same night against a sleep goal of 200 minutes (the phone's, M4 WO-11): met, a full duration part
        HealthProfile p = profile();
        p.sleepGoalMin = 200;
        const SleepNight withGoal = estimateNight(THU, p, 56);
        QCOMPARE(withGoal.goalMin, 200);
        QVERIFY(withGoal.goalMet);
        QVERIFY(withGoal.score > n.score);
        QCOMPARE(withGoal.main.asleepMin, n.main.asleepMin);
    }

    void napsOfTwentyMinutesAndMore()
    {
        Logs l;
        typicalNight(l);
        l.heart(ms(THU, 9, 0), 240, 76);      // a morning at the desk
        l.heart(ms(THU, 13, 0), 25, 57);      // a nap of 25 minutes (version 1 needed 30)
        l.heart(ms(THU, 13, 25), 95, 76);
        l.heart(ms(THU, 15, 0), 15, 57);      // 15 still minutes: no nap
        l.heart(ms(THU, 15, 15), 45, 76);
        l.heart(ms(THU, 16, 0), 40, 76);      // sitting: the heart rate is not near resting
        l.batchMotion(ms(THU, 9, 0), ms(THU, 17, 0), {ms(THU, 9, 30), ms(THU, 10, 20), ms(THU, 11, 10), ms(THU, 12, 0),
                                                      ms(THU, 13, 40), ms(THU, 14, 30), ms(THU, 15, 20)});
        for (int h : {9, 10, 11, 12, 14})
            l.walk(ms(THU, h, 50), 5, 50);
        l.write();
        const SleepNight n = night();
        QVERIFY(n.valid);
        describe("naps", n);
        QCOMPARE(n.naps.size(), 1);
        QCOMPARE(n.naps[0].startMs, ms(THU, 13, 0));
        QVERIFY(qAbs(n.napMin - 25) <= 1);
        checkTimeline(n.naps[0]);
    }

    // R5 E-30: the evening's naps (18:00-20:00) are looked for; one still going at 20:00 is the next night's
    void anEveningNapIsLookedFor()
    {
        Logs l;
        typicalNight(l);
        l.heart(ms(THU, 9, 0), 570, 76);       // the day, 9:00-18:30
        l.heart(ms(THU, 18, 30), 35, 57);      // a nap 18:30-19:05
        l.heart(ms(THU, 19, 5), 55, 78);
        l.batchMotion(ms(THU, 9, 0), ms(THU, 20, 0), {ms(THU, 9, 30), ms(THU, 11, 10), ms(THU, 13, 0), ms(THU, 15, 20),
                                                      ms(THU, 17, 0), ms(THU, 19, 15)});
        for (int h : {9, 11, 13, 15, 17})
            l.walk(ms(THU, h, 50), 5, 50);
        l.write();
        const SleepNight n = night();
        QVERIFY(n.valid);
        describe("evening nap", n);
        QCOMPARE(n.naps.size(), 1);
        QCOMPARE(n.naps[0].startMs, ms(THU, 18, 30));
        checkTimeline(n.naps[0]);
        // and the next night's window does not take it again: not a nap of Friday, not its night
        const SleepNight fri = night(THU.addDays(1));
        QVERIFY(!fri.valid);
        QVERIFY(fri.naps.isEmpty());
    }

    void aSessionStillGoingAtTheWindowsEndIsNoNap()
    {
        Logs l;
        typicalNight(l);
        l.heart(ms(THU, 9, 0), 600, 76);       // the day, 9:00-19:00
        l.heart(ms(THU, 19, 0), 90, 57);       // asleep from 19:00, still at 20:00: an early night, not a nap
        l.batchMotion(ms(THU, 9, 0), ms(THU, 20, 30), {ms(THU, 9, 30), ms(THU, 13, 0), ms(THU, 17, 0)});
        l.write();
        const SleepNight n = night();
        QVERIFY(n.valid);
        QCOMPARE(n.naps.size(), 0);
    }

    // R5 E-30: Bedtime mode as a hint, never needed: in Bedtime a heart rate a little higher still counts as asleep,
    // and a night may begin from 19:00
    void bedtimeIsAHint()
    {
        // a night at 88 bpm: over the lower fifth of the reserve (56 + 0.20 x 134 = 82.8), within the lower fourth
        // (56 + 0.25 x 134 = 89.5)
        Logs l;
        l.wrist(ms(WED, 7, 0), 0);
        l.heart(ms(WED, 21, 0), 60, 95);
        l.heart(ms(WED, 22, 0), 480, 88);
        l.heart(ms(THU, 6, 0), 240, 95);       // up, and the morning
        l.walk(ms(THU, 6, 5), 30, 60);
        l.batchMotion(ms(WED, 22, 0), ms(THU, 6, 0));
        l.batchMotion(ms(THU, 6, 0), ms(THU, 10, 0), {ms(THU, 6, 5), ms(THU, 7, 0), ms(THU, 8, 0), ms(THU, 9, 0)});
        l.write();
        QVERIFY(!night().valid); // without Bedtime: no sleep at that rate
        Logs b;
        b.rec("bedtime", ms(WED, 21, 55), 1);
        b.rec("bedtime", ms(THU, 6, 10), 0);
        b.write();
        const SleepNight n = night();
        QVERIFY(n.valid);
        describe("bedtime", n);
        QVERIFY(qAbs(n.main.startMs - ms(WED, 22, 0)) <= 15 * MIN);
        QVERIFY(n.main.asleepMin >= 400);
        checkTimeline(n.main);
    }

    void aNightThatBeganAt1930InBedtime()
    {
        Logs l;
        l.wrist(ms(WED, 7, 0), 0);
        l.heart(ms(WED, 18, 0), 90, 76);
        l.heart(ms(WED, 19, 30), 600, sine);   // asleep 19:30-05:30
        l.heart(ms(THU, 5, 30), 60, 80);
        l.walk(ms(THU, 5, 35), 30, 60);
        l.batchMotion(ms(WED, 19, 30), ms(THU, 5, 30));
        l.write();
        QVERIFY(!night().valid); // a session from 19:30 is not a night's (from 20:00), without Bedtime
        Logs b;
        b.rec("bedtime", ms(WED, 19, 20), 1);
        b.rec("bedtime", ms(THU, 5, 40), 0);
        b.write();
        const SleepNight n = night();
        QVERIFY(n.valid);
        QVERIFY(qAbs(n.main.startMs - ms(WED, 19, 30)) <= 15 * MIN);
        QVERIFY(qAbs(n.main.endMs - ms(THU, 5, 30)) <= 15 * MIN);
    }

    void bedtimeRecordsPerMinute()
    {
        const qint64 from = MinuteSeries::minuteOf(ms(WED, 22, 0));
        // on 22:02:30-22:05:00, off after; the state before the window counts
        QVector<char> v = bedtimeMinutes({QPointF(ms(WED, 22, 2, 30) / 1000.0, 1), QPointF(ms(WED, 22, 5) / 1000.0, 0)},
                                         from, 8);
        QCOMPARE(v, QVector<char>({0, 0, 1, 1, 1, 0, 0, 0}));
        v = bedtimeMinutes({QPointF(ms(WED, 21, 0) / 1000.0, 1)}, from, 3);
        QCOMPARE(v, QVector<char>({1, 1, 1}));
        QCOMPARE(bedtimeMinutes({}, from, 2), QVector<char>({0, 0}));
    }

    // the monitor logs Bedtime's changes (the launcher's dconf key), only a change
    void theMonitorLogsBedtime()
    {
        HealthMonitor *mon = makeMonitor(ms(THU, 21, 0));
        QVERIFY(loadStateRecords("bedtime", ms(THU, 0, 0), ms(THU.addDays(1), 0)).isEmpty()); // off: nothing
        clock = ms(THU, 22, 10);
        MGConfItem::setTestValue("/org/asteroidos/modes/bedtime", true);
        clock = ms(THU.addDays(1), 6, 40);
        MGConfItem::setTestValue("/org/asteroidos/modes/bedtime", false);
        QList<QPointF> r = loadStateRecords("bedtime", ms(THU, 0, 0), ms(THU.addDays(1), 12, 0));
        QCOMPARE(r.size(), 2);
        QCOMPARE((qint64)r[0].x() * 1000, ms(THU, 22, 10));
        QCOMPARE((int)r[0].y(), 1);
        QCOMPARE((int)r[1].y(), 0);
        // a restart with Bedtime off writes nothing more
        mon = makeMonitor(ms(THU.addDays(1), 7, 0));
        QCOMPARE(loadStateRecords("bedtime", ms(THU, 0, 0), ms(THU.addDays(1), 12, 0)).size(), 2);
        // one with it on (switched while the daemon was not running) logs it
        MGConfItem::setTestValue("/org/asteroidos/modes/bedtime", true);
        r = loadStateRecords("bedtime", ms(THU, 0, 0), ms(THU.addDays(1), 12, 0));
        QCOMPARE(r.size(), 3);
        Q_UNUSED(mon)
    }

    // R5 E-10: the Morning Brief at the night's end (stored), when Bedtime did not bring it; once a day
    void theMorningBriefComesWithTheNightWithoutBedtime()
    {
        Logs l;
        typicalNight(l);
        l.write();
        QList<HealthNotification> posted;
        struct Poster : NotificationPoster {
            QList<HealthNotification> *out;
            uint post(const HealthNotification &n) override { out->append(n); return 1; }
        } capture;
        capture.out = &posted;
        monitor.reset();
        clock = ms(THU, 7, 40);
        monitor.reset(new HealthMonitor(settings, &capture));
        monitor->setClock([this] { return clock; });
        monitor->start();
        monitor->evaluate();
        SleepNight stored;
        QVERIFY(HealthDb::readNight(healthDbPath(), THU, &stored));
        QList<HealthNotification> briefs;
        for (const HealthNotification &n : posted)
            if (n.hints.value("x-eos-open-page").toString() == "morning")
                briefs << n;
        QCOMPARE(briefs.size(), 1);
        QCOMPARE(briefs[0].summary, QString("Start your day"));
        QCOMPARE(briefs[0].body, QString::fromUtf8("Good morning! Here’s how your day is shaping up."));
        QCOMPARE(briefs[0].hints.value("x-eos-app").toString(), QString("asteroid-health"));
        // once a day
        posted.clear();
        clock = ms(THU, 9, 0);
        monitor->evaluate();
        for (const HealthNotification &n : posted)
            QVERIFY(n.hints.value("x-eos-open-page").toString() != "morning");
        monitor.reset();
    }

    void noMorningBriefFromHereAfterABedtimeNight()
    {
        Logs l;
        typicalNight(l);
        l.rec("bedtime", ms(WED, 22, 20), 1);
        l.rec("bedtime", ms(THU, 7, 0), 0); // the launcher posted it then
        l.write();
        QList<HealthNotification> posted;
        struct Poster : NotificationPoster {
            QList<HealthNotification> *out;
            uint post(const HealthNotification &n) override { out->append(n); return 1; }
        } capture;
        capture.out = &posted;
        monitor.reset();
        clock = ms(THU, 7, 40);
        monitor.reset(new HealthMonitor(settings, &capture));
        monitor->setClock([this] { return clock; });
        monitor->start();
        monitor->evaluate();
        QVERIFY(HealthDb::readNight(healthDbPath(), THU, nullptr));
        for (const HealthNotification &n : posted)
            QVERIFY(n.hints.value("x-eos-open-page").toString() != "morning");
        // and not in the afternoon either: a night stored late says nothing of the morning
        monitor.reset();
    }

    // R5 E-30: a day of naps and no night keeps its naps
    void aDayOfNapsWithoutANightKeepsThem()
    {
        Logs l;
        l.wrist(ms(WED, 7, 0), 0);
        l.wrist(ms(WED, 21, 0), 1);            // off the wrist all night
        l.wrist(ms(THU, 9, 0), 0);
        l.heart(ms(THU, 9, 0), 240, 78);
        l.heart(ms(THU, 13, 0), 40, 57);       // a nap 13:00-13:40
        l.heart(ms(THU, 13, 40), 200, 78);
        l.batchMotion(ms(THU, 9, 0), ms(THU, 17, 0), {ms(THU, 9, 30), ms(THU, 10, 20), ms(THU, 11, 10), ms(THU, 12, 0),
                                                      ms(THU, 13, 50), ms(THU, 14, 30), ms(THU, 15, 20)});
        for (int h : {9, 10, 11, 12, 14, 15, 16})
            l.walk(ms(THU, h, 50), 5, 50);
        l.write();
        const SleepNight n = night();
        QVERIFY(!n.valid);
        QCOMPARE(n.naps.size(), 1);

        HealthMonitor *mon = makeMonitor(ms(THU, 13, 50));
        mon->evaluate();
        QCOMPARE(HealthDb::readCounts(healthDbPath())["naps"].toLongLong(), 0LL); // not over 30 minutes yet
        clock = ms(THU, 14, 20);
        mon->evaluate();
        SleepNight stored;
        QVERIFY(!HealthDb::readNight(healthDbPath(), THU, &stored)); // no night
        QCOMPARE(stored.naps.size(), 1);                             // its nap
        QCOMPARE(stored.naps[0].startMs, ms(THU, 13, 0));
        QCOMPARE(HealthDb::readCounts(healthDbPath())["naps"].toLongLong(), 1LL);
        QCOMPARE(HealthDb::readCounts(healthDbPath())["nights"].toLongLong(), 0LL);
        // nothing new: nothing written
        const qint64 seq = mon->store()->seq();
        clock = ms(THU, 16, 0);
        mon->evaluate();
        QCOMPARE(mon->store()->seq(), seq);
    }

    void theWatchTakenOffInTheNight()
    {
        Logs l;
        l.wrist(ms(WED, 7, 0), 0);
        l.heart(ms(WED, 21, 0), 120, 80);
        l.walk(ms(WED, 22, 45), 10, 30);
        l.heart(ms(WED, 23, 0), 240, 55);
        l.batchMotion(ms(WED, 23, 0), ms(THU, 7, 0), {ms(WED, 23, 50), ms(THU, 1, 30), ms(THU, 2, 59)});
        // off at 03:00, on the bedside table: still, and readings at 50 that are not the wearer's
        l.wrist(ms(THU, 3, 0), 1);
        l.heart(ms(THU, 3, 0), 240, 50);
        l.wrist(ms(THU, 7, 30), 0);
        l.write();
        const SleepNight n = night();
        QVERIFY(n.valid);
        QCOMPARE(n.main.startMs, ms(WED, 23, 0));
        QCOMPARE(n.main.endMs, ms(THU, 3, 0));
        QCOMPARE(n.main.asleepMin, 240);
        checkTimeline(n.main);
        QCOMPARE(stageAt(n.main, ms(THU, 4, 0)), -1);
    }

    void aNightOnTheCharger()
    {
        // off the wrist from the evening, on the charger: the hub measures no heart rate; the watch lies still
        Logs l;
        l.wrist(ms(WED, 7, 0), 0);
        l.heart(ms(WED, 18, 0), 240, 75);
        l.wrist(ms(WED, 22, 0), 1);
        l.batchMotion(ms(WED, 22, 0), ms(THU, 7, 0));
        l.wrist(ms(THU, 7, 0), 0);
        l.heart(ms(THU, 7, 0), 120, 80);
        l.walk(ms(THU, 7, 10), 30, 60);
        l.write();
        QVERIFY(!night().valid);
        // the same with the off-body sensor not known (provider off): no reading, so not worn either
        QVERIFY(QFile::remove(testenv::dayFile("offBody", WED)) && QFile::remove(testenv::dayFile("offBody", THU)));
        QVERIFY(!night().valid);
    }

    void aGapInTheReadingsWhileWornAndStill()
    {
        Logs l;
        l.wrist(ms(WED, 7, 0), 0);
        l.heart(ms(WED, 23, 0), 180, 55);
        l.heart(ms(THU, 2, 40), 260, 55); // no readings 02:00-02:40 (the hub's readings paused)
        l.batchMotion(ms(WED, 23, 0), ms(THU, 7, 0), {ms(WED, 23, 50), ms(THU, 1, 0), ms(THU, 4, 0), ms(THU, 5, 30)});
        l.walk(ms(THU, 7, 0), 20, 60);
        l.heart(ms(THU, 7, 0), 20, 90);
        l.write();
        const SleepNight n = night();
        QVERIFY(n.valid);
        QCOMPARE(n.main.startMs, ms(WED, 23, 0));
        QCOMPARE(n.main.endMs, ms(THU, 7, 0));
        describe("gap in the readings", n);
        QVERIFY(n.main.asleepMin >= 470);
        checkTimeline(n.main);
        // without the hub's streams (version 1's way) the gap is no sleep and splits the night
        MinuteSeries s;
        loadSteps(s, WED);
        loadSteps(s, THU);
        loadHeartRate(s, ms(WED, 18), ms(THU, 18));
        const SleepNight v1 = estimateSleep(s, THU, profile(), 56);
        QVERIFY(v1.valid);
        QVERIFY(v1.main.asleepMin < n.main.asleepMin - 100);
    }

    void usingTheWatchInBedIsAwake()
    {
        Logs l;
        l.wrist(ms(WED, 7, 0), 0);
        l.heart(ms(WED, 23, 0), 480, 56);
        l.batchMotion(ms(WED, 23, 0), ms(THU, 7, 0), {ms(WED, 23, 40), ms(THU, 3, 0)});
        l.inUse(ms(THU, 1, 30), 12); // reading the watch: its heart rate barely up
        for (HeartrateDb::Reading &r : l.readings)
            if (r.takenMs >= ms(THU, 1, 30) && r.takenMs < ms(THU, 1, 42))
                r.bpm = 62;
        l.walk(ms(THU, 7, 0), 20, 60);
        l.heart(ms(THU, 7, 0), 20, 90);
        l.write();
        const SleepNight n = night();
        QVERIFY(n.valid);
        QCOMPARE(n.main.startMs, ms(WED, 23, 0)); // one night, the spell inside it
        QVERIFY(n.main.awakeMin >= 12);
        QCOMPARE(stageAt(n.main, ms(THU, 1, 36)), (int)StageAwake);
        checkTimeline(n.main);
    }

    void theNightsOfADaylightSavingChange_data()
    {
        QTest::addColumn<QDate>("wake");
        // the test zone's rule (run-sl.sh): summer time from the last Sunday of September, back on the first Sunday
        // of April; in UTC these are ordinary nights
        QTest::newRow("clocks forward") << QDate(2026, 9, 27);
        QTest::newRow("clocks back") << QDate(2026, 4, 5);
    }
    void theNightsOfADaylightSavingChange()
    {
        QFETCH(QDate, wake);
        const QDate eve = wake.addDays(-1);
        const qint64 from = ms(eve, 22, 0), to = ms(wake, 6, 0);
        const int minutes = (int)((to - from) / MIN); // 420, 480 or 540: what the clock skipped or had twice
        Logs l;
        l.wrist(ms(eve, 7, 0), 0);
        l.heart(from, minutes, sine);
        l.batchMotion(from, to, {from + 100 * MIN, from + 250 * MIN});
        l.walk(to, 30, 60);
        l.heart(to, 30, 90);
        l.write();
        const SleepNight n = estimateNight(wake, profile(), 56);
        QVERIFY(n.valid);
        QCOMPARE(n.main.startMs, from);
        QCOMPARE(n.main.endMs, to);
        QCOMPARE(n.main.asleepMin + n.main.awakeMin, minutes);
        checkTimeline(n.main);
    }

    // ---- storage ----
    void theNightIsStoredWithItsTimelineAndNapsFollow()
    {
        Logs l;
        typicalNight(l);
        l.heart(ms(THU, 9, 0), 240, 78);
        l.heart(ms(THU, 13, 0), 40, 57); // a nap 13:00-13:40
        l.heart(ms(THU, 13, 40), 200, 78);
        l.batchMotion(ms(THU, 9, 0), ms(THU, 17, 0), {ms(THU, 9, 30), ms(THU, 10, 20), ms(THU, 11, 10), ms(THU, 12, 0),
                                                      ms(THU, 13, 50), ms(THU, 14, 30), ms(THU, 15, 20)});
        for (int h : {9, 10, 11, 12, 14, 15, 16})
            l.walk(ms(THU, h, 50), 5, 50);
        l.write();

        // the morning: the night is stored (version 2, with its timeline), no nap yet
        HealthMonitor *mon = makeMonitor(ms(THU, 8, 30));
        mon->evaluate();
        SleepNight stored;
        QVERIFY(HealthDb::readNight(healthDbPath(), THU, &stored));
        QCOMPARE(stored.algo, QString::fromLatin1(SLEEP_ALGO_V2));
        QCOMPARE(stored.naps.size(), 0);
        checkTimeline(stored.main);
        const SleepNight estimate = estimateNight(THU, HealthProfile::load(THU), 60);
        QVERIFY(stored.main.stages == estimate.main.stages);
        QVariantMap counts = HealthDb::readCounts(healthDbPath());
        QCOMPARE(counts["nightsV2"].toLongLong(), 1LL);
        QCOMPARE(counts["sleepStages"].toLongLong(), (qlonglong)stored.main.stages.size());
        QCOMPARE(counts["naps"].toLongLong(), 0LL);
        // the Sleep card's loader has the timeline
        SleepDataLoader loader;
        const QVariantMap map = loader.forDate(THU);
        QVERIFY(map["stored"].toBool());
        const QVariantList stages = map["main"].toMap()["stages"].toList();
        QCOMPARE(stages.size(), stored.main.stages.size());
        for (const QVariant &v : stages)
            QVERIFY(QStringList({"Awake", "Light", "Deep", "REM"}).contains(v.toMap()["name"].toString()));

        // 13:50: the nap ended 10 minutes ago, not yet; 14:20: stored with the morning's main sleep as it was
        qint64 seq = mon->store()->seq();
        clock = ms(THU, 13, 50);
        mon->evaluate();
        QCOMPARE(mon->store()->seq(), seq);
        clock = ms(THU, 14, 20);
        mon->evaluate();
        QVERIFY(mon->store()->seq() > seq);
        SleepNight withNap;
        QVERIFY(HealthDb::readNight(healthDbPath(), THU, &withNap));
        QCOMPARE(withNap.naps.size(), 1);
        QCOMPARE(withNap.naps[0].startMs, ms(THU, 13, 0));
        QCOMPARE(withNap.napMin, withNap.naps[0].asleepMin);
        QCOMPARE(withNap.main.startMs, stored.main.startMs);
        QVERIFY(withNap.main.stages == stored.main.stages);
        QCOMPARE(withNap.score, stored.score);
        checkTimeline(withNap.naps[0]);
        counts = HealthDb::readCounts(healthDbPath());
        QCOMPARE(counts["naps"].toLongLong(), 1LL);
        QCOMPARE(counts["nights"].toLongLong(), 1LL);
        // nothing new: nothing written
        seq = mon->store()->seq();
        clock = ms(THU, 16, 0);
        mon->evaluate();
        QCOMPARE(mon->store()->seq(), seq);
    }
};

SENSORLOGD_TEST_MAIN(TestSleep)
#include "tst_sleep.moc"
