// The workout latency the daemon asks of the sensor hub (daemon/logger.*, M4 WO-08): while a workout is on and the
// app that runs it is on the bus, org.eos.UsfHub.SetWorkoutMode(true) goes to sensorfwd's hub connection, at the
// workout's end, the app's death or a setting off SetWorkoutMode(false); a new sensorfwd is told again. A fake hub
// owns com.nokia.SensorService (run under dbus-run-session, the session bus standing in for the system bus), the
// app is a connection of its own that calls the workout methods as asteroid-exercise does. The workouts are invented.
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingReply>
#include <QTest>

#include "../common.h"
#include "peek.h"
#include "testenv.h"

namespace {
const QDate SAT(2026, 11, 21);
qint64 ms(int h, int m = 0) { return QDateTime(SAT, QTime(h, m)).toMSecsSinceEpoch(); }
}

class FakeHub : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.eos.UsfHub")
public:
    explicit FakeHub(const QString &name) : name_(name), bus(QDBusConnection::connectToBus(QDBusConnection::SessionBus, name)) {}
    ~FakeHub() override
    {
        bus.unregisterService(QStringLiteral("com.nokia.SensorService"));
        QDBusConnection::disconnectFromBus(name_);
    }
    QString name_;
    QDBusConnection bus;
    QList<bool> modes;
    int subscribes = 0;
    bool up()
    {
        return bus.registerObject(QStringLiteral("/org/eos/usfhub"), this, QDBusConnection::ExportScriptableContents) &&
               bus.registerService(QStringLiteral("com.nokia.SensorService"));
    }
public slots:
    Q_SCRIPTABLE int SetWorkoutMode(bool on) { modes << on; return 0; }
    Q_SCRIPTABLE int SubscribeActivity() { subscribes++; return 3; }
    Q_SCRIPTABLE int UnsubscribeActivity() { return 0; }
};

// an app on a connection of its own (the Exercise app)
class App
{
public:
    explicit App(const QString &name) : name_(name), bus(QDBusConnection::connectToBus(QDBusConnection::SessionBus, name)) {}
    QVariant call(const char *method, const QVariantList &args)
    {
        QDBusMessage m = QDBusMessage::createMethodCall("org.asteroid.sensorlogd.logger", "/org/asteroid/sensorlogd/logger",
                                                        "org.asteroid.sensorlogd.logger", QLatin1String(method));
        m.setArguments(args);
        QDBusPendingCall pc = bus.asyncCall(m, 5000);
        while (!pc.isFinished())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QDBusMessage r = pc.reply();
        return r.arguments().value(0);
    }
    // the process ends (exit, crash, kill): its connection closes once no copy is left
    void quit()
    {
        bus = QDBusConnection(QString());
        QDBusConnection::disconnectFromBus(name_);
    }
    QString name_;
    QDBusConnection bus;
};

class TestWorkoutHub : public QObject
{
    Q_OBJECT
    Logger *logger = nullptr;
    FakeHub *hub = nullptr;

    QVariantMap summary() { return {{"activeS", 600}, {"steps", 900}}; }

private slots:
    void initTestCase()
    {
        QVERIFY2(QDBusConnection::sessionBus().isConnected(), "run under dbus-run-session");
        testenv::clean();
        logger = new Logger();
        hub = new FakeHub(QStringLiteral("hub1"));
        QVERIFY(hub->up());
        QTRY_VERIFY(logger->m_setupDone); // the Logger sets up once sensorfwd (the fake hub) is on the bus
        QTRY_COMPARE(hub->subscribes, 1);  // the activity streams (WO-05)
    }
    void cleanupTestCase()
    {
        delete logger;
        delete hub;
    }

    void aWorkoutOnAndOff()
    {
        App app(QStringLiteral("app1"));
        const qlonglong id = app.call("BeginWorkout", {(qlonglong)ms(9), QString("walk"), QString("manual"), false}).toLongLong();
        QVERIFY(id > 0);
        QTRY_COMPARE(hub->modes, QList<bool>({true}));
        QVERIFY(logger->m_hubWorkout);
        // the app's other calls keep it on, nothing new is sent
        QVERIFY(app.call("SetPaused", {id, true, (qlonglong)ms(9, 5), false}).toBool());
        QVERIFY(app.call("SaveWorkoutState", {id, (qlonglong)ms(9, 6), QString("{}")}).toBool());
        QTest::qWait(200);
        QCOMPARE(hub->modes.size(), 1);
        QVERIFY(app.call("EndWorkout", {id, (qlonglong)ms(9, 10), summary()}).toBool());
        QTRY_COMPARE(hub->modes, QList<bool>({true, false}));
        app.quit();
        QTest::qWait(200);
        QCOMPARE(hub->modes.size(), 2); // its leaving after the end changes nothing
    }

    void theAppDiesInAWorkout()
    {
        hub->modes.clear();
        App *app = new App(QStringLiteral("app2"));
        const qlonglong id = app->call("BeginWorkout", {(qlonglong)ms(10), QString("run"), QString("manual"), false}).toLongLong();
        QVERIFY(id > 0);
        QTRY_COMPARE(hub->modes, QList<bool>({true}));
        app->quit(); // killed
        delete app;
        QTRY_COMPARE(hub->modes, QList<bool>({true, false}));
        // the workout stays on in health.db for the app's recovery
        QCOMPARE(logger->m_store->openWorkoutCount(), 1);
        // the app starts again and goes on with it (its snapshot): on again
        App again(QStringLiteral("app3"));
        QVERIFY(again.call("SaveWorkoutState", {id, (qlonglong)ms(10, 20), QString("{}")}).toBool());
        QTRY_COMPARE(hub->modes, QList<bool>({true, false, true}));
        QVERIFY(again.call("EndWorkout", {id, (qlonglong)ms(10, 30), summary()}).toBool());
        QTRY_COMPARE(hub->modes, QList<bool>({true, false, true, false}));
        again.quit();
    }

    void aNewSensorfwdIsToldAgain()
    {
        hub->modes.clear();
        App app(QStringLiteral("app4"));
        const qlonglong id = app.call("BeginWorkout", {(qlonglong)ms(11), QString("walk"), QString("manual"), false}).toLongLong();
        QTRY_COMPARE(hub->modes, QList<bool>({true}));
        // sensorfwd restarts: a new hub connection that knows nothing
        delete hub;
        hub = new FakeHub(QStringLiteral("hub2"));
        QVERIFY(hub->up());
        QTRY_COMPARE(hub->modes, QList<bool>({true}));
        QTRY_COMPARE(hub->subscribes, 1);
        QVERIFY(app.call("EndWorkout", {id, (qlonglong)ms(11, 15), summary()}).toBool());
        QTRY_COMPARE(hub->modes, QList<bool>({true, false}));
        app.quit();
    }

    void notFromInsideAndNotWhenSwitchedOff()
    {
        hub->modes.clear();
        // a call that is not over the bus has no app to watch: no workout latency
        const qlonglong id = logger->BeginWorkout(ms(12), "walk", "manual", false);
        QVERIFY(id > 0);
        QTest::qWait(300);
        QVERIFY(hub->modes.isEmpty());
        QVERIFY(logger->EndWorkout(id, ms(12, 5), summary()));
        // workoutHr/fast = false: the batch latency in workouts too
        testenv::setSetting("workoutHr/fast", false);
        logger->settings->sync();
        App app(QStringLiteral("app5"));
        const qlonglong id2 = app.call("BeginWorkout", {(qlonglong)ms(13), QString("walk"), QString("manual"), false}).toLongLong();
        QVERIFY(id2 > 0);
        QTest::qWait(300);
        QVERIFY(hub->modes.isEmpty());
        QVERIFY(app.call("EndWorkout", {id2, (qlonglong)ms(13, 5), summary()}).toBool());
        app.quit();
        testenv::setSetting("workoutHr/fast", true);
        logger->settings->sync();
    }
};

SENSORLOGD_TEST_MAIN(TestWorkoutHub)
#include "tst_workouthub.moc"
