// The skin temperature (M4 WO-13): SkinTempPlugin (daemon/sensorPlugins/skinTempSensor.cpp), the hub's readings as
// sensorfwd's hub connection sends them (org.eos.UsfHub SubscribeSkinTemp, ActivitySamples of type 0x3d) made into a
// row a minute in health.db skin_temp; when it is sampled (metrics/skinTemp.h skinTempWanted, the Health monitor's
// look: inside a night's sleep only); the night's value, baseline, variation and personal range over invented series
// of nights (a baseline built over nights, a +1.0 deg C night, gaps, minutes off the wrist and awake left out); the
// states the Health app shows; health.db's skin_temp_night (schema version 3) and the QML loader. A fake hub on the
// bus (run under dbus-run-session, the session bus standing in for the system bus). Every value is invented.
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QSignalSpy>
#include <QTest>
#include <cmath>
#include <memory>
#include <time.h>

#include <sqlite3.h>

#include "peek.h"
#include "../common.h"
#include "../daemon/sensorPlugins/sampleTime.h"
#include "../metrics/healthData.h"
#include "../metrics/skinTemp.h"
#include "../qmlplugin/healthMetricsLoader.h"
#include "../store/healthDb.h"
#include "testenv.h"

using namespace Health;

namespace {

const QDate D0(2026, 11, 2); // the first morning of the invented nights (no daylight saving change near it)
const QDate THU(2026, 11, 12);

qint64 at(const QDate &d, int h, int m = 0) { return QDateTime(d, QTime(h, m)).toMSecsSinceEpoch(); }

// the CLOCK_BOOTTIME time stamp of a wall clock time (tst_activity's helper)
qlonglong bootUsAt(qint64 wallMs)
{
    static const qint64 off = [] {
        struct timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        const qint64 b = (qint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
        return readingWallMs((quint64)b) * 1000 - b;
    }();
    return (qlonglong)(wallMs * 1000 - off);
}
qint64 minuteAgo(int minutes) { return (QDateTime::currentMSecsSinceEpoch() / 60000 - minutes) * 60000; }

// a night from 23:00 the evening before to 07:00, asleep all of it (Light) unless spans are given
SleepSession sleepOf(const QDate &wake, QVector<SleepStageSpan> spans = {})
{
    SleepSession s;
    s.startMs = at(wake.addDays(-1), 23);
    s.endMs = at(wake, 7);
    s.asleepMin = 480;
    if (spans.isEmpty())
        spans.append({s.startMs, s.endMs, StageLight});
    s.stages = spans;
    return s;
}

// minute rows of [fromMs, toMs) at about base deg C (a slow invented wave of +-0.08)
QVector<QPair<qint64, double>> temps(qint64 fromMs, qint64 toMs, double base, double wave = 0.08)
{
    QVector<QPair<qint64, double>> rows;
    int i = 0;
    for (qint64 t = fromMs; t < toMs; t += 60000, i++)
        rows.append(qMakePair(t, std::round((base + wave * std::sin(i / 23.0)) * 100.0) / 100.0));
    return rows;
}
// a whole night's rows as the sampling gives them: from 45 minutes after falling asleep (the first look) to the end
QVector<QPair<qint64, double>> nightTemps(const QDate &wake, double base)
{
    const SleepSession s = sleepOf(wake);
    return temps(s.startMs + 45 * 60000, s.endMs, base);
}
double meanOf(const QVector<QPair<qint64, double>> &rows)
{
    double s = 0;
    for (const auto &r : rows)
        s += r.second;
    return rows.isEmpty() ? 0 : s / rows.size();
}

MinuteSeries series(qint64 nowMs, int steps, int bpm, int hrMinutes)
{
    MinuteSeries s;
    const qint64 last = MinuteSeries::minuteOf(nowMs);
    for (int i = 0; i < hrMinutes; i++)
        s.addHr(MinuteSeries::msOf(last - i) + 10000, bpm);
    if (steps > 0)
        s.addSteps(MinuteSeries::msOf(last - 3) + 5000, steps);
    return s;
}

class FakePoster : public NotificationPoster
{
public:
    QList<HealthNotification> posted;
    uint post(const HealthNotification &n) override
    {
        posted << n;
        return 100 + posted.size();
    }
};

} // namespace

// sensorfwd's hub connection as the plugin sees it
class FakeHub : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.eos.UsfHub")
public:
    QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("hub"));
    int subscribes = 0, unsubscribes = 0;
    bool up()
    {
        return bus.registerObject(QStringLiteral("/org/eos/usfhub"), this, QDBusConnection::ExportScriptableContents) &&
               bus.registerService(QStringLiteral("com.nokia.SensorService"));
    }
public slots:
    Q_SCRIPTABLE int SubscribeSkinTemp() { subscribes++; return 1; }
    Q_SCRIPTABLE int UnsubscribeSkinTemp() { unsubscribes++; return 0; }
};

class TestSkinTemp : public QObject
{
    Q_OBJECT
    FakeHub *hub = nullptr;
    std::unique_ptr<HealthDb> store;
    std::unique_ptr<SkinTempPlugin> plugin;
    QSettings *settings = nullptr;
    FakePoster poster;
    qint64 clock = 0;
    QScopedPointer<HealthMonitor> monitor;

    void make() { plugin.reset(new SkinTempPlugin(store.get())); }
    void samples(int type, const QList<qint64> &wallMs, const QList<double> &v)
    {
        QList<qlonglong> t;
        for (qint64 ms : wallMs)
            t << bootUsAt(ms);
        plugin->samples(type, t, v);
    }
    void settle() { QTest::qWait(SkinTempPlugin::SETTLE_MS + 200); }
    void settleStop() { QTest::qWait(SkinTempPlugin::STOP_SETTLE_MS + 300); }
    QList<double> values(qint64 fromMs, qint64 toMs)
    {
        QList<double> out;
        for (const auto &r : HealthDb::readSkinTemp(healthDbPath(), fromMs, toMs))
            out << r.second;
        return out;
    }
    HealthMonitor *makeMonitor(qint64 now)
    {
        monitor.reset();
        clock = now;
        monitor.reset(new HealthMonitor(settings, &poster));
        monitor->setClock([this] { return clock; });
        monitor->setStore(store.get());
        monitor->setSkinTempAvailable(true);
        monitor->start();
        return monitor.data();
    }
    // the nights of D0 + k with these bases (0: none), each from the ones before, as the monitor makes them
    QList<SkinTempNight> buildNights(const QList<double> &bases)
    {
        QList<SkinTempNight> made;
        for (int k = 0; k < bases.size(); k++) {
            if (bases[k] <= 0)
                continue;
            const QDate wake = D0.addDays(k);
            made.append(skinTempNight(wake, sleepOf(wake), nightTemps(wake, bases[k]), {}, made));
        }
        return made;
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QDBusConnection::sessionBus().isConnected(), "run under dbus-run-session");
        QVERIFY2(QDBusConnection::systemBus().isConnected(), "DBUS_SYSTEM_BUS_ADDRESS set to the session bus");
        hub = new FakeHub();
        QVERIFY(hub->up());
        settings = new QSettings("asteroid", "sensorlogd");
    }
    void cleanupTestCase()
    {
        delete hub;
        delete settings;
    }
    void init()
    {
        testenv::clean();
        settings->sync();
        store.reset(new HealthDb(healthDbPath()));
        QVERIFY(store->isOpen());
    }
    void cleanup()
    {
        plugin.reset();
        monitor.reset();
        store.reset();
    }

    // ---- the plugin ----
    void subscribesOnlyWhileWanted()
    {
        const int s0 = hub->subscribes, u0 = hub->unsubscribes;
        make();
        QTest::qWait(300);
        QCOMPARE(hub->subscribes, s0); // nothing in the day: not until the monitor asks
        plugin->setWanted(true);
        QTRY_COMPARE(hub->subscribes, s0 + 1);
        plugin->setWanted(true);
        plugin->resubscribe(); // sensorfwd came back
        QTRY_COMPARE(hub->subscribes, s0 + 2);
        plugin->setWanted(false);
        QTRY_COMPARE(hub->unsubscribes, u0 + 1);
        plugin->resubscribe();
        QTest::qWait(300);
        QCOMPARE(hub->subscribes, s0 + 2);
        plugin->setWanted(true);
        QTRY_COMPARE(hub->subscribes, s0 + 3);
        plugin.reset(); // the daemon ends while sampling
        QTRY_COMPARE(hub->unsubscribes, u0 + 2);
    }

    void aRowAMinuteFromOnChangeReadings()
    {
        make();
        plugin->setWanted(true);
        const qint64 m = minuteAgo(3); // the deliveries' readings up to about now, as a batch has them
        samples(0x3d, {m, m + 30000, m + 90000}, {34.0, 34.5, 33.0});
        settle();
        // a reading holds until the next: 34.0 for 30 s and 34.5 for 30 s; the second minute is still open
        QCOMPARE(values(m, m + 3600000), QList<double>({34.25}));
        samples(0x3d, {m + 150000}, {33.5});
        settle();
        QCOMPARE(values(m, m + 3600000), QList<double>({34.25, 33.75}));
        const qint64 seq = store->seq();
        // the end: the reading in force counts up to the stop (33.0 and 33.5 in the third minute)
        plugin->setWanted(false);
        settleStop();
        const QList<double> v = values(m, m + 3600000);
        QVERIFY(v.size() >= 3);
        QCOMPARE(v.mid(0, 3), QList<double>({34.25, 33.75, 33.25}));
        for (int i = 3; i < v.size(); i++)
            QCOMPARE(v[i], 33.5);
        QCOMPARE(store->seq(), seq); // a sample table: no change of seq
        QCOMPARE(plugin->minutesWritten(), v.size());
    }

    void aReadingHoldsFiveMinutesAtMost()
    {
        make();
        plugin->setWanted(true);
        const qint64 m = minuteAgo(12);
        samples(0x3d, {m}, {34.0});
        settle();
        samples(0x3d, {m + 10 * 60000}, {34.2}); // the next one ten minutes later: nothing came between (on-change)
        settle();
        plugin->setWanted(false);
        settleStop();
        const auto rows = HealthDb::readSkinTemp(healthDbPath(), m, m + 3600000);
        QVERIFY(rows.size() >= 7);
        QCOMPARE(rows[4].first, m + 4 * 60000);
        QCOMPARE(rows[4].second, 34.0);
        QCOMPARE(rows[5].first, m + 10 * 60000); // the five minutes after the first reading's hold have no row
        QCOMPARE(rows[6].second, 34.2);
    }

    void otherStreamsAndBadValuesAreLeftOut()
    {
        make();
        plugin->setWanted(true);
        const qint64 m = minuteAgo(3);
        samples(0x3a, {m, m + 30000}, {96.0, 100.0}); // step cadence: not ours
        samples(0x3d, {m, m + 10000, m + 20000, m + 30000}, {std::nan(""), 99.0, 2.0, 34.0});
        settle();
        plugin->setWanted(false);
        settleStop();
        QCOMPARE(plugin->readingsDropped(), 3);
        const QList<double> v = values(m, m + 3600000);
        QVERIFY(!v.isEmpty());
        for (double c : v)
            QCOMPARE(c, 34.0);
    }

    void theHubsLastBatchAfterTheStopCounts()
    {
        make();
        plugin->setWanted(true);
        const qint64 m = minuteAgo(5);
        samples(0x3d, {m, m + 60000}, {34.0, 34.4});
        settle();
        plugin->setWanted(false);
        // the hub flushes what it held at the unsubscribe; it comes a moment later
        QTest::qWait(400);
        samples(0x3d, {m + 120000, m + 180000}, {34.8, 35.0});
        settleStop();
        const QList<double> v = values(m, m + 3600000);
        QVERIFY(v.size() >= 5);
        QCOMPARE(v.mid(0, 5), QList<double>({34.0, 34.4, 34.8, 35.0, 35.0}));
    }

    // ---- when it is sampled ----
    void wantedOnlyInsideASleep_data()
    {
        QTest::addColumn<bool>("running");
        QTest::addColumn<int>("hour");
        QTest::addColumn<int>("steps");     // in the last 20 minutes
        QTest::addColumn<int>("bpm");       // their mean heart rate
        QTest::addColumn<int>("hrMinutes"); // minutes with a reading
        QTest::addColumn<bool>("worn");
        QTest::addColumn<bool>("wanted");
        // resting 60, maximum 190: asleep up to 86 bpm, awake above 105.5
        QTest::newRow("asleep at 23:30") << false << 23 << 0 << 55 << 20 << true << true;
        QTest::newRow("asleep at 03:00") << false << 3 << 0 << 55 << 20 << true << true;
        QTest::newRow("not started before 20:00") << false << 19 << 0 << 55 << 20 << true << false;
        QTest::newRow("not started from 08:00") << false << 9 << 0 << 55 << 20 << true << false;
        QTest::newRow("running: goes on at 09:00 in bed") << true << 9 << 0 << 58 << 20 << true << true;
        QTest::newRow("running: stops at noon") << true << 12 << 0 << 55 << 20 << true << false;
        QTest::newRow("never in the afternoon") << true << 15 << 0 << 55 << 20 << true << false;
        QTest::newRow("turning over: a few steps") << false << 23 << 8 << 55 << 20 << true << true;
        QTest::newRow("still up: steps") << false << 23 << 40 << 55 << 20 << true << false;
        QTest::newRow("running: the bathroom, 60 steps") << true << 3 << 60 << 70 << 20 << true << true;
        QTest::newRow("running: up and about, 150 steps") << true << 7 << 150 << 80 << 20 << true << false;
        QTest::newRow("awake heart rate on the sofa") << false << 22 << 0 << 95 << 20 << true << false;
        QTest::newRow("running: awake heart rate") << true << 6 << 0 << 110 << 20 << true << false;
        QTest::newRow("too few heart rate minutes") << false << 23 << 0 << 55 << 3 << true << false;
        QTest::newRow("off the wrist") << false << 23 << 0 << 55 << 20 << false << false;
        QTest::newRow("running: off the wrist (the charger)") << true << 3 << 0 << 55 << 20 << false << false;
    }
    void wantedOnlyInsideASleep()
    {
        QFETCH(bool, running);
        QFETCH(int, hour);
        QFETCH(int, steps);
        QFETCH(int, bpm);
        QFETCH(int, hrMinutes);
        QFETCH(bool, worn);
        QFETCH(bool, wanted);
        const qint64 now = at(THU, hour, 30);
        QCOMPARE(skinTempWanted(running, series(now, steps, bpm, hrMinutes), now, worn, 60, 190), wanted);
    }

    void theMonitorSwitchesItAtItsLooks()
    {
        HealthMonitor *mon = makeMonitor(at(THU, 23, 30));
        QSignalSpy spy(mon, &HealthMonitor::skinTempSamplingChanged);
        // twenty quiet minutes at 55 bpm before 23:30
        for (int i = 20; i >= 1; i--)
            mon->ingestHeart(at(THU, 23, 30) - i * 60000 + 10000, 55);
        mon->evaluate();
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy[0][0].toBool(), true);
        QVERIFY(mon->skinTempSampling());
        QCOMPARE(store->meta("skin_temp.on_ms").toLongLong(), at(THU, 23, 30));
        mon->evaluate();
        QCOMPARE(spy.size(), 1); // the same again: nothing
        // 03:10: the bathroom (60 steps), back in bed
        clock = at(THU.addDays(1), 3, 10);
        for (int i = 20; i >= 1; i--)
            mon->ingestHeart(clock - i * 60000 + 10000, i > 10 ? 54 : 66);
        mon->ingestSteps(clock - 12 * 60000, 60);
        mon->evaluate();
        QCOMPARE(spy.size(), 1);
        // 07:30: up and about
        clock = at(THU.addDays(1), 7, 30);
        for (int i = 20; i >= 1; i--) {
            mon->ingestHeart(clock - i * 60000 + 10000, 82);
            mon->ingestSteps(clock - i * 60000 + 20000, 20);
        }
        mon->evaluate();
        QCOMPARE(spy.size(), 2);
        QCOMPARE(spy[1][0].toBool(), false);
        QCOMPARE(store->meta("skin_temp.on_ms").toLongLong(), 0LL);
        QCOMPARE(store->meta("skin_temp.off_ms").toLongLong(), clock);
        // a new run of the daemon finds no stale mark
        store->setMeta("skin_temp.on_ms", (qlonglong)at(THU.addDays(1), 2, 0));
        makeMonitor(at(THU.addDays(1), 9, 0));
        QCOMPARE(store->meta("skin_temp.on_ms").toLongLong(), 0LL);
    }

    void withoutThePluginNothingIsAsked()
    {
        monitor.reset(new HealthMonitor(settings, &poster));
        clock = at(THU, 23, 30);
        monitor->setClock([this] { return clock; });
        monitor->setStore(store.get());
        monitor->start();
        QSignalSpy spy(monitor.data(), &HealthMonitor::skinTempSamplingChanged);
        for (int i = 20; i >= 1; i--)
            monitor->ingestHeart(clock - i * 60000 + 10000, 55);
        monitor->evaluate();
        QCOMPARE(spy.size(), 0);
        QVERIFY(store->meta("skin_temp.on_ms").isNull());
    }

    // ---- the night ----
    void theBaselineIsBuiltOverNights()
    {
        const QList<SkinTempNight> n = buildNights({33.9, 34.1, 33.8, 34.0});
        QCOMPARE(n.size(), 4);
        for (int k = 0; k < 3; k++) {
            QVERIFY(n[k].hasMean);
            QCOMPARE(n[k].nights, k);
            QVERIFY(!n[k].hasVariation);
            QCOMPARE(n[k].minutes, 435); // 23:45 to 07:00
        }
        // the mornings before the fourth: "Wear to sleep 3, 2, 1 more times"
        for (int k = 0; k < 3; k++) {
            const SkinTempView v = skinTempView(D0.addDays(k), n.mid(0, k + 1), 0, 0, at(D0.addDays(k), 8));
            QCOMPARE(v.state, (int)SkinCalibrating);
            QCOMPARE(v.nightsLeft, 3 - k);
        }
        QCOMPARE(skinTempView(D0, {}, 0, 0, at(D0, 8)).state, (int)SkinNoData); // nothing yet: "Wear your watch to sleep"
        const SkinTempNight &fourth = n[3];
        QVERIFY(fourth.hasVariation);
        QCOMPARE(fourth.nights, 3);
        const double base = (n[0].meanC + n[1].meanC + n[2].meanC) / 3;
        QCOMPARE(fourth.baselineC, std::round(base * 100) / 100);
        QCOMPARE(fourth.devC, std::round((fourth.meanC - base) * 100) / 100);
        QCOMPARE(fourth.hiC, SKIN_RANGE_MIN_C); // three close nights: the narrowest range
        QCOMPARE(fourth.loC, -SKIN_RANGE_MIN_C);
        const SkinTempView v = skinTempView(D0.addDays(3), n, 0, 0, at(D0.addDays(3), 8));
        QCOMPARE(v.state, (int)SkinInRange);
        QCOMPARE(v.nightsLeft, 0);
        QCOMPARE(v.toMap().value("state").toString(), QString("inRange"));
    }

    void aWarmNightIsAboveRangeAndACoolOneBelow()
    {
        const QList<SkinTempNight> n = buildNights({33.9, 34.1, 33.8, 34.0, 35.0, 33.1});
        const SkinTempNight &warm = n[4];
        QVERIFY(warm.hasVariation);
        QVERIFY2(warm.devC > 0.9 && warm.devC < 1.2, qPrintable(QString::number(warm.devC)));
        QCOMPARE(warm.hiC, SKIN_RANGE_MIN_C);
        QCOMPARE(skinTempView(D0.addDays(4), n, 0, 0, at(D0.addDays(4), 8)).state, (int)SkinAboveRange);
        // the warm night is in the next night's baseline: its range is wider (2 standard deviations)
        const SkinTempNight &cool = n[5];
        QVERIFY(cool.hasVariation);
        QCOMPARE(cool.nights, 5);
        QVERIFY2(cool.hiC > SKIN_RANGE_MIN_C && cool.hiC <= SKIN_RANGE_MAX_C, qPrintable(QString::number(cool.hiC)));
        QVERIFY2(cool.devC < cool.loC, qPrintable(QString("%1 %2").arg(cool.devC).arg(cool.loC)));
        QCOMPARE(skinTempView(D0.addDays(5), n, 0, 0, at(D0.addDays(5), 8)).state, (int)SkinBelowRange);
        // the week as the loader gives it is the same computation; the warm night's value in deg C, to 0.01
        QCOMPARE(warm.devC, std::round(warm.devC * 100) / 100);
    }

    void gapsAMissedNightAndOldNights()
    {
        // sampling with gaps (the watch looked at, the sampling stopped and began again): 30 + 60 + 20 minutes
        const QDate wake = D0;
        const SleepSession s = sleepOf(wake);
        QVector<QPair<qint64, double>> rows = temps(at(wake, 0, 0), at(wake, 0, 30), 34.0, 0);
        rows += temps(at(wake, 2, 0), at(wake, 3, 0), 34.0, 0);
        rows += temps(at(wake, 5, 0), at(wake, 5, 20), 34.0, 0);
        SkinTempNight g = skinTempNight(wake, s, rows, {}, {});
        QCOMPARE(g.minutes, 110);
        QVERIFY(g.hasMean);
        QCOMPARE(g.meanC, 34.0);
        // too few: 45 minutes
        SkinTempNight few = skinTempNight(wake, s, temps(at(wake, 2, 0), at(wake, 2, 45), 34.0, 0), {}, {});
        QCOMPARE(few.minutes, 45);
        QVERIFY(!few.hasMean);
        // a missed night (the third): the fifth night has three before it all the same
        const QList<SkinTempNight> n = buildNights({33.9, 34.1, 0, 33.8, 34.0});
        QCOMPARE(n.size(), 4);
        QCOMPARE(n[2].nights, 2);
        QVERIFY(!n[2].hasVariation);
        QCOMPARE(n[3].nights, 3);
        QVERIFY(n[3].hasVariation);
        // the morning after the missed night: still two to go ("Wear to sleep 2 more times")
        const SkinTempView missed = skinTempView(D0.addDays(2), n.mid(0, 2), 0, 0, at(D0.addDays(2), 9));
        QCOMPARE(missed.state, (int)SkinCalibrating);
        QCOMPARE(missed.nightsLeft, 2);
        // a night without a value does not count either
        QList<SkinTempNight> withFew = n.mid(0, 2);
        few.wakeDate = D0.addDays(2);
        withFew.append(few);
        QCOMPARE(skinTempView(D0.addDays(2), withFew, 0, 0, at(D0.addDays(2), 9)).nightsLeft, 2);
        // nights more than 30 days before do not make a baseline
        QList<SkinTempNight> old;
        for (int days : {31, 35, 40}) {
            SkinTempNight o;
            o.wakeDate = D0.addDays(40 - days);
            o.hasMean = true;
            o.meanC = 34.0;
            old.append(o);
        }
        const QDate late = D0.addDays(40);
        SkinTempNight after = skinTempNight(late, sleepOf(late), nightTemps(late, 34.0), {}, old);
        QCOMPARE(after.nights, 0);
        QVERIFY(!after.hasVariation);
        old[0].wakeDate = late.addDays(-30); // exactly 30 days before: counts
        QCOMPARE(skinTempNight(late, sleepOf(late), nightTemps(late, 34.0), {}, old).nights, 1);
    }

    void offWristAwakeAndOutsideMinutesAreLeftOut()
    {
        const QDate wake = D0;
        SleepSession s = sleepOf(wake);
        // asleep 23:00-03:10, awake 03:10-03:40 (warm: under the blanket, getting up), asleep 03:40-07:00
        s.stages = {{s.startMs, at(wake, 3, 10), StageLight}, {at(wake, 3, 10), at(wake, 3, 40), StageAwake},
                    {at(wake, 3, 40), s.endMs, StageDeep}};
        QVector<QPair<qint64, double>> rows;
        for (qint64 t = at(wake.addDays(-1), 22, 0); t < at(wake, 8, 0); t += 60000) {
            double c = 34.0;
            if (t >= at(wake, 2, 0) && t < at(wake, 2, 40))
                c = 30.5;  // off the wrist on the pillow (a value a wrist could have: only the off-body says)
            else if (t >= at(wake, 3, 10) && t < at(wake, 3, 40))
                c = 35.5;  // awake
            else if (t < s.startMs || t >= s.endMs)
                c = 31.0;  // before and after the sleep
            else if (t >= at(wake, 5, 0) && t < at(wake, 5, 5))
                c = 22.0;  // a reading no wrist has (the sensor in the air)
            rows.append(qMakePair(t, c));
        }
        const QList<QPointF> offBody = {QPointF(at(wake, 2, 0) / 1000, 1), QPointF(at(wake, 2, 40) / 1000, 0)};
        const SkinTempNight n = skinTempNight(wake, s, rows, offBody, {});
        QCOMPARE(n.minutes, 480 - 40 - 30 - 5);
        QCOMPARE(n.meanC, 34.0);
        // without the off-body records the pillow's minutes count
        const SkinTempNight noOff = skinTempNight(wake, s, rows, {}, {});
        QCOMPARE(noOff.minutes, 480 - 30 - 5);
        QVERIFY(noOff.meanC < 34.0);
        // a night of version 1 (no timeline): all of the session
        SleepSession v1 = sleepOf(wake);
        v1.stages.clear();
        QCOMPARE(skinTempNight(wake, v1, rows, offBody, {}).minutes, 480 - 40 - 5);
    }

    void theStatesOfTheCard()
    {
        const QList<SkinTempNight> n = buildNights({33.9, 34.1, 33.8});
        const QDate today = D0.addDays(3);
        const qint64 now = at(today, 7, 50);
        // sampling: "Tracking..." (also over a value)
        QCOMPARE(skinTempView(today, n, now - 2 * 3600000, 0, now).state, (int)SkinTracking);
        // a mark older than 16 hours is stale
        QCOMPARE(skinTempView(today, n, now - 17 * 3600000LL, 0, now).state, (int)SkinNoData);
        // the baseline is there, the night not through: "Analyzing..." up to an hour after the sampling stopped
        QCOMPARE(skinTempView(today, n, 0, now - 20 * 60000, now).state, (int)SkinAnalyzing);
        QCOMPARE(skinTempView(today, n, 0, now - 2 * 3600000, now).state, (int)SkinNoData);
        // the night done without a value: "Wear your watch to sleep"
        SkinTempNight none;
        none.wakeDate = today;
        none.minutes = 12;
        QList<SkinTempNight> withNone = n;
        withNone.append(none);
        const SkinTempView v = skinTempView(today, withNone, 0, now - 20 * 60000, now);
        QCOMPARE(v.state, (int)SkinNoData);
        QCOMPARE(v.nightsLeft, 0);
        const QVariantMap m = v.toMap();
        QCOMPARE(m.value("state").toString(), QString("noData"));
        QCOMPARE(m.value("measured").toBool(), true);
        QCOMPARE(m.value("minutes").toInt(), 12);
        QCOMPARE(m.value("hasVariation").toBool(), false);
        QCOMPARE(skinTempStateName(SkinCalibrating), QString("calibrating"));
        QCOMPARE(skinTempStateName(SkinAboveRange), QString("aboveRange"));
    }

    // ---- health.db ----
    void storedAndReadBack()
    {
        int changes = 0;
        store->setChangeHandler([&changes](qint64) { changes++; });
        const qint64 seq0 = store->seq();
        const QVector<QPair<qint64, double>> rows = temps(at(THU, 1, 0), at(THU, 1, 10), 34.0);
        QVERIFY(store->addSkinTemp(rows));
        QCOMPARE(store->seq(), seq0);
        QCOMPARE(changes, 0);
        QVERIFY(store->addSkinTemp({qMakePair(at(THU, 1, 0), 30.0)})); // a minute that has a row keeps it
        const auto back = HealthDb::readSkinTemp(healthDbPath(), at(THU, 1, 0), at(THU, 1, 10));
        QVERIFY(back == rows);
        QCOMPARE(HealthDb::readSkinTemp(healthDbPath(), at(THU, 1, 5), at(THU, 1, 7)).size(), 2);

        const QList<SkinTempNight> n = buildNights({33.9, 34.1, 33.8, 35.0});
        for (const SkinTempNight &x : n)
            QVERIFY(store->storeSkinTempNight(x));
        QCOMPARE(changes, 4);
        const qint64 seq1 = store->seq();
        QVERIFY(seq1 > seq0);
        QVERIFY(store->storeSkinTempNight(n[3])); // the same again: no change
        QCOMPARE(store->seq(), seq1);
        QCOMPARE(changes, 4);
        const QList<SkinTempNight> read = HealthDb::readSkinTempNights(healthDbPath(), D0, D0.addDays(3));
        QCOMPARE(read.size(), 4);
        QCOMPARE(read[3].wakeDate, D0.addDays(3));
        QCOMPARE(read[3].meanC, n[3].meanC);
        QCOMPARE(read[3].devC, n[3].devC);
        QCOMPARE(read[3].loC, n[3].loC);
        QCOMPARE(read[3].hiC, n[3].hiC);
        QCOMPARE(read[3].baselineC, n[3].baselineC);
        QCOMPARE(read[3].nights, 3);
        QCOMPARE(read[3].minutes, n[3].minutes);
        QVERIFY(read[3].hasVariation && !read[0].hasVariation && read[0].hasMean);
        QCOMPARE(HealthDb::readSkinTempNights(healthDbPath(), D0.addDays(1), D0.addDays(2)).size(), 2);
        // the day's row has the variation (only the night that has one)
        QVariantMap c = HealthDb::readCounts(healthDbPath());
        QCOMPARE(c["skinTempMinutes"].toLongLong(), 10LL);
        QCOMPARE(c["skinTempNights"].toLongLong(), 4LL);
        QCOMPARE(c["skinTempValues"].toLongLong(), 4LL);
        QCOMPARE(c["skinTempVariations"].toLongLong(), 1LL);
        QCOMPARE(c["daysTempDev"].toLongLong(), 1LL);
        // stored again without a variation: the day's goes too
        SkinTempNight again = n[3];
        again.hasVariation = false;
        QVERIFY(store->storeSkinTempNight(again));
        c = HealthDb::readCounts(healthDbPath());
        QCOMPARE(c["skinTempVariations"].toLongLong(), 0LL);
        QCOMPARE(c["daysTempDev"].toLongLong(), 0LL);
        // a sync reads the nights by seq
        const ChangePage page = HealthDb::changes(healthDbPath(), "skin_temp_night", seq0);
        QCOMPARE(page.rows.size(), 4);
        QCOMPARE(page.rows.last().value("wake_date").toString(), D0.addDays(3).toString(Qt::ISODate));
    }

    void aDatabaseOfVersionTwoIsGivenTheTable()
    {
        QVERIFY(store->beginWorkout(at(THU, 8), "walk") > 0);
        const qint64 seq = store->seq();
        store.reset();
        sqlite3 *raw = nullptr;
        QCOMPARE(sqlite3_open(QFile::encodeName(healthDbPath()).constData(), &raw), SQLITE_OK);
        QCOMPARE(sqlite3_exec(raw, "DROP TABLE skin_temp_night; PRAGMA user_version=2", nullptr, nullptr, nullptr),
                 SQLITE_OK);
        sqlite3_close(raw);
        // a reader of the old version finds no nights, and no error
        QVERIFY(HealthDb::readSkinTempNights(healthDbPath(), D0, THU).isEmpty());
        QCOMPARE(HealthDb::readCounts(healthDbPath()).value("skinTempNights").toLongLong(), 0LL);
        store.reset(new HealthDb(healthDbPath()));
        QVERIFY(store->isOpen());
        QCOMPARE(store->seq(), seq);
        QCOMPARE(HealthDb::readCounts(healthDbPath()).value("schema").toLongLong(), 3LL);
        QCOMPARE(HealthDb::readWorkouts(healthDbPath(), 0, at(THU.addDays(1), 0)).size(), 1);
        QVERIFY(store->storeSkinTempNight(buildNights({34.0})[0]));
        QVERIFY(store->seq() > seq); // its triggers are there
    }

    // ---- the morning, end to end ----
    void theMorningsNightIsStoredOnceAndShown()
    {
        const QDate today = QDate::currentDate();
        const QDate wake = today;
        // three earlier nights, then last night: stored by the sleep estimate, the skin temperature sampled
        for (const SkinTempNight &x : buildNights({33.9, 34.0, 34.1})) {
            SkinTempNight y = x;
            y.wakeDate = today.addDays(D0.daysTo(x.wakeDate) - 3);
            QVERIFY(store->storeSkinTempNight(y));
        }
        SleepNight night;
        night.valid = true;
        night.wakeDate = wake;
        night.main = sleepOf(wake);
        night.score = 85;
        QVERIFY(store->storeNight(night, QStringLiteral("eos-motion-hr-2")));
        QVERIFY(store->addSkinTemp(temps(night.main.startMs + 45 * 60000, night.main.endMs, 34.0, 0)));
        HealthMonitor *mon = makeMonitor(at(wake, 4, 30));
        mon->evaluate(); // before 05:00: not yet
        QVERIFY(HealthDb::readSkinTempNights(healthDbPath(), wake, wake).isEmpty());
        clock = at(wake, 7, 45);
        mon->evaluate();
        QList<SkinTempNight> r = HealthDb::readSkinTempNights(healthDbPath(), wake, wake);
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].minutes, 435);
        QCOMPARE(r[0].meanC, 34.0);
        QVERIFY(r[0].hasVariation);
        QVERIFY2(qAbs(r[0].devC) <= 0.02, qPrintable(QString::number(r[0].devC))); // as the three nights before
        QCOMPARE(r[0].nights, 3);
        QCOMPARE(HealthDb::readCounts(healthDbPath()).value("daysTempDev").toLongLong(), 1LL);
        const qint64 seq = store->seq();
        clock = at(wake, 8, 15);
        mon->evaluate();
        QCOMPARE(store->seq(), seq); // once a morning
        makeMonitor(at(wake, 9, 0))->evaluate(); // a new run: the row says it is done
        QCOMPARE(store->seq(), seq);
        // the Health app's loader
        HealthMetricsLoader loader;
        const QVariantMap v = loader.skinTemp(wake);
        QCOMPARE(v.value("state").toString(), QString("inRange"));
        QCOMPARE(v.value("hasVariation").toBool(), true);
        QCOMPARE(v.value("nightsLeft").toInt(), 0);
        const QVariantList week = loader.skinTempWeek(wake);
        QCOMPARE(week.size(), 7);
        QCOMPARE(week.last().toMap().value("state").toString(), QString("inRange"));
        QCOMPARE(week.last().toMap().value("date").toDate(), wake);
        QCOMPARE(week.first().toMap().value("hasVariation").toBool(), false);
        // while the sampling runs the card says "Tracking..."
        store->setMeta("skin_temp.on_ms", (qlonglong)(QDateTime::currentMSecsSinceEpoch() - 3600000));
        QCOMPARE(loader.skinTemp(wake).value("state").toString(), QString("tracking"));
    }
};

SENSORLOGD_TEST_MAIN(TestSkinTemp)
#include "tst_skintemp.moc"
