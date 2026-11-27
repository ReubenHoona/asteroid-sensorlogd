// ActivityStreamsPlugin (daemon/sensorPlugins/activityStreams.cpp, M4 WO-05): the sensor hub's motion detect,
// stationary detect and step cadence as sensorfwd's hub connection sends them (org.eos.UsfHub ActivitySamples),
// written as motion/ changes and stepCadence/ minutes. A fake hub on the bus (run under dbus-run-session, the session
// bus standing in for the system bus) answers SubscribeActivity and sends the signal with the real D-Bus types.
// The samples are synthetic; their time stamps are CLOCK_BOOTTIME microseconds, as the hub's are.
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QTest>
#include <memory>
#include <time.h>

#include "../common.h"
#include "peek.h"
#include "testenv.h"
#include "../daemon/sensorPlugins/sampleTime.h"

namespace {
// the CLOCK_BOOTTIME time stamp of a wall clock time, with the offset readingWallMs() keeps (sampleTime.h), so
// that the plugin reads back exactly that wall clock time
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
qint64 minuteAgo(int minutes)
{
    return (QDateTime::currentMSecsSinceEpoch() / 60000 - minutes) * 60000;
}
QStringList lines(const QString &prefix)
{
    const QDate today = QDate::currentDate();
    return testenv::dayLines(prefix, today.addDays(-1)) + testenv::dayLines(prefix, today);
}
QString rec(qint64 ms, int v) { return QString::number(ms / 1000) + ":" + QString::number(v); }
}

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
    void send(int type, const QList<qlonglong> &t, const QList<double> &v)
    {
        QDBusMessage s = QDBusMessage::createSignal(QStringLiteral("/org/eos/usfhub"), QStringLiteral("org.eos.UsfHub"),
                                                    QStringLiteral("ActivitySamples"));
        s << type << QVariant::fromValue(t) << QVariant::fromValue(v);
        bus.send(s);
    }
public slots:
    Q_SCRIPTABLE int SubscribeActivity() { subscribes++; return 3; }
    Q_SCRIPTABLE int UnsubscribeActivity() { unsubscribes++; return 0; }
};

class TestActivity : public QObject
{
    Q_OBJECT
    std::unique_ptr<ActivityStreamsPlugin> plugin;
    FakeHub *hub = nullptr;

    void make() { plugin.reset(new ActivityStreamsPlugin()); }
    void samples(int type, const QList<qint64> &wallMs, const QList<double> &v)
    {
        QList<qlonglong> t;
        for (qint64 ms : wallMs)
            t << bootUsAt(ms);
        plugin->samples(type, t, v);
    }
    void settle() { QTest::qWait(ActivityStreamsPlugin::SETTLE_MS + 150); }

private slots:
    void initTestCase()
    {
        QVERIFY2(QDBusConnection::sessionBus().isConnected(), "run under dbus-run-session");
        QVERIFY2(QDBusConnection::systemBus().isConnected(), "DBUS_SYSTEM_BUS_ADDRESS set to the session bus");
        hub = new FakeHub();
        QVERIFY(hub->up());
    }
    void cleanupTestCase() { delete hub; }
    void init() { testenv::clean(); }
    void cleanup() { plugin.reset(); }

    void subscribesAndUnsubscribes()
    {
        const int s0 = hub->subscribes, u0 = hub->unsubscribes;
        make();
        QTRY_COMPARE(hub->subscribes, s0 + 1);
        QVERIFY(QDir(testenv::root() + "motion").exists() && QDir(testenv::root() + "stepCadence").exists());
        plugin->subscribe(); // sensorfwd came back
        QTRY_COMPARE(hub->subscribes, s0 + 2);
        plugin.reset();
        QTRY_COMPARE(hub->unsubscribes, u0 + 1);
    }

    void motionChangesInTimeOrder()
    {
        make();
        const qint64 m = minuteAgo(30);
        // both armed at the start, then stillness, a movement, stillness, as the hub sends them
        samples(0x20, {m}, {-1});
        samples(0x21, {m}, {-1});
        samples(0x21, {m + 6000}, {1.0});
        settle();
        QCOMPARE(lines("motion"), QStringList({rec(m, 2), rec(m + 6000, 0)}));
        // one delivery after a sleep: the signals per type, not in time order between them
        samples(0x20, {m + 600000, m + 900000}, {1.0, -1});
        samples(0x21, {m + 300000, m + 900000}, {1.0, -1});
        settle();
        QCOMPARE(lines("motion"), QStringList({rec(m, 2), rec(m + 6000, 0), rec(m + 600000, 1), rec(m + 900000, 2)}));
        // the same state again is not a change; an older record is not written
        samples(0x21, {m + 950000, m + 960000}, {1.0, 1.0});
        samples(0x20, {m + 100000}, {1.0});
        settle();
        QCOMPARE(lines("motion").size(), 5);
        QCOMPARE(lines("motion").last(), rec(m + 950000, 0));
    }

    void cadenceMinutes()
    {
        make();
        const qint64 m = minuteAgo(40);
        // on change: 100 for 30 s, 120 for 40 s (into the next minute), then 0; a reading holds 60 s at most
        samples(0x3a, {m, m + 30000, m + 70000}, {100, 120, 0});
        settle();
        QCOMPARE(lines("stepCadence"), QStringList({rec(m, 110), rec(m + 60000, 20)}));
        // a walk that the next reading ends 5 minutes later: the reading holds for one minute only
        samples(0x3a, {m + 600000}, {90});
        samples(0x3a, {m + 900000}, {0});
        settle();
        QCOMPARE(lines("stepCadence").mid(2), QStringList({rec(m + 600000, 90)}));
        // the plugin goes: the open minute as far as it went (0 steps: no record)
        plugin.reset();
        QCOMPARE(lines("stepCadence").size(), 3);
    }

    void aRestartDoesNotWriteAgain()
    {
        const qint64 m = minuteAgo(15);
        if (QDateTime::fromMSecsSinceEpoch(m).date() != QDate::currentDate())
            QSKIP("near midnight");
        testenv::writeDay("stepCadence", QDate::currentDate(), rec(m, 100).toLatin1() + "\n");
        testenv::writeDay("motion", QDate::currentDate(), rec(m + 1000, 1).toLatin1() + "\n");
        make();
        samples(0x3a, {m, m + 60000}, {100, 0});
        samples(0x21, {m + 500}, {1.0});
        samples(0x21, {m + 2000}, {1.0});
        settle();
        QCOMPARE(lines("stepCadence"), QStringList({rec(m, 100)}));
        QCOMPARE(lines("motion"), QStringList({rec(m + 1000, 1), rec(m + 2000, 0)}));
    }

    void theSignalFromTheHub()
    {
        make();
        const qint64 m = minuteAgo(25);
        hub->send(0x21, {bootUsAt(m + 1000)}, {1.0});
        hub->send(0x3a, {bootUsAt(m), bootUsAt(m + 60000)}, {104.0, 0.0});
        QTRY_COMPARE(lines("motion"), QStringList({rec(m + 1000, 0)}));
        QTRY_COMPARE(lines("stepCadence"), QStringList({rec(m, 104)}));
    }
};

SENSORLOGD_TEST_MAIN(TestActivity)
#include "tst_activity.moc"
