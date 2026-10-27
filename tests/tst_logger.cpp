// Logger (daemon/logger.cpp) on a private D-Bus: dbus-run-session's bus serves as both the session and the system
// bus (DBUS_SYSTEM_BUS_ADDRESS is pointed at it), and a second connection of the test plays sensorfwd
// (com.nokia.SensorService), mce (com.nokia.mce, display_status_ind) and the health app (resetup /
// triggerRecording calls). Checked: the sensors are set up only once sensorfwd's service is there (the fix for the
// "no sensor manager yet" spin), the settings choose the sensors, their intervals and continuous heart rate,
// resetup() replaces the sensors instead of adding a second set, a display-on signal compensates the timers for
// time in suspend, and the boot id decides whether the step counter's stored offset is trusted.
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QFile>
#include <QTest>
#include <memory>

#include "peek.h"
#include "testenv.h"

namespace {
const QString sensorfw = "com.nokia.SensorService";
const QString mce = "com.nokia.mce";
const QString loggerService = "org.asteroid.sensorlogd.logger";
const QString loggerPath = "/org/asteroid/sensorlogd/logger";

QString bootId()
{
    QFile f("/proc/sys/kernel/random/boot_id");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll()).trimmed();
}
}

class TestLogger : public QObject
{
    Q_OBJECT
    std::unique_ptr<Logger> logger;

    // the other programs on the bus: sensorfwd, mce, the health app
    QDBusConnection peer() { return QDBusConnection::connectToBus(QDBusConnection::SessionBus, "peer"); }

    void sensorfwUp(bool up)
    {
        if (up)
            QVERIFY(peer().registerService(sensorfw));
        else
            peer().unregisterService(sensorfw);
    }

    bool callLogger(const QString &method)
    {
        // asynchronous: the logger answers from this thread's event loop
        QDBusPendingCall call =
            peer().asyncCall(QDBusMessage::createMethodCall(loggerService, loggerPath, "", method));
        if (!QTest::qWaitFor([&] { return call.isFinished(); }, 5000))
            return false;
        if (call.isError())
            qWarning() << method << call.error().message();
        return !call.isError();
    }

    void display(const QString &state)
    {
        QDBusMessage signal = QDBusMessage::createSignal("/com/nokia/mce/signal", "com.nokia.mce.signal",
                                                         "display_status_ind");
        signal << state;
        QVERIFY(peer().send(signal));
    }

private slots:
    void initTestCase()
    {
        // never the machine's system bus: the test bus is both
        const QByteArray session = qgetenv("DBUS_SESSION_BUS_ADDRESS");
        QVERIFY2(!session.isEmpty(), "run the Logger test under dbus-run-session");
        qputenv("DBUS_SYSTEM_BUS_ADDRESS", session);
        QVERIFY(QDBusConnection::sessionBus().isConnected());
        QVERIFY(QDBusConnection::systemBus().isConnected());
        QVERIFY(peer().isConnected());
        QVERIFY(peer().registerService(mce));
        QVERIFY(!bootId().isEmpty());
    }

    void init()
    {
        testenv::clean();
        sensorfwUp(false);
    }

    void cleanup() { logger.reset(); }

    void waitsForSensorfw()
    {
        logger.reset(new Logger());
        QVERIFY(QDBusConnection::sessionBus().interface()->isServiceRegistered(loggerService).value());
        QVERIFY(!logger->m_setupDone);
        QVERIFY(!logger->m_stepCounter && !logger->m_heartrateSensor && !logger->m_barometerSensor);
        // no sensor exists before sensorfwd is there, so none is stuck in "no sensor manager yet"
        QCOMPARE(FakeSensorBackend::created(QStepCounterSensor::sensorType), 0);
        // a resetup from the settings app while waiting changes nothing (setup() reads the settings later)
        QVERIFY(callLogger("resetup"));
        QVERIFY(!logger->m_setupDone);
        QVERIFY(callLogger("triggerRecording")); // and a recording request does not crash
        display("on");
        QTest::qWait(100);

        sensorfwUp(true);
        QTRY_VERIFY(logger->m_setupDone);
        QVERIFY(logger->m_stepCounter && logger->m_heartrateSensor && logger->m_barometerSensor);
        StepCounterPlugin *steps = logger->m_stepCounter;
        // sensorfwd restarting later is the backend's business: no second set-up
        sensorfwUp(false);
        sensorfwUp(true);
        QTest::qWait(200);
        QCOMPARE(logger->m_stepCounter, steps);
        QCOMPARE(logger->findChildren<StepCounterPlugin *>().size(), 1);
    }

    void setsUpAtOnceWhenSensorfwIsThere()
    {
        sensorfwUp(true);
        logger.reset(new Logger());
        QVERIFY(logger->m_setupDone);
        QVERIFY(logger->heartrateSensorEnabled && logger->stepCounterEnabled && logger->barometerEnabled);
        QCOMPARE(logger->m_heartrateSensor->interval, 600000);
        QVERIFY(!logger->m_heartrateSensor->continuous);
        QCOMPARE(logger->m_stepCounter->interval, 600000);
        QCOMPARE(logger->m_barometerSensor->interval, 600000);
        // setup() records at once: the heart rate recording is running
        QVERIFY(FakeSensorBackend::last(QHrmSensor::sensorType)->running());
        QVERIFY(FakeSensorBackend::last(QPressureSensor::sensorType)->running());
    }

    void settingsChooseTheSensors()
    {
        testenv::setSetting("heartrateSensor/interval", 120000);
        testenv::setSetting("heartrateSensor/continuous", true);
        testenv::setSetting("stepCounter/interval", 300000);
        testenv::setSetting("barometer/enabled", false);
        sensorfwUp(true);
        logger.reset(new Logger());
        QCOMPARE(logger->m_heartrateSensor->interval, 120000);
        QVERIFY(logger->m_heartrateSensor->continuous);
        QCOMPARE(logger->m_stepCounter->interval, 300000);
        QVERIFY(!logger->barometerEnabled);
        QVERIFY(!logger->m_barometerSensor);
        QCOMPARE(FakeSensorBackend::created(QPressureSensor::sensorType), 0);
    }

    void allSensorsOff()
    {
        testenv::setSetting("heartrateSensor/enabled", false);
        testenv::setSetting("stepCounter/enabled", false);
        testenv::setSetting("barometer/enabled", false);
        sensorfwUp(true);
        logger.reset(new Logger());
        QVERIFY(logger->m_setupDone);
        QVERIFY(!logger->m_stepCounter && !logger->m_heartrateSensor && !logger->m_barometerSensor);
        QVERIFY(callLogger("triggerRecording"));
        display("on");
        QTest::qWait(100);
        QCOMPARE(FakeSensorBackend::created(QHrmSensor::sensorType), 0);
    }

    void resetupReplacesTheSensors()
    {
        sensorfwUp(true);
        logger.reset(new Logger());
        QVERIFY(logger->m_barometerSensor);
        // the settings app turns the barometer off and makes heart rate continuous, then calls resetup
        testenv::setSetting("barometer/enabled", false);
        testenv::setSetting("heartrateSensor/continuous", true);
        QVERIFY(callLogger("resetup"));
        QCOMPARE(logger->findChildren<HeartrateSensorPlugin *>().size(), 1);
        QCOMPARE(logger->findChildren<StepCounterPlugin *>().size(), 1);
        QCOMPARE(logger->findChildren<BarometerSensorPlugin *>().size(), 0);
        QVERIFY(!logger->m_barometerSensor);
        QVERIFY(logger->m_heartrateSensor->continuous);
        // the old heart rate sensor is gone with its plugin; the new one runs continuously
        QVERIFY(FakeSensorBackend::last(QHrmSensor::sensorType)->running());
        QVERIFY(!FakeSensorBackend::last(QPressureSensor::sensorType)); // the barometer's sensor was destroyed
    }

    void triggerRecordingOverDBus()
    {
        sensorfwUp(true);
        logger.reset(new Logger());
        FakeSensorBackend *hr = FakeSensorBackend::last(QHrmSensor::sensorType);
        hr->pushHeartRate(70, QHrmSensor::AccuracyHigh); // finishes setup()'s recording
        QVERIFY(!hr->running());
        QVERIFY(callLogger("triggerRecording"));
        QCOMPARE(hr->starts(), 2);
        QVERIFY(hr->running());
    }

    void displayOnCompensatesForSuspend()
    {
        sensorfwUp(true);
        logger.reset(new Logger());
        FakeSensorBackend *hr = FakeSensorBackend::last(QHrmSensor::sensorType);
        hr->pushHeartRate(70, QHrmSensor::AccuracyHigh);
        QCOMPARE(hr->starts(), 1);
        // an hour in suspend: the interval timers did not run
        logger->m_heartrateSensor->lastRecordTime = QDateTime::currentDateTime().addSecs(-3600);
        logger->m_barometerSensor->lastRecordTime = QDateTime::currentDateTime().addSecs(-3600);
        logger->m_stepCounter->lastRecordTime = QDateTime::currentDateTime().addSecs(-60);
        display("off");
        QTest::qWait(200);
        QCOMPARE(hr->starts(), 1); // display off: nothing
        display("on");
        QTRY_COMPARE(hr->starts(), 2); // overdue: recorded now
        const int remaining = logger->m_stepCounter->recordIntervalTimer->remainingTime();
        QVERIFY2(remaining <= 567000 && remaining > 500000, // a coarse timer: 5 % slack
                 qPrintable(QString::number(remaining)));
    }

    void bootIdDecidesFreshness()
    {
        sensorfwUp(true);
        logger.reset(new Logger());
        QVERIFY(logger->daemonFresh); // first start this boot
        QVERIFY(logger->m_stepCounter->m_daemonFresh);
        QCOMPARE(testenv::setting("lastBootId").toString(), bootId());
        logger.reset(); // the first one gives up its D-Bus object first
        logger.reset(new Logger());
        QVERIFY(!logger->daemonFresh); // restarted within the boot: the stored offset holds
        QVERIFY(logger->m_stepCounter->m_storedOffset);
        testenv::setSetting("lastBootId", "00000000-0000-0000-0000-000000000000");
        QVERIFY(callLogger("resetup"));
        QVERIFY(logger->daemonFresh);
    }
};

SENSORLOGD_TEST_MAIN(TestLogger)
#include "tst_logger.moc"
