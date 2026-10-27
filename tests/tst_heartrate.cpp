// HeartrateSensorPlugin (daemon/sensorPlugins/heartrateSensor.cpp) against the fake QtSensors backend.
// Interval mode: a recording starts the sensor, readings with bpm 0 or a status below AccuracyMedium are not
// logged, the first good one is and stops the sensor, a recording without a good reading is given up after the
// timeout, a sensor that does not start is made again, the interval timer and its suspend compensation.
// Continuous mode: the sensor stays on, good samples are averaged per minute of the time they were taken (the
// reading's CLOCK_MONOTONIC timestamp, so batched samples are dated right), a minute is written when the first
// sample of a later minute arrives, and a sensor that does not start is retried with a backoff.
#include <QRegularExpression>
#include <QTest>
#include <memory>
#include <time.h>

#include "../common.h"
#include "peek.h"
#include "testenv.h"

namespace {
const QByteArray type = QHrmSensor::sensorType;
const QString prefix = "heartrateMonitor";
enum { NoContact, Unreliable, AccuracyLow, AccuracyMedium, AccuracyHigh };

FakeSensorBackend *backend()
{
    FakeSensorBackend *b = FakeSensorBackend::last(type);
    if (!b)
        qFatal("no heart rate backend");
    return b;
}

// the reading timestamp (CLOCK_MONOTONIC, microseconds) of a sample taken at wall-clock time TAKENMS
quint64 timestampFor(qint64 takenMs)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const qint64 monoUs = (qint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    const qint64 wallMs = QDateTime::currentMSecsSinceEpoch();
    return monoUs - (wallMs - takenMs) * 1000;
}

quint64 monotonicUs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (quint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

QString record(qint64 ms, int bpm) { return QString::number(ms / 1000) + ":" + QString::number(bpm); }
}

class TestHeartrate : public QObject
{
    Q_OBJECT
    std::unique_ptr<HeartrateSensorPlugin> plugin;

    HeartrateSensorPlugin *make(bool continuous, int interval = 600000)
    {
        plugin.reset(new HeartrateSensorPlugin(nullptr, interval, continuous));
        return plugin.get();
    }

private slots:
    void init() { testenv::clean(); }
    void cleanup() { plugin.reset(); }

    // ---- interval mode

    void intervalModeWaitsForTheTimer()
    {
        make(false);
        QVERIFY(QDir(testenv::root() + prefix).exists());
        QVERIFY(plugin->recordIntervalTimer->isActive());
        QVERIFY(!plugin->recordTimeoutTimer->isActive());
        QCOMPARE(plugin->recordTimeoutTimer->interval(), 60000);
        QVERIFY(plugin->hrmSensor->isAlwaysOn());
        QVERIFY(!plugin->hrmSensor->isActive());
        QCOMPARE(FakeSensorBackend::created(type), 0); // not started yet
    }

    void recordingLogsTheFirstGoodReading()
    {
        make(false);
        plugin->triggerRecording();
        QVERIFY(backend()->running());
        QVERIFY(plugin->recordTimeoutTimer->isActive());
        backend()->pushHeartRate(70, AccuracyLow);
        backend()->pushHeartRate(0, AccuracyHigh);
        backend()->pushHeartRate(65, Unreliable);
        QVERIFY(testenv::dayLines(prefix).isEmpty());
        QVERIFY(backend()->running());
        QCOMPARE(plugin->m_readings, 3);
        QCOMPARE(plugin->m_bestStatus, (int)AccuracyHigh);
        backend()->pushHeartRate(72, AccuracyMedium);
        QCOMPARE(testenv::dayValues(prefix), QStringList({"72"}));
        QVERIFY(!backend()->running());
        QCOMPARE(backend()->stops(), 1);
        QVERIFY(!plugin->recordTimeoutTimer->isActive());
    }

    void eachRecordingStartsAfresh()
    {
        make(false);
        plugin->triggerRecording();
        backend()->pushHeartRate(80, AccuracyHigh);
        plugin->triggerRecording();
        QCOMPARE(plugin->m_readings, 0);
        QCOMPARE(plugin->m_bestStatus, -1);
        QVERIFY(backend()->running());
        backend()->pushHeartRate(81, AccuracyHigh);
        QCOMPARE(testenv::dayValues(prefix), QStringList({"80", "81"}));
        QCOMPARE(backend()->starts(), 2);
    }

    void recordingWithoutAGoodReadingIsGivenUp()
    {
        make(false);
        plugin->triggerRecording();
        backend()->pushHeartRate(0, NoContact);
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression("^no usable heartrate reading in 60 s: 1 readings, best status 0"));
        QVERIFY(QMetaObject::invokeMethod(plugin.get(), "abortRecording"));
        QVERIFY(!backend()->running());
        QVERIFY(testenv::dayLines(prefix).isEmpty());
    }

    void timeoutTimerGivesUp()
    {
        make(false);
        plugin->recordTimeoutTimer->setInterval(20);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^no usable heartrate reading"));
        plugin->triggerRecording();
        QTRY_VERIFY(!backend()->running());
    }

    void sensorThatDoesNotStartIsMadeAgain()
    {
        make(false);
        FakeSensorBackend::failStarts(type, 1);
        QTest::ignoreMessage(QtWarningMsg, "heart rate sensor did not start, making it again");
        plugin->triggerRecording();
        QCOMPARE(FakeSensorBackend::created(type), 2);
        QVERIFY(plugin->hrmSensor->isActive());
        QVERIFY(plugin->hrmSensor->isAlwaysOn());
        QVERIFY(plugin->recordTimeoutTimer->isActive());
        backend()->pushHeartRate(66, AccuracyHigh); // the new sensor is connected
        QCOMPARE(testenv::dayValues(prefix), QStringList({"66"}));
    }

    void sensorThatStillDoesNotStartWaitsForTheNextRecording()
    {
        make(false);
        FakeSensorBackend::failStarts(type, 2);
        QTest::ignoreMessage(QtWarningMsg, "heart rate sensor did not start, making it again");
        QTest::ignoreMessage(QtWarningMsg, "heart rate sensor still does not start");
        plugin->triggerRecording();
        QVERIFY(!plugin->hrmSensor->isActive());
        QVERIFY(!plugin->recordTimeoutTimer->isActive());
        QVERIFY(plugin->recordIntervalTimer->isActive());
        plugin->triggerRecording();
        QVERIFY(plugin->hrmSensor->isActive());
        QVERIFY(plugin->recordTimeoutTimer->isActive());
    }

    void noBackendIsLikeAFailedStart()
    {
        make(false);
        FakeSensorBackend::failCreates(type, 1);
        QTest::ignoreMessage(QtWarningMsg, "heart rate sensor did not start, making it again");
        plugin->triggerRecording();
        QVERIFY(plugin->hrmSensor->isActive());
    }

    void intervalTimerStartsRecordings()
    {
        make(false, 30);
        QTRY_VERIFY(FakeSensorBackend::last(type) && backend()->starts() >= 1);
    }

    void timeUpdateRecordsWhenOverdue()
    {
        make(false);
        plugin->lastRecordTime = QDateTime::currentDateTime().addSecs(-3600);
        plugin->timeUpdate();
        QVERIFY(backend()->running());
        QVERIFY(plugin->recordIntervalTimer->remainingTime() > 590000);
    }

    void timeUpdateShortensTheTimer()
    {
        make(false);
        plugin->lastRecordTime = QDateTime::currentDateTime().addSecs(-200);
        plugin->timeUpdate();
        QCOMPARE(FakeSensorBackend::created(type), 0);
        const int remaining = plugin->recordIntervalTimer->remainingTime();
        QVERIFY2(remaining <= 420000 && remaining > 380000, // a coarse timer: 5 % slack
                 qPrintable(QString::number(remaining)));
    }

    // ---- continuous mode

    void continuousModeKeepsTheSensorOn()
    {
        make(true);
        QVERIFY(!plugin->recordIntervalTimer->isActive());
        QVERIFY(backend()->running());
        QVERIFY(!plugin->m_retryTimer->isActive());
        plugin->triggerRecording(); // no interval recordings in continuous mode
        plugin->timeUpdate();
        QCOMPARE(backend()->starts(), 1);
        QVERIFY(!plugin->recordIntervalTimer->isActive());
        QVERIFY(!plugin->recordTimeoutTimer->isActive());
    }

    void continuousAveragesEachMinute()
    {
        make(true);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 m = now / 60000 * 60000; // this minute
        const qint64 b1 = m - 5 * 60000, b2 = m - 4 * 60000, b3 = m - 2 * 60000;
        backend()->pushHeartRate(60, AccuracyMedium, timestampFor(b1 + 10000));
        backend()->pushHeartRate(71, AccuracyHigh, timestampFor(b1 + 30000));
        backend()->pushHeartRate(0, AccuracyHigh, timestampFor(b1 + 40000));    // no reading
        backend()->pushHeartRate(200, AccuracyLow, timestampFor(b1 + 45000));  // not accurate enough
        QVERIFY(testenv::dayLines(prefix).isEmpty()); // the minute is still open
        backend()->pushHeartRate(80, AccuracyHigh, timestampFor(b2 + 10000));
        backend()->pushHeartRate(81, AccuracyHigh, timestampFor(b2 + 20000));
        backend()->pushHeartRate(85, AccuracyHigh, timestampFor(b2 + 30000));
        backend()->pushHeartRate(90, AccuracyHigh, timestampFor(b3 + 5000));
        // one record per minute at the minute's start: 65.5 rounds to 66, (80 + 81 + 85) / 3 = 82
        const QDate day = QDateTime::fromMSecsSinceEpoch(b1).date();
        const QStringList expected = {record(b1, 66), record(b2, 82)};
        if (QDateTime::fromMSecsSinceEpoch(b2).date() == day)
            QCOMPARE(testenv::dayLines(prefix, day), expected);
        QVERIFY(backend()->running()); // never stopped in continuous mode
        QCOMPARE(plugin->minuteStart, b3);
        QCOMPARE(plugin->minuteCount, 1);
        QCOMPARE(plugin->minuteSum, 90);
    }

    void continuousDatesAMinuteByItsOwnDay()
    {
        // a minute is written into the file of the day it was taken (batched samples from before midnight)
        make(true);
        const qint64 m = QDateTime::currentMSecsSinceEpoch() / 60000 * 60000;
        const qint64 b1 = m - 10 * 60000;
        backend()->pushHeartRate(70, AccuracyHigh, timestampFor(b1 + 1000));
        backend()->pushHeartRate(75, AccuracyHigh, timestampFor(m - 60000 + 1000));
        const QDate day = QDateTime::fromMSecsSinceEpoch(b1).date();
        QCOMPARE(testenv::dayLines(prefix, day), QStringList({record(b1, 70)}));
    }

    void continuousUnknownTimeBaseMeansNow()
    {
        make(true);
        const qint64 m = QDateTime::currentMSecsSinceEpoch() / 60000 * 60000;
        const qint64 b1 = m - 3 * 60000;
        backend()->pushHeartRate(70, AccuracyHigh, timestampFor(b1 + 1000));
        // a timestamp in the future (not CLOCK_MONOTONIC): dated now
        backend()->pushHeartRate(90, AccuracyHigh, monotonicUs() + 3600ULL * 1000000);
        QVERIFY(plugin->minuteStart >= m);
        QCOMPARE(plugin->minuteSum, 90);
        // older than two hours: also dated now
        backend()->pushHeartRate(92, AccuracyHigh, timestampFor(QDateTime::currentMSecsSinceEpoch() - 3 * 3600 * 1000));
        QVERIFY(plugin->minuteStart >= m);
        QCOMPARE(testenv::dayLines(prefix, QDateTime::fromMSecsSinceEpoch(b1).date()), QStringList({record(b1, 70)}));
    }

    void continuousStartFailureRetriesWithBackoff()
    {
        FakeSensorBackend::failStarts(type, 1000);
        QTest::ignoreMessage(QtWarningMsg, "continuous heart rate: retrying in 30 s");
        make(true);
        QVERIFY(!plugin->hrmSensor->isActive());
        QVERIFY(plugin->m_retryTimer->isActive());
        QCOMPARE(plugin->m_retryTimer->interval(), 30000);
        for (int delay : {60000, 120000, 240000, 480000, 960000, 1800000, 1800000}) {
            QVERIFY(QMetaObject::invokeMethod(plugin.get(), "retryContinuous"));
            QCOMPARE(plugin->m_retryTimer->interval(), delay);
        }
        FakeSensorBackend::failStarts(type, 0);
        QVERIFY(QMetaObject::invokeMethod(plugin.get(), "retryContinuous"));
        QVERIFY(plugin->hrmSensor->isActive());
        QCOMPARE(plugin->m_retryDelay, 30000);
        const qint64 m = QDateTime::currentMSecsSinceEpoch() / 60000 * 60000;
        backend()->pushHeartRate(70, AccuracyHigh, timestampFor(m - 3 * 60000 + 1000));
        QCOMPARE(plugin->minuteCount, 1); // the new sensor's readings arrive
    }

    void continuousRetryTimerFires()
    {
        FakeSensorBackend::failStarts(type, 2); // both tries of the first start
        make(true);
        QVERIFY(plugin->m_retryTimer->isActive());
        plugin->m_retryTimer->start(10);
        QTRY_VERIFY(plugin->hrmSensor->isActive());
    }
};

SENSORLOGD_TEST_MAIN(TestHeartrate)
#include "tst_heartrate.moc"
