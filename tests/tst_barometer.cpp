// BarometerSensorPlugin (daemon/sensorPlugins/barometerSensor.cpp) against the fake QtSensors backend: the sensor
// starts at once and stays on (the sensor hub batches pressure with the screen off, M4 WO-05); one record a minute,
// the median of its readings dated by its last one, raw and compensated by the dconf calibration offset; a batch of
// many minutes gives a record each, the minute still open is written at triggerRecording() and when the plugin goes;
// zero readings, replays, readings out of order and what a restart finds in the file are not written again.
// The readings are synthetic; their time stamps are CLOCK_BOOTTIME microseconds, as sensorfw's are.
#include <QTest>
#include <memory>
#include <time.h>

#include "../common.h"
#include "peek.h"
#include "testenv.h"
#include "../daemon/sensorPlugins/sampleTime.h"

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

// the CLOCK_BOOTTIME time stamp of a wall clock time, with the offset readingWallMs() keeps (sampleTime.h), so
// that the plugin reads back exactly that wall clock time
quint64 bootUsAt(qint64 wallMs)
{
    static const qint64 off = [] {
        struct timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        const qint64 b = (qint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
        return readingWallMs((quint64)b) * 1000 - b;
    }();
    return (quint64)(wallMs * 1000 - off);
}

// a whole minute some minutes ago
qint64 minuteAgo(int minutes)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return (now / 60000 - minutes) * 60000;
}

// the records of yesterday and today (a test near midnight crosses the day)
QStringList lines(const QString &prefix)
{
    const QDate today = QDate::currentDate();
    return testenv::dayLines(prefix, today.addDays(-1)) + testenv::dayLines(prefix, today);
}
QStringList values(const QString &prefix)
{
    QStringList v;
    for (const QString &l : lines(prefix))
        v << l.section(':', 1);
    return v;
}
QList<qint64> times(const QString &prefix)
{
    QList<qint64> t;
    for (const QString &l : lines(prefix))
        t << l.section(':', 0, 0).toLongLong();
    return t;
}
}

class TestBarometer : public QObject
{
    Q_OBJECT
    std::unique_ptr<BarometerSensorPlugin> plugin;

    BarometerSensorPlugin *make(int interval = 60000)
    {
        plugin.reset(new BarometerSensorPlugin(nullptr, interval));
        return plugin.get();
    }
    void push(qint64 wallMs, qreal pa) { backend()->pushPressure(pa, bootUsAt(wallMs)); }

private slots:
    void init()
    {
        testenv::clean();
        MGConfItem::clearTestValues();
        FakeSensorBackend::reset();
    }
    void cleanup() { plugin.reset(); }

    void startsAtOnceAndStaysOn()
    {
        make();
        QVERIFY(QDir(testenv::root() + "barometer-raw").exists());
        QVERIFY(QDir(testenv::root() + "barometer-compensated").exists());
        QVERIFY(plugin->barometerSensor->isAlwaysOn());
        QVERIFY(backend()->running());
        QCOMPARE(plugin->m_interval, 60000LL);
        QVERIFY(lines("barometer-raw").isEmpty());
        // display on: nothing to do while it runs
        plugin->timeUpdate();
        QCOMPARE(backend()->starts(), 1);
    }

    void aMinuteIsOneRecordTheMedianAtItsLastReading()
    {
        MGConfItem::setTestValue(offsetKey, 250);
        make();
        const qint64 m = minuteAgo(10);
        push(m + 1000, 101300);
        push(m + 2000, 101330);
        push(m + 3000, 101310);
        push(m + 59000, 101320);
        QVERIFY(lines("barometer-raw").isEmpty()); // the minute may still get readings
        push(m + 61000, 101200);                   // the next minute: the first one is over
        QCOMPARE(values("barometer-raw"), QStringList({"101320"}));
        QCOMPARE(values("barometer-compensated"), QStringList({"101070"}));
        QCOMPARE(times("barometer-raw"), QList<qint64>({(m + 59000) / 1000}));
        QCOMPARE(times("barometer-raw"), times("barometer-compensated"));
        QVERIFY(backend()->running()); // stays on
    }

    void aBatchOfManyMinutes()
    {
        make();
        const qint64 m = minuteAgo(20);
        // five minutes at 1 Hz, as the hub delivers them after the screen was off: pressure falling (a climb)
        for (int i = 0; i < 300; i++)
            push(m + i * 1000, 101325 - i);
        QCOMPARE(lines("barometer-raw").size(), 4);
        plugin.reset(); // SIGTERM: the open minute too
        const QStringList v = values("barometer-raw");
        QCOMPARE(v.size(), 5);
        // the median of 60 readings falling by 1 Pa a second: the 31st (index 30) of each minute
        QCOMPARE(v.first(), QString::number(101325 - 29));
        QCOMPARE(v.last(), QString::number(101325 - 240 - 29));
        const QList<qint64> t = times("barometer-raw");
        for (int k = 0; k < 5; k++)
            QCOMPARE(t[k], (m + k * 60000 + 59000) / 1000);
    }

    void unusableReplayedAndLateReadingsAreNotWritten()
    {
        make();
        const qint64 m = minuteAgo(8);
        push(m + 1000, 0); // no usable reading
        push(m + 5000, 100000);
        push(m + 5000, 100000); // the same again (sensorfw replays the last reading at a start)
        push(m + 2000, 90000);  // older than what was taken
        push(m + 65000, 100100);
        QCOMPARE(values("barometer-raw"), QStringList({"100000"}));
    }

    void triggerRecordingWritesTheOpenMinute()
    {
        make();
        const qint64 m = minuteAgo(5);
        push(m + 1000, 99000);
        plugin->triggerRecording();
        QCOMPARE(values("barometer-raw"), QStringList({"99000"}));
        plugin->triggerRecording(); // nothing new: nothing written
        QCOMPARE(values("barometer-raw").size(), 1);
    }

    void aRestartDoesNotWriteAgain()
    {
        const qint64 m = minuteAgo(6);
        const QDate d = QDateTime::fromMSecsSinceEpoch(m).date();
        testenv::writeDay("barometer-raw", d, QByteArray::number((m + 30000) / 1000) + ":100500\n");
        if (d != QDate::currentDate())
            QSKIP("the minute is yesterday's (near midnight)");
        make();
        push(m + 30000, 100500); // the replay of the last reading
        push(m + 70000, 100400);
        plugin->triggerRecording();
        QCOMPARE(values("barometer-raw"), QStringList({"100500", "100400"}));
    }

    void aSensorThatDoesNotStartIsTriedAgain()
    {
        FakeSensorBackend::failStarts(type, 1);
        make();
        QVERIFY(plugin->m_retryTimer->isActive());
        QCOMPARE(plugin->m_retryTimer->interval(), 30000);
        QVERIFY(QMetaObject::invokeMethod(plugin.get(), "retryStart"));
        QVERIFY(backend()->running());
        QCOMPARE(plugin->m_retryDelay, 30000);
    }
};

SENSORLOGD_TEST_MAIN(TestBarometer)
#include "tst_barometer.moc"
