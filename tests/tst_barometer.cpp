// BarometerSensorPlugin (daemon/sensorPlugins/barometerSensor.cpp) against the fake QtSensors backend: a recording
// starts the sensor, a zero reading is not logged, the first real one is written raw and compensated by the dconf
// calibration offset and stops the sensor, a recording without a reading is given up after the timeout, the
// interval timer and its suspend compensation.
#include <QRegularExpression>
#include <QTest>
#include <memory>

#include "../common.h"
#include "peek.h"
#include "testenv.h"

namespace {
const QByteArray type = QPressureSensor::sensorType;
const QString offsetKey = "/org/asteroidos/sensors/barometer-offset";

FakeSensorBackend *backend()
{
    FakeSensorBackend *b = FakeSensorBackend::last(type);
    if (!b)
        qFatal("no pressure backend");
    return b;
}
}

class TestBarometer : public QObject
{
    Q_OBJECT
    std::unique_ptr<BarometerSensorPlugin> plugin;

    BarometerSensorPlugin *make(int interval = 600000)
    {
        plugin.reset(new BarometerSensorPlugin(nullptr, interval));
        return plugin.get();
    }

private slots:
    void init()
    {
        testenv::clean();
        MGConfItem::clearTestValues();
    }
    void cleanup() { plugin.reset(); }

    void waitsForTheTimer()
    {
        make();
        QVERIFY(QDir(testenv::root() + "barometer-raw").exists());
        QVERIFY(QDir(testenv::root() + "barometer-compensated").exists());
        QVERIFY(plugin->recordIntervalTimer->isActive());
        QVERIFY(!plugin->recordTimeoutTimer->isActive());
        QCOMPARE(plugin->recordTimeoutTimer->interval(), 10000);
        QVERIFY(plugin->barometerSensor->isAlwaysOn());
        QCOMPARE(FakeSensorBackend::created(type), 0);
    }

    void recordingLogsRawAndCompensated()
    {
        MGConfItem::setTestValue(offsetKey, 250);
        make();
        plugin->triggerRecording();
        QVERIFY(backend()->running());
        QVERIFY(plugin->recordTimeoutTimer->isActive());
        backend()->pushPressure(0);
        QVERIFY(testenv::dayLines("barometer-raw").isEmpty());
        QVERIFY(backend()->running());
        backend()->pushPressure(101325);
        QCOMPARE(testenv::dayValues("barometer-raw"), QStringList({"101325"}));
        QCOMPARE(testenv::dayValues("barometer-compensated"), QStringList({"101075"}));
        QCOMPARE(testenv::dayTimes("barometer-raw"), testenv::dayTimes("barometer-compensated"));
        QVERIFY(!backend()->running());
        QVERIFY(!plugin->recordTimeoutTimer->isActive());
    }

    void noCalibrationMeansNoOffset()
    {
        make();
        plugin->triggerRecording();
        backend()->pushPressure(99000);
        QCOMPARE(testenv::dayValues("barometer-compensated"), QStringList({"99000"}));
    }

    void recordingWithoutAReadingIsGivenUp()
    {
        make();
        plugin->triggerRecording();
        QVERIFY(QMetaObject::invokeMethod(plugin.get(), "abortRecording"));
        QVERIFY(!backend()->running());
        QVERIFY(testenv::dayLines("barometer-raw").isEmpty());
    }

    void timeoutTimerGivesUp()
    {
        make();
        plugin->recordTimeoutTimer->setInterval(20);
        plugin->triggerRecording();
        QVERIFY(backend()->running());
        QTRY_VERIFY(!backend()->running());
    }

    void intervalTimerStartsRecordings()
    {
        make(30);
        QTRY_VERIFY(FakeSensorBackend::last(type) && backend()->starts() >= 1);
    }

    void timeUpdateRecordsWhenOverdue()
    {
        make();
        plugin->lastRecordTime = QDateTime::currentDateTime().addSecs(-3600);
        plugin->timeUpdate();
        QVERIFY(backend()->running());
        QVERIFY(plugin->recordIntervalTimer->remainingTime() > 590000);
    }

    void timeUpdateShortensTheTimer()
    {
        make();
        plugin->lastRecordTime = QDateTime::currentDateTime().addSecs(-300);
        plugin->timeUpdate();
        QCOMPARE(FakeSensorBackend::created(type), 0);
        const int remaining = plugin->recordIntervalTimer->remainingTime();
        QVERIFY2(remaining <= 315000 && remaining > 285000, // a coarse timer: 5 % slack
                 qPrintable(QString::number(remaining)));
    }
};

SENSORLOGD_TEST_MAIN(TestBarometer)
#include "tst_barometer.moc"
