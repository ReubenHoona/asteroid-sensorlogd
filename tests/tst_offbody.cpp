// OffBodyPlugin (daemon/sensorPlugins/offBody.cpp, M4 WO-03): the sensor hub's off-body state as sensorfwd's hub
// connection says it (org.eos.UsfHub GetOffBody, OffBodyChanged), logged as offBody/ changes; the Health monitor's
// worn() from it (a table, and a reminder to move that is not made off the wrist); the off-wrist ranges read back
// from the day files, which the day's resting heart rate and the night's estimate leave out. A fake hub on the bus
// (run under dbus-run-session, the session bus standing in for the system bus). Every value is synthetic.
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QSignalSpy>
#include <QTest>
#include <memory>

#include "peek.h"
#include "../common.h"
#include "../daemon/sensorPlugins/heartrateDb.h"
#include "../metrics/healthData.h"
#include "../metrics/restingHr.h"
#include "testenv.h"

using namespace Health;

namespace {

const QDate THU(2026, 11, 12);

qint64 ms(const QDate &d, int h, int m = 0, int s = 0) { return QDateTime(d, QTime(h, m, s)).toMSecsSinceEpoch(); }
QString rec(qint64 ms, int v) { return QString::number(ms / 1000) + ":" + QString::number(v); }
QByteArray recs(const QList<QPair<qint64, int>> &r)
{
    QByteArray b;
    for (const auto &p : r)
        b += rec(p.first, p.second).toLatin1() + "\n";
    return b;
}
QStringList todays() { return testenv::dayLines("offBody", QDate::currentDate()); }
int valueOf(const QString &line) { return line.section(':', 1).toInt(); }

// a reading a minute of [from, from + minutes) at bpm, accuracy high
void readings(qint64 from, int minutes, int bpm)
{
    HeartrateDb db(heartrateDbPath());
    QVector<HeartrateDb::Reading> rows;
    for (int i = 0; i < minutes; i++)
        rows.append({from + (qint64)i * 60000 + 20000, bpm, 4});
    QVERIFY(db.add(rows));
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

HealthProfile profile()
{
    HealthProfile p;
    p.sex = HealthProfile::Male;
    p.heightCm = 175;
    p.weightKg = 70;
    p.age = 30;
    return p;
}

}

// sensorfwd's hub connection as the plugin sees it
class FakeHub : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.eos.UsfHub")
public:
    QDBusConnection bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("hub"));
    int state = -1;
    int queries = 0;
    bool registered = false;
    bool up()
    {
        if (!registered)
            registered = bus.registerObject(QStringLiteral("/org/eos/usfhub"), this,
                                            QDBusConnection::ExportScriptableContents);
        return registered && bus.registerService(QStringLiteral("com.nokia.SensorService"));
    }
    void down() { bus.unregisterService(QStringLiteral("com.nokia.SensorService")); }
    void change(int s)
    {
        state = s;
        QDBusMessage m = QDBusMessage::createSignal(QStringLiteral("/org/eos/usfhub"), QStringLiteral("org.eos.UsfHub"),
                                                    QStringLiteral("OffBodyChanged"));
        m << s;
        bus.send(m);
    }
public slots:
    Q_SCRIPTABLE int GetOffBody() { queries++; return state; }
};

class TestOffBody : public QObject
{
    Q_OBJECT
    std::unique_ptr<OffBodyPlugin> plugin;
    FakeHub *hub = nullptr;
    FakePoster poster;
    QSettings *settings = nullptr;
    qint64 clock = 0;
    QScopedPointer<HealthMonitor> monitor;

    void make() { plugin.reset(new OffBodyPlugin()); }
    HealthMonitor *makeMonitor(qint64 at)
    {
        monitor.reset();
        clock = at;
        monitor.reset(new HealthMonitor(settings, &poster));
        monitor->setClock([this] { return clock; });
        monitor->start();
        return monitor.data();
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
        poster.posted.clear();
        hub->state = -1;
    }
    void cleanup()
    {
        plugin.reset();
        monitor.reset();
    }

    // ---- the plugin ----
    void asksAtTheStartAndLogsTheState()
    {
        hub->state = 0;
        const int q0 = hub->queries;
        make();
        QSignalSpy spy(plugin.get(), &OffBodyPlugin::changed);
        QTRY_COMPARE(plugin->state(), 0);
        QCOMPARE(hub->queries, q0 + 1);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(todays().size(), 1);
        QCOMPARE(valueOf(todays()[0]), 0);
        QVERIFY(qAbs(todays()[0].section(':', 0, 0).toLongLong() * 1000 - QDateTime::currentMSecsSinceEpoch()) < 5000);
    }

    void followsTheSignalAndLogsChangesOnly()
    {
        hub->state = 0;
        make();
        QTRY_COMPARE(plugin->state(), 0);
        hub->change(1); // taken off
        QTRY_COMPARE(plugin->state(), 1);
        hub->change(1); // the same again: no record
        hub->change(0); // on again
        QTRY_COMPARE(plugin->state(), 0);
        hub->change(-1); // sensorfwd's hub went away
        QTRY_COMPARE(plugin->state(), -1);
        QStringList values;
        for (const QString &l : todays())
            values << QString::number(valueOf(l));
        QCOMPARE(values, QStringList({"0", "1", "0", "-1"}));
        // sensorfwd is back: asked again
        hub->state = 1;
        plugin->query();
        QTRY_COMPARE(plugin->state(), 1);
        QCOMPARE(todays().size(), 5);
        QCOMPARE(valueOf(todays().last()), 1);
    }

    void unknownWithNothingBeforeIsNotLogged()
    {
        hub->state = -1; // the provider off in eos-usf.conf: GetOffBody answers -1
        make();
        QTRY_COMPARE(hub->queries > 0, true);
        QTest::qWait(200);
        QCOMPARE(plugin->state(), -1);
        QVERIFY(todays().isEmpty());
        // no hub connection at all (an older sensorfwd): unknown, nothing logged
        hub->down();
        plugin->query();
        QTest::qWait(300);
        QCOMPARE(plugin->state(), -1);
        QVERIFY(todays().isEmpty());
        QVERIFY(hub->up());
    }

    void aRestartDoesNotLogTheSameStateAgain()
    {
        const qint64 earlier = QDateTime::currentMSecsSinceEpoch() - 60000;
        if (QDateTime::fromMSecsSinceEpoch(earlier).date() != QDate::currentDate())
            QSKIP("near midnight");
        testenv::writeDay("offBody", QDate::currentDate(), recs({{earlier, 1}}));
        hub->state = 1;
        make();
        QTRY_COMPARE(plugin->state(), 1);
        QCOMPARE(todays().size(), 1);
        hub->change(0);
        QTRY_COMPARE(todays().size(), 2);
    }

    // ---- the monitor's worn() ----
    void wornFollowsTheSensorWhenItKnows_data()
    {
        QTest::addColumn<int>("offBody");
        QTest::addColumn<bool>("hrAttached");
        QTest::addColumn<int>("hrAgeMin"); // -1: no heart rate yet
        QTest::addColumn<bool>("worn");
        // the sensor knows: it decides
        QTest::newRow("on the wrist, heart rate just now") << 0 << true << 1 << true;
        QTest::newRow("on the wrist, no heart rate for an hour") << 0 << true << 60 << true;
        QTest::newRow("on the wrist, no heart rate yet") << 0 << true << -1 << true;
        QTest::newRow("off the wrist, heart rate just now") << 1 << true << 1 << false;
        QTest::newRow("off the wrist, no heart rate plugin") << 1 << false << -1 << false;
        // it does not know: the heart rate, as before WO-03
        QTest::newRow("unknown, heart rate 10 min ago") << -1 << true << 10 << true;
        QTest::newRow("unknown, heart rate 20 min ago") << -1 << true << 20 << false;
        QTest::newRow("unknown, no heart rate yet") << -1 << true << -1 << false;
        QTest::newRow("unknown, no heart rate plugin") << -1 << false << -1 << true;
    }
    void wornFollowsTheSensorWhenItKnows()
    {
        QFETCH(int, offBody);
        QFETCH(bool, hrAttached);
        QFETCH(int, hrAgeMin);
        QFETCH(bool, worn);
        HealthMonitor *m = makeMonitor(ms(THU, 10, 0));
        m->m_hrAttached = hrAttached;
        m->m_lastHrMs = hrAgeMin < 0 ? 0 : clock - hrAgeMin * 60000LL;
        m->setOffBody(offBody);
        QCOMPARE(m->worn(clock), worn);
    }

    void noReminderToMoveOffTheWrist()
    {
        settings->setValue("reminders/enabled", true);
        settings->sync();
        HealthMonitor *m = makeMonitor(ms(THU, 10, 30));
        m->m_hrAttached = true;
        // a heart rate a minute ago, but the sensor says off the wrist: the watch is on a desk
        m->ingestHeart(ms(THU, 10, 49), 70);
        m->setOffBody(1);
        clock = ms(THU, 10, 50, 10);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 0);
        // the next hour, on the wrist without a heart rate reading (the hub's readings paused): reminded
        m->setOffBody(0);
        clock = ms(THU, 11, 50, 10);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        QCOMPARE(poster.posted[0].summary, QString("Reminder to move"));
    }

    void theLoggerHandsTheStateToTheMonitor()
    {
        hub->state = 1;
        Logger logger;
        QVERIFY(logger.m_offBody);
        QVERIFY(logger.m_health);
        QTRY_COMPARE(logger.m_health->m_offBody, 1);
        hub->change(0);
        QTRY_COMPARE(logger.m_health->m_offBody, 0);
        // sensorfwd came back: asked again
        const int q = hub->queries;
        hub->state = 1;
        logger.sensorfwRegistered();
        QTRY_COMPARE(hub->queries, q + 1);
        QTRY_COMPARE(logger.m_health->m_offBody, 1);
    }

    // ---- what is left out off the wrist ----
    void offWristRangesFromTheDayFiles()
    {
        const QDate wed = THU.addDays(-1);
        testenv::writeDay("offBody", THU.addDays(-4), recs({{ms(THU.addDays(-4), 9), 0}}));
        testenv::writeDay("offBody", wed, recs({{ms(wed, 21, 0), 1}}));
        testenv::writeDay("offBody", THU, recs({{ms(THU, 7, 0), 0}, {ms(THU, 12, 0), 1}, {ms(THU, 12, 30), -1},
                                                {ms(THU, 13, 0), 1}, {ms(THU, 13, 10), 1}}));
        const auto r = offWristRanges(ms(wed, 18, 0), ms(THU, 18, 0));
        QCOMPARE(r.size(), 3);
        QCOMPARE(r[0], qMakePair(ms(wed, 21, 0), ms(THU, 7, 0)));
        QCOMPARE(r[1], qMakePair(ms(THU, 12, 0), ms(THU, 12, 30))); // unknown ends it
        QCOMPARE(r[2], qMakePair(ms(THU, 13, 0), ms(THU, 18, 0)));
        // the state at the start comes from the last record before it, days back if need be
        const auto later = offWristRanges(ms(wed, 22, 0), ms(wed, 23, 0));
        QCOMPARE(later.size(), 1);
        QCOMPARE(later[0], qMakePair(ms(wed, 22, 0), ms(wed, 23, 0)));
        QVERIFY(offWristRanges(ms(wed, 8, 0), ms(wed, 20, 0)).isEmpty()); // on since the day before yesterday's record
        // no records: nothing is known to be off
        QVERIFY(offWristRanges(ms(THU.addDays(-10), 0), ms(THU.addDays(-9), 0)).isEmpty());
    }

    void theRestingRateLeavesOutTheTimeOffTheWrist()
    {
        // 00:00-04:00 asleep at 52, 04:00-08:00 readings at 90 from a watch off the wrist (not the wearer's)
        readings(ms(THU, 0, 0), 240, 52);
        readings(ms(THU, 4, 0), 240, 90);
        QCOMPARE(estimateRestingHrForDay(THU, 0), 71); // the median of both halves: the sensor did not say
        testenv::writeDay("offBody", THU, recs({{ms(THU, 4, 0), 1}, {ms(THU, 8, 0), 0}}));
        QCOMPARE(estimateRestingHrForDay(THU, 0), 52);
    }

    void theNightLeavesOutTheTimeOffTheWrist()
    {
        const QDate wed = THU.addDays(-1);
        // asleep 23:00-03:00 at 55; the watch then lies on the table, its readings at 50 are not the wearer's
        readings(ms(wed, 23, 0), 240, 55);
        readings(ms(THU, 3, 0), 240, 50);
        SleepNight n = estimateNight(THU, profile(), 56);
        QVERIFY(n.valid);
        QCOMPARE(n.main.endMs, ms(THU, 7, 0)); // without the sensor the readings look like sleep
        testenv::writeDay("offBody", wed, recs({{ms(wed, 22, 50), 0}}));
        testenv::writeDay("offBody", THU, recs({{ms(THU, 3, 0), 1}, {ms(THU, 7, 30), 0}}));
        n = estimateNight(THU, profile(), 56);
        QVERIFY(n.valid);
        QCOMPARE(n.main.startMs, ms(wed, 23, 0));
        QCOMPARE(n.main.endMs, ms(THU, 3, 0));
        QCOMPARE(n.main.asleepMin, 240);
    }
};

SENSORLOGD_TEST_MAIN(TestOffBody)
#include "tst_offbody.moc"
