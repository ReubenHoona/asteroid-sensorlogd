// StepCounterPlugin (daemon/sensorPlugins/stepCounter.cpp) against the fake QtSensors backend: no record before the
// first real reading, how the offset is settled on that reading (fresh boot or a restart within the boot, stored
// offset, today's day file), a counter that starts again from zero, zero readings, a value below the last record,
// the midnight and date-jump handling, the offset kept in the settings, the start retry with its backoff (the fix
// for the busy loop, static report rank 1) and the interval timer with its suspend compensation.
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <memory>

#include "../common.h"
#include "peek.h"
#include "testenv.h"

namespace {
const QByteArray type = QStepCounterSensor::sensorType;
const QString prefix = "stepCounter";
const QString offsetKey = "StepCounterPrivate/stepsOffset";

FakeSensorBackend *backend()
{
    FakeSensorBackend *b = FakeSensorBackend::last(type);
    if (!b)
        qFatal("no step counter backend");
    return b;
}
}

class TestStepCounter : public QObject
{
    Q_OBJECT
    std::unique_ptr<StepCounterPlugin> plugin;

    StepCounterPlugin *make(bool daemonFresh, int interval = 600000)
    {
        plugin.reset(new StepCounterPlugin(nullptr, interval, daemonFresh));
        return plugin.get();
    }

private slots:
    void init() { testenv::clean(); }
    void cleanup() { plugin.reset(); }

    void startsTheSensorAlwaysOn()
    {
        make(true);
        QVERIFY(QDir(testenv::root() + prefix).exists());
        QCOMPARE(FakeSensorBackend::created(type), 1);
        QVERIFY(backend()->running());
        QVERIFY(plugin->stepcounterSensor->isAlwaysOn());
        QVERIFY(plugin->stepcounterSensor->isActive());
        QVERIFY(!plugin->m_retryTimer->isActive());
        QVERIFY(plugin->recordIntervalTimer->isActive());
        QVERIFY(plugin->recordIntervalTimer->isSingleShot());
    }

    void noRecordBeforeTheFirstReading()
    {
        make(true);
        plugin->triggerRecording();
        QVERIFY(testenv::dayLines(prefix).isEmpty());
        // a reading without a timestamp is not a sample yet (sensorfw's initial value)
        backend()->pushSteps(1234, 0);
        QVERIFY(!plugin->m_offsetReady);
        plugin->triggerRecording();
        QVERIFY(testenv::dayLines(prefix).isEmpty());
        QVERIFY(plugin->recordIntervalTimer->isActive()); // the next recording is still due
    }

    void freshBootWithoutDayFile()
    {
        make(true);
        backend()->pushSteps(1000);
        QVERIFY(plugin->m_offsetReady);
        QCOMPARE(plugin->m_stepsOffset, 0);
        QCOMPARE(testenv::setting(offsetKey).toInt(), 0);
        plugin->triggerRecording();
        backend()->pushSteps(1500);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix), QStringList({"1000", "1500"}));
    }

    void freshBootDropsAStoredOffsetAboveTheCounter()
    {
        testenv::setSetting(offsetKey, 5000); // the previous boot's
        make(true);
        backend()->pushSteps(100);
        QCOMPARE(plugin->m_stepsOffset, 0);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix), QStringList({"100"}));
    }

    void freshBootKeepsAStoredOffsetBelowTheCounter()
    {
        // (current behaviour: with no day file today the stored offset is used when it is not above the counter)
        testenv::setSetting(offsetKey, 300);
        make(true);
        backend()->pushSteps(1000);
        QCOMPARE(plugin->m_stepsOffset, 300);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix), QStringList({"700"}));
    }

    void freshBootContinuesTodaysDayFile()
    {
        testenv::writeDay(prefix, QDate::currentDate(), "100:1200\n200:2000\n");
        testenv::setSetting(offsetKey, 9999);
        make(true);
        backend()->pushSteps(500);
        QCOMPARE(plugin->m_stepsOffset, 500 - 2000);
        QCOMPARE(testenv::setting(offsetKey).toInt(), -1500);
        backend()->pushSteps(600);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix), QStringList({"1200", "2000", "2100"}));
    }

    void freshBootAfterPowerLoss()
    {
        // power loss mid-write: today's file ends in a zero-filled tail, then the watch boots again
        testenv::writeDay(prefix, QDate::currentDate(), QByteArray("100:1200\n200:4800\n") + QByteArray(16, '\0'));
        make(true);
        backend()->pushSteps(300);
        QEXPECT_FAIL("", "BUG fileGetPrevRecord ignores the day's last valid record after a torn tail, so the day "
                         "counts from zero again (patches/sensorlogd/0001-fileGetPrevRecord-last-valid-record.patch)",
                     Continue);
        QCOMPARE(plugin->m_stepsOffset, 300 - 4800);
    }

    void restartKeepsTheStoredOffset()
    {
        testenv::setSetting(offsetKey, 300);
        testenv::writeDay(prefix, QDate::currentDate(), "100:650\n");
        make(false);
        QVERIFY(plugin->m_storedOffset);
        backend()->pushSteps(1000);
        QCOMPARE(plugin->m_stepsOffset, 300);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix), QStringList({"650", "700"}));
    }

    void restartWithStoredOffsetAboveTheCounterUsesTheDayFile()
    {
        testenv::setSetting(offsetKey, 5000);
        testenv::writeDay(prefix, QDate::currentDate(), "100:800\n");
        make(false);
        backend()->pushSteps(100);
        QCOMPARE(plugin->m_stepsOffset, 100 - 800);
        backend()->pushSteps(150);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix), QStringList({"800", "850"}));
    }

    void restartWithStoredOffsetAboveTheCounterNoDayFile()
    {
        testenv::setSetting(offsetKey, 5000);
        make(false);
        backend()->pushSteps(100);
        QCOMPARE(plugin->m_stepsOffset, 0);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix), QStringList({"100"}));
    }

    void counterStartingAgainCarriesOn()
    {
        make(true);
        backend()->pushSteps(1000);
        plugin->triggerRecording();
        backend()->pushSteps(1200);
        plugin->triggerRecording();
        // sensor hub reset / sensorfwd restart: the cumulative counter is back near zero
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^step counter started again at 50"));
        backend()->pushSteps(50);
        QCOMPARE(plugin->m_stepsOffset, -1200);
        QCOMPARE(testenv::setting(offsetKey).toInt(), -1200);
        plugin->triggerRecording();
        backend()->pushSteps(100);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix), QStringList({"1000", "1200", "1250", "1300"}));
    }

    void zeroIsNotARestart()
    {
        make(true);
        backend()->pushSteps(1000);
        plugin->triggerRecording();
        backend()->pushSteps(0);
        QCOMPARE(plugin->m_stepsOffset, 0);
        QCOMPARE(plugin->m_lastRawSteps, 1000);
        plugin->triggerRecording(); // 0 < 1000: not recorded
        QCOMPARE(testenv::dayValues(prefix), QStringList({"1000"}));
        backend()->pushSteps(1100);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix), QStringList({"1000", "1100"}));
    }

    void valueBelowTheLastRecordIsNotRecorded()
    {
        make(true);
        backend()->pushSteps(100);
        // something else logged more today (e.g. a previous daemon with another offset)
        testenv::writeDay(prefix, QDate::currentDate(), "100:900\n");
        const QDateTime before = plugin->lastRecordTime;
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix), QStringList({"900"}));
        QCOMPARE(plugin->lastRecordTime, before); // not taken as a recording
    }

    void midnightWritesYesterdaysTotal()
    {
        const QDate today = QDate::currentDate();
        const QDate yesterday = today.addDays(-1);
        make(true);
        backend()->pushSteps(1000);
        plugin->lastRecordTime = QDateTime(yesterday, QTime(23, 50));
        plugin->triggerRecording();
        QCOMPARE(testenv::dayLines(prefix, yesterday),
                 QStringList({QString::number(QDateTime(yesterday, QTime(23, 59, 59)).toSecsSinceEpoch()) + ":1000"}));
        QVERIFY(!QFile::exists(testenv::dayFile(prefix, today)));
        QCOMPARE(plugin->m_stepsOffset, 1000);
        QCOMPARE(testenv::setting(offsetKey).toInt(), 1000);
        QCOMPARE(plugin->lastRecordTime.date(), today);
        backend()->pushSteps(1300);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix, today), QStringList({"300"}));
    }

    void dateChangeWithTodaysFileContinuesIt()
    {
        // a system time change (no robust time at boot): today's file already has records
        const QDate today = QDate::currentDate();
        testenv::writeDay(prefix, today, "100:400\n");
        make(true);
        backend()->pushSteps(1000); // offset settled from today's file: 600
        plugin->m_stepsOffset = 0;
        plugin->lastRecordTime = QDateTime(today.addDays(-1), QTime(10, 0));
        plugin->triggerRecording();
        QCOMPARE(plugin->m_stepsOffset, 600);
        QCOMPARE(testenv::setting(offsetKey).toInt(), 600);
        QCOMPARE(testenv::dayValues(prefix, today), QStringList({"400"}));
        QVERIFY(!QFile::exists(testenv::dayFile(prefix, today.addDays(-1))));
        backend()->pushSteps(1100);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix, today), QStringList({"400", "500"}));
    }

    void dateJumpOfSeveralDaysWritesNothing()
    {
        const QDate today = QDate::currentDate();
        make(true);
        backend()->pushSteps(1000);
        plugin->lastRecordTime = QDateTime(today.addDays(-3), QTime(12, 0));
        plugin->triggerRecording();
        for (int d = 0; d <= 3; d++)
            QVERIFY(!QFile::exists(testenv::dayFile(prefix, today.addDays(-d))));
        QCOMPARE(plugin->m_stepsOffset, 0);
        QCOMPARE(plugin->lastRecordTime.date(), today);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix, today), QStringList({"1000"}));
    }

    void startFailureRetriesWithBackoff()
    {
        // sensorfw's backend before sensorfwd is up: start() fails; this used to spin a core in a busy loop
        FakeSensorBackend::failStarts(type, 1000);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^step counter did not start, retrying in 30 s"));
        make(true);
        QVERIFY(!plugin->stepcounterSensor->isActive());
        QVERIFY(plugin->m_retryTimer->isActive());
        QVERIFY(plugin->m_retryTimer->isSingleShot());
        QCOMPARE(plugin->m_retryTimer->interval(), 30000);
        const QList<int> expected = {60000, 120000, 240000, 480000, 960000, 1800000, 1800000};
        for (int delay : expected) {
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^step counter did not start"));
            QVERIFY(QMetaObject::invokeMethod(plugin.get(), "retryStart"));
            QCOMPARE(plugin->m_retryTimer->interval(), delay);
        }
        QCOMPARE(FakeSensorBackend::created(type), 1 + expected.size()); // a new sensor each time
        // sensorfwd is up: the sensor starts and the backoff starts over
        FakeSensorBackend::failStarts(type, 0);
        QVERIFY(QMetaObject::invokeMethod(plugin.get(), "retryStart"));
        QVERIFY(plugin->stepcounterSensor->isActive());
        QCOMPARE(plugin->m_retryDelay, 30000);
        // and readings reach the new sensor's slot
        backend()->pushSteps(42);
        QVERIFY(plugin->m_offsetReady);
        plugin->triggerRecording();
        QCOMPARE(testenv::dayValues(prefix), QStringList({"42"}));
    }

    void noBackendRetriesToo()
    {
        FakeSensorBackend::failCreates(type, 1);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^step counter did not start"));
        make(true);
        QVERIFY(plugin->m_retryTimer->isActive());
        plugin->m_retryTimer->stop(); // as if it had fired
        QVERIFY(QMetaObject::invokeMethod(plugin.get(), "retryStart"));
        QVERIFY(plugin->stepcounterSensor->isActive());
        QVERIFY(!plugin->m_retryTimer->isActive());
        QCOMPARE(plugin->m_retryDelay, 30000);
    }

    void retryTimerFires()
    {
        FakeSensorBackend::failStarts(type, 1);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^step counter did not start"));
        make(true);
        plugin->m_retryTimer->start(10); // instead of 30 s
        QTRY_VERIFY(plugin->stepcounterSensor->isActive());
    }

    void intervalTimerRecords()
    {
        make(true, 50);
        backend()->pushSteps(77);
        QTRY_VERIFY(!testenv::dayValues(prefix).isEmpty());
        QCOMPARE(testenv::dayValues(prefix).first(), QString("77"));
    }

    void timeUpdateRecordsWhenOverdue()
    {
        make(true);
        backend()->pushSteps(500);
        plugin->lastRecordTime = QDateTime::currentDateTime().addSecs(-3600); // an hour in suspend
        plugin->timeUpdate();
        QCOMPARE(testenv::dayValues(prefix), QStringList({"500"}));
        QVERIFY(plugin->recordIntervalTimer->remainingTime() > 590000);
    }

    void timeUpdateShortensTheTimer()
    {
        make(true);
        backend()->pushSteps(500);
        plugin->lastRecordTime = QDateTime::currentDateTime().addSecs(-100);
        plugin->timeUpdate();
        QVERIFY(testenv::dayLines(prefix).isEmpty());
        const int remaining = plugin->recordIntervalTimer->remainingTime();
        QVERIFY2(remaining <= 525000 && remaining > 475000, // a coarse timer: 5 % slack
                 qPrintable(QString::number(remaining)));
    }
};

SENSORLOGD_TEST_MAIN(TestStepCounter)
#include "tst_stepcounter.moc"
