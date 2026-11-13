// The Health monitor (daemon/healthMonitor.*) on recorded readings: the high and low heart rate alerts with stock's
// wording and the 12/24-hour time of the detail text, reminders to move with their celebrations and the state that
// survives a restart, the steps goal reached once a day, the exercise prompts, the nightly sleep estimate stored
// in sleep/<date>.json, and the plumbing from the step counter and heart rate plugins (stepsTaken with the time of
// each reading, heartReading, batchSettled) to the evaluation. The readings are synthetic. The battery rules are
// assertions too: the monitor has no periodic timer, its settle timer and the reminder timer are single-shot and
// armed only by data, display on or an enabled reminder setting.
#include <time.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include "peek.h"
#include "../common.h"
#include "../metrics/healthData.h"
#include "testenv.h"

using namespace Health;

namespace {

const QDate THU(2026, 11, 12);

qint64 ms(const QDate &d, int h, int m = 0, int s = 0) { return QDateTime(d, QTime(h, m, s)).toMSecsSinceEpoch(); }

quint64 bootUsAgo(qint64 ageMs)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (quint64)((qint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 - ageMs * 1000);
}

class FakePoster : public NotificationPoster
{
public:
    QList<HealthNotification> posted;
    uint next = 100;
    uint post(const HealthNotification &n) override
    {
        posted << n;
        return next++;
    }
};

QJsonObject healthJson(const HealthNotification &n)
{
    return QJsonDocument::fromJson(n.hints.value("x-asteroid-health").toString().toUtf8()).object();
}

const char *const TWELVE = "/org/asteroidos/settings/use-12h-format";
const char *const GOAL = "/org/asteroidos/ux/steps-goal";

}

class TestMonitor : public QObject
{
    Q_OBJECT

    FakePoster poster;
    QSettings *settings = nullptr;
    qint64 clock = 0;
    QScopedPointer<HealthMonitor> monitor;

    HealthMonitor *make(qint64 startAt)
    {
        monitor.reset();
        clock = startAt;
        monitor.reset(new HealthMonitor(settings, &poster));
        monitor->setClock([this] { return clock; });
        monitor->start();
        return monitor.data();
    }

    // a heart rate at every minute of [from, to), a reading in the middle of it, as a batch delivers them
    void heart(const QDate &d, int h, int m, int minutes, int bpm)
    {
        for (int i = 0; i < minutes; i++)
            monitor->ingestHeart(ms(d, h, m) + (qint64)i * 60000 + 30000, bpm);
    }
    void steps(const QDate &d, int h, int m, int minutes, int perMinute)
    {
        for (int i = 0; i < minutes; i++)
            monitor->ingestSteps(ms(d, h, m) + (qint64)i * 60000 + 20000, perMinute);
    }

private slots:
    void initTestCase() { settings = new QSettings("asteroid", "sensorlogd"); }
    void cleanupTestCase() { delete settings; }

    void init()
    {
        testenv::clean();
        settings->sync();
        settings->clear();
        settings->setValue("loggerRootPath", testenv::root());
        settings->sync();
        MGConfItem::clearTestValues();
        poster.posted.clear();
        poster.next = 100;
    }
    void cleanup() { monitor.reset(); }

    // ---- battery: nothing runs by itself ----
    void nothingIsArmedUntilDataOrTheReminderSetting()
    {
        HealthMonitor *m = make(ms(THU, 9, 0));
        QVERIFY(!m->m_settle->isActive());
        QVERIFY(!m->m_reminderTimer->isActive()); // reminders are off, as stock
        QVERIFY(m->m_settle->isSingleShot());
        QVERIFY(m->m_reminderTimer->isSingleShot());
        QCOMPARE(m->nextReminderMs(), qint64(-1));
        // evaluating with nothing to do arms nothing either
        m->evaluate();
        QVERIFY(!m->m_settle->isActive());
        QVERIFY(!m->m_reminderTimer->isActive());
        QVERIFY(poster.posted.isEmpty());
        // the children that are timers: only the two
        QCOMPARE(m->findChildren<QTimer *>().size(), 2);
    }

    void theReminderTimerFollowsTheSetting()
    {
        settings->setValue("reminders/enabled", true);
        HealthMonitor *m = make(ms(THU, 10, 30));
        QVERIFY(m->m_reminderTimer->isActive());
        QCOMPARE(m->nextReminderMs(), ms(THU, 10, 50));
        QCOMPARE(m->m_reminderTimer->interval(), 20 * 60 * 1000);
        settings->setValue("reminders/enabled", false);
        m->reloadSettings();
        QVERIFY(!m->m_reminderTimer->isActive());
        settings->setValue("reminders/enabled", true);
        settings->setValue("reminders/startHour", 12);
        m->reloadSettings();
        QCOMPARE(m->nextReminderMs(), ms(THU, 12, 50));
        QCOMPARE(m->m_reminderTimer->interval(), 140 * 60 * 1000);
    }

    // ---- heart rate alerts ----
    void aHighHeartRateInactiveForTenMinutesIsNotifiedInStocksWords()
    {
        MGConfItem::setTestValue(TWELVE, true);
        HealthMonitor *m = make(ms(THU, 13, 59));
        heart(THU, 14, 0, 12, 130);
        clock = ms(THU, 14, 12, 10);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        const HealthNotification &n = poster.posted[0];
        QCOMPARE(n.summary, QString("High heart rate"));
        QCOMPARE(n.body, QString("130 bpm"));
        QCOMPARE(n.hints.value("x-asteroid-detail").toString(),
                 QString("Your heart rate rose above 125 bpm while you appeared to be inactive from 2:00 PM - 2:10 PM."));
        QCOMPARE(n.hints.value("x-asteroid-category").toString(), QString("health-alert"));
        QCOMPARE(n.hints.value("x-nemo-preview-summary").toString(), n.summary);
        QCOMPARE(n.hints.value("x-nemo-preview-body").toString(), n.body);
        QCOMPARE(n.hints.value("x-nemo-feedback").toString(), QString("notif_strong"));
        QCOMPARE(n.icon, QString("ios-heart"));
        QCOMPARE(n.replaces, 0u);
        const QJsonObject j = healthJson(n);
        QCOMPARE(j["type"].toString(), QString("hr-high"));
        QCOMPARE(j["bpm"].toInt(), 130);
        QCOMPARE(j["threshold"].toInt(), 125);
        QCOMPARE((qint64)j["start"].toDouble(), ms(THU, 14, 0));
        QCOMPARE((qint64)j["end"].toDouble(), ms(THU, 14, 10));
        // looking again does not repeat it
        clock = ms(THU, 14, 20);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
    }

    void aLowHeartRateIsLowAndTheTimesFollowThe24HourSetting()
    {
        MGConfItem::setTestValue(TWELVE, false);
        HealthMonitor *m = make(ms(THU, 2, 59));
        heart(THU, 15, 0, 10, 36);
        clock = ms(THU, 15, 11);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        QCOMPARE(poster.posted[0].summary, QString("Low heart rate"));
        QCOMPARE(poster.posted[0].body, QString("36 bpm"));
        QCOMPARE(poster.posted[0].hints.value("x-asteroid-detail").toString(),
                 QString("Your heart rate fell below 40 bpm while you appeared to be inactive from 15:00 - 15:10."));
    }

    void aNewAlertReplacesTheOneOnScreen()
    {
        HealthMonitor *m = make(ms(THU, 13, 59));
        heart(THU, 14, 0, 10, 130);
        clock = ms(THU, 14, 11);
        m->evaluate();
        heart(THU, 14, 10, 2, 90);
        heart(THU, 14, 12, 10, 140);
        clock = ms(THU, 14, 25);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 2);
        QCOMPARE(poster.posted[0].replaces, 0u);
        QCOMPARE(poster.posted[1].replaces, 100u); // the id the first one was given: stock posts under one id
        QCOMPARE(poster.posted[1].body, QString("140 bpm"));
    }

    void thresholdsAndSwitchesComeFromTheSettings()
    {
        settings->setValue("hrAlerts/highBpm", 100);
        HealthMonitor *m = make(ms(THU, 13, 59));
        heart(THU, 14, 0, 10, 110);
        clock = ms(THU, 14, 11);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        QVERIFY(poster.posted[0].hints.value("x-asteroid-detail").toString().contains("rose above 100 bpm"));

        poster.posted.clear();
        settings->setValue("hrAlerts/highEnabled", false);
        settings->setValue("hrAlerts/lowBpm", 70);
        m->reloadSettings();
        heart(THU, 14, 12, 10, 110); // high, but switched off
        heart(THU, 14, 22, 10, 65);  // below 70
        clock = ms(THU, 14, 33);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        QCOMPARE(poster.posted[0].summary, QString("Low heart rate"));
        QVERIFY(poster.posted[0].hints.value("x-asteroid-detail").toString().contains("fell below 70 bpm"));

        poster.posted.clear();
        settings->setValue("hrAlerts/lowEnabled", false);
        m->reloadSettings();
        heart(THU, 14, 33, 10, 20);
        clock = ms(THU, 14, 45);
        m->evaluate();
        QVERIFY(poster.posted.isEmpty());
    }

    void defaultThresholdsFollowTheProfile()
    {
        MGConfItem::setTestValue("/org/asteroidos/health/profile/age", 60); // maximum 160
        MGConfItem::setTestValue("/org/asteroidos/health/profile/resting-hr", 70);
        HealthMonitor *m = make(ms(THU, 13, 59));
        heart(THU, 14, 0, 10, 116); // 70 + (160 - 70) / 2 = 115: above
        clock = ms(THU, 14, 11);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        QVERIFY(poster.posted[0].hints.value("x-asteroid-detail").toString().contains("rose above 115 bpm"));
    }

    void activeMinutesAreNoAlert()
    {
        HealthMonitor *m = make(ms(THU, 13, 59));
        heart(THU, 14, 0, 12, 130);
        steps(THU, 14, 4, 1, 40); // a walk of 40 steps in minute 4
        clock = ms(THU, 14, 12, 10);
        m->evaluate();
        QVERIFY(poster.posted.isEmpty()); // 4 minutes, a break, 7 minutes
    }

    void liveReadingsEvaluateAtEveryNewMinute()
    {
        HealthMonitor *m = make(ms(THU, 14, 0));
        QList<int> postedAfter;
        for (int i = 0; i < 12; i++) {
            clock = ms(THU, 14, i, 1);
            m->ingestHeart(clock - 500, 130); // a live reading (under 5 s old)
            clock += 20000;
            m->ingestHeart(clock - 500, 130); // the same minute: no second look
            postedAfter << poster.posted.size();
        }
        // the run of 10 minutes is complete when minute 10 begins (minute 9 is over)
        QCOMPARE(postedAfter, QList<int>({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1}));
        QVERIFY(!m->m_settle->isActive()); // live readings need no timer
    }

    // ---- reminders to move ----
    void aReminderToMoveAtMinute50()
    {
        settings->setValue("reminders/enabled", true);
        HealthMonitor *m = make(ms(THU, 10, 0));
        steps(THU, 10, 0, 10, 10);
        clock = ms(THU, 10, 49, 59);
        m->evaluate();
        QVERIFY(poster.posted.isEmpty());
        clock = ms(THU, 10, 50);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        const HealthNotification &n = poster.posted[0];
        QCOMPARE(n.summary, QString("Reminder to move"));
        QCOMPARE(n.body, QString("150 steps left\nthis hour"));
        QCOMPARE(n.hints.value("x-asteroid-category").toString(), QString("celebration"));
        QVERIFY(n.hints.value("transient").toBool());
        QCOMPARE(n.hints.value("x-nemo-feedback").toString(), QString("notif_strong")); // the vibration
        const QJsonObject j = healthJson(n);
        QCOMPARE(j["type"].toString(), QString("reminder-to-move"));
        QCOMPARE(j["steps"].toInt(), 100); // the ring: 100 of 250
        QCOMPARE(j["goal"].toInt(), 250);
        // the reminder timer goes on to the next hour
        QCOMPARE(m->nextReminderMs(), ms(THU, 11, 50));
        QCOMPARE(m->m_reminderTimer->interval(), 60 * 60 * 1000);
    }

    void aSingleStepLeftIsSingular()
    {
        settings->setValue("reminders/enabled", true);
        HealthMonitor *m = make(ms(THU, 10, 0));
        steps(THU, 10, 0, 1, 249);
        clock = ms(THU, 10, 50);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        QCOMPARE(poster.posted[0].body, QString("1 step left\nthis hour"));
    }

    void reachingTheGoalAfterAReminderCelebrates()
    {
        settings->setValue("reminders/enabled", true);
        HealthMonitor *m = make(ms(THU, 10, 0));
        steps(THU, 10, 0, 10, 10);
        clock = ms(THU, 10, 50);
        m->evaluate();
        steps(THU, 10, 50, 5, 30);
        clock = ms(THU, 10, 56);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 2);
        QCOMPARE(poster.posted[1].summary, QString("Reminder to move"));
        QCOMPARE(poster.posted[1].body, QString("Goal complete!"));
        QCOMPARE(healthJson(poster.posted[1])["type"].toString(), QString("reminder-to-move-complete"));
        QCOMPARE(healthJson(poster.posted[1])["steps"].toInt(), 250);
    }

    void everyHourMetIsTheHourlyActivityCelebration()
    {
        settings->setValue("reminders/enabled", true);
        HealthMonitor *m = make(ms(THU, 9, 0));
        for (int h = 9; h < 18; h++) {
            steps(THU, h, 0, 25, 10);
            clock = ms(THU, h, 30);
            m->evaluate();
        }
        QCOMPARE(poster.posted.size(), 1);
        QCOMPARE(poster.posted[0].summary, QString("Hourly Activity"));
        QCOMPARE(poster.posted[0].body, QString("9 hr"));
        QCOMPARE(healthJson(poster.posted[0])["hours"].toInt(), 9);
    }

    void aRestartDoesNotRepeatTheReminderOfTheHour()
    {
        settings->setValue("reminders/enabled", true);
        HealthMonitor *m = make(ms(THU, 10, 0));
        clock = ms(THU, 10, 50);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        settings->sync();
        m = make(ms(THU, 10, 52)); // sensorlogd restarted (resetup, an update)
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        clock = ms(THU, 11, 51); // the next hour does
        m->evaluate();
        QCOMPARE(poster.posted.size(), 2);
    }

    void noReminderWhenTheWatchIsNotWorn()
    {
        settings->setValue("reminders/enabled", true);
        HealthMonitor *m = make(ms(THU, 10, 0));
        m->m_hrAttached = true; // the heart rate sensor is there: its readings say whether the watch is on
        clock = ms(THU, 10, 50);
        m->evaluate();
        QVERIFY(poster.posted.isEmpty()); // no reading for 15 minutes: on a desk
        m->ingestHeart(ms(THU, 10, 48), 70);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
    }

    void noReminderWhenOffOrOutsideTheWindow()
    {
        HealthMonitor *m = make(ms(THU, 10, 0));
        clock = ms(THU, 10, 50);
        m->evaluate();
        QVERIFY(poster.posted.isEmpty()); // off by default
        settings->setValue("reminders/enabled", true);
        m->reloadSettings();
        clock = ms(THU, 8, 50);
        m->evaluate();
        clock = ms(THU, 18, 50);
        m->evaluate();
        QVERIFY(poster.posted.isEmpty());
    }

    // ---- the steps goal ----
    void theStepsGoalIsReachedOnceADay()
    {
        MGConfItem::setTestValue(GOAL, 500);
        HealthMonitor *m = make(ms(THU, 8, 0));
        QSignalSpy reached(m, &HealthMonitor::goalReached);
        steps(THU, 8, 0, 4, 100);
        clock = ms(THU, 8, 5);
        m->evaluate();
        QCOMPARE(reached.count(), 0);
        steps(THU, 8, 5, 3, 100);
        clock = ms(THU, 8, 9);
        m->evaluate();
        QCOMPARE(reached.count(), 1);
        QCOMPARE(reached[0][0].toString(), QString("steps"));
        QCOMPARE(reached[0][1].toInt(), 700);
        QCOMPARE(reached[0][2].toInt(), 500);
        clock = ms(THU, 9, 0);
        m->evaluate();
        QCOMPARE(reached.count(), 1); // not again today
        // a restart the same day: still no second
        HealthMonitor *again = make(ms(THU, 9, 30));
        QSignalSpy second(again, &HealthMonitor::goalReached);
        again->evaluate();
        QCOMPARE(second.count(), 0);
        // tomorrow it starts again: today's steps only
        clock = ms(THU.addDays(1), 7, 0);
        again->evaluate();
        QCOMPARE(second.count(), 0);
        steps(THU.addDays(1), 7, 0, 6, 100);
        clock = ms(THU.addDays(1), 7, 10);
        again->evaluate();
        QCOMPARE(second.count(), 1);
        QCOMPARE(second[0][1].toInt(), 600);
    }

    void theGoalIsTheOneDconfKey()
    {
        // no key: 10000 as stock's tile; a goal of 10000 is not met by 9999
        HealthMonitor *m = make(ms(THU, 8, 0));
        QSignalSpy reached(m, &HealthMonitor::goalReached);
        steps(THU, 8, 0, 1, 9999);
        clock = ms(THU, 8, 5);
        m->evaluate();
        QCOMPARE(reached.count(), 0);
        steps(THU, 8, 1, 1, 1);
        m->evaluate();
        QCOMPARE(reached.count(), 1);
        QCOMPARE(reached[0][2].toInt(), 10000);
    }

    // ---- auto-detected exercise ----
    void aWalkOffersAndTheEndOfAWorkoutAsks()
    {
        HealthMonitor *m = make(ms(THU, 10, 0));
        QSignalSpy offered(m, &HealthMonitor::exerciseOffered);
        steps(THU, 10, 0, 10, 100);
        clock = ms(THU, 10, 10, 10);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        QCOMPARE(poster.posted[0].summary, QString("Activity detected"));
        QCOMPARE(poster.posted[0].body, QString("Start tracking?"));
        QCOMPARE(poster.posted[0].hints.value("x-asteroid-category").toString(), QString("exercise-detect"));
        QVERIFY(poster.posted[0].hints.value("transient").toBool());
        QCOMPARE(offered.count(), 1);
        QCOMPARE(offered[0][0].toString(), QString("start"));
        QCOMPARE(offered[0][1].toString(), QString("walking"));
        QCOMPARE(offered[0][2].toLongLong(), ms(THU, 10, 0));
        const QJsonObject j = QJsonDocument::fromJson(poster.posted[0].hints.value("x-asteroid-exercise").toString().toUtf8()).object();
        QCOMPARE(j["action"].toString(), QString("start"));

        // the Exercise app starts the workout; the walk goes on, then stops
        m->setWorkoutActive(true);
        steps(THU, 10, 10, 10, 100);
        clock = ms(THU, 10, 20);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        clock = ms(THU, 10, 26);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 2);
        QCOMPARE(poster.posted[1].summary, QString("Finished working out?"));
        QCOMPARE(offered.count(), 2);
        QCOMPARE(offered[1][0].toString(), QString("stop"));
    }

    void exercisePromptsFollowTheirSettings()
    {
        settings->setValue("autoExercise/start", false);
        HealthMonitor *m = make(ms(THU, 10, 0));
        steps(THU, 10, 0, 12, 100);
        clock = ms(THU, 10, 13);
        m->evaluate();
        QVERIFY(poster.posted.isEmpty());
    }

    // ---- the plugins' readings ----
    void theStepPluginTellsTheMonitorEachReadingsSteps()
    {
        HealthMonitor *m = make(QDateTime::currentMSecsSinceEpoch());
        m->setClock({});
        QScopedPointer<StepCounterPlugin> plugin(new StepCounterPlugin(nullptr, 600000, true));
        QSignalSpy taken(plugin.data(), &StepCounterPlugin::stepsTaken);
        m->attach(plugin.data(), nullptr);
        FakeSensorBackend *b = FakeSensorBackend::last(QStepCounterSensor::sensorType);
        QVERIFY(b);
        b->pushSteps(1000, bootUsAgo(200000)); // the first reading of a start: no steps from it
        QCOMPARE(taken.count(), 0);
        b->pushSteps(1050, bootUsAgo(150000));
        QCOMPARE(taken.count(), 1);
        QCOMPARE(taken[0][1].toInt(), 50);
        const qint64 t1 = taken[0][0].toLongLong();
        QVERIFY(qAbs(t1 - (QDateTime::currentMSecsSinceEpoch() - 150000)) < 2000);
        b->pushSteps(1080, bootUsAgo(30000));
        QCOMPARE(taken.count(), 2);
        QCOMPARE(taken[1][1].toInt(), 30);
        // a reading older than what was counted (a replay): changes nothing
        b->pushSteps(1090, bootUsAgo(170000));
        QCOMPARE(taken.count(), 2);
        // the counter started again: the steps since it did
        b->pushSteps(20, bootUsAgo(5000));
        QCOMPARE(taken.count(), 3);
        QCOMPARE(taken[2][1].toInt(), 20);
        // the monitor has them by the minute they were taken in
        QCOMPARE(m->series().stepsBetween(t1, t1), 50);
        QCOMPARE(m->series().stepsBetween(QDateTime::currentMSecsSinceEpoch() - 230000, QDateTime::currentMSecsSinceEpoch()), 100);
    }

    void theHeartPluginTellsTheMonitorEachUsableReading()
    {
        HealthMonitor *m = make(QDateTime::currentMSecsSinceEpoch());
        m->setClock({});
        QSettings plain("asteroid", "sensorlogd");
        QScopedPointer<HeartrateSensorPlugin> plugin(new HeartrateSensorPlugin(nullptr, &plain, 600000));
        QSignalSpy readings(plugin.data(), &HeartrateSensorPlugin::heartReading);
        m->attach(nullptr, plugin.data());
        QVERIFY(m->m_hrAttached);
        FakeSensorBackend *b = FakeSensorBackend::last(QHrmSensor::sensorType);
        QVERIFY(b);
        b->pushHeartRate(72, QHrmSensor::AccuracyHigh, bootUsAgo(60000));
        b->pushHeartRate(0, QHrmSensor::AccuracyHigh, bootUsAgo(50000));   // no rate
        b->pushHeartRate(75, QHrmSensor::AccuracyLow, bootUsAgo(40000));   // below the accuracy logged
        b->pushHeartRate(78, QHrmSensor::AccuracyMedium, bootUsAgo(30000));
        QCOMPARE(readings.count(), 2);
        QCOMPARE(readings[0][1].toInt(), 72);
        QCOMPARE(readings[1][1].toInt(), 78);
        QVERIFY(m->m_lastHrMs > QDateTime::currentMSecsSinceEpoch() - 40000);
    }

    void aBatchBeingOverMakesTheMonitorLookWithinHalfASecond()
    {
        MGConfItem::setTestValue(GOAL, 100);
        HealthMonitor *m = make(QDateTime::currentMSecsSinceEpoch());
        m->setClock({});
        QScopedPointer<StepCounterPlugin> plugin(new StepCounterPlugin(nullptr, 600000, true));
        QSignalSpy reached(m, &HealthMonitor::goalReached);
        QSignalSpy settled(plugin.data(), &StepCounterPlugin::batchSettled);
        m->attach(plugin.data(), nullptr);
        FakeSensorBackend *b = FakeSensorBackend::last(QStepCounterSensor::sensorType);
        b->pushSteps(1000, bootUsAgo(300000));
        b->pushSteps(1150, bootUsAgo(200000)); // 150 steps in a batch
        QVERIFY(!m->m_settle->isActive());       // nothing looked at while the batch comes
        QCOMPARE(reached.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(settled.count(), 1, 5000); // the plugin's own 2 s idle time
        QVERIFY(m->m_settle->isActive());
        QCOMPARE(m->m_settle->interval(), int(HealthMonitor::SETTLE_MS));
        QTRY_COMPARE_WITH_TIMEOUT(reached.count(), 1, 3000);
        QCOMPARE(reached[0][1].toInt(), 150);
    }

    void displayOnWaitsForTheBatchThenLooksAnyway()
    {
        HealthMonitor *m = make(ms(THU, 9, 0));
        QVERIFY(!m->m_settle->isActive());
        m->displayOn();
        QVERIFY(m->m_settle->isActive());
        QCOMPARE(m->m_settle->interval(), int(HealthMonitor::DISPLAY_SETTLE_MS));
        m->batchSettled(); // the hub's batch is in: look sooner
        QCOMPARE(m->m_settle->interval(), int(HealthMonitor::SETTLE_MS));
    }

    void todaysStepsAreReadBackAtAStart()
    {
        settings->setValue("reminders/enabled", true);
        // the day file: 100 steps by 10:09, 160 by 10:19 (the daemon's records)
        testenv::writeDay("stepCounter", THU, QByteArray::number(ms(THU, 10, 9) / 1000) + ":100\n"
                                                + QByteArray::number(ms(THU, 10, 19) / 1000) + ":160\n");
        HealthMonitor *m = make(ms(THU, 10, 30));
        QCOMPARE(m->series().stepsBetween(ms(THU, 10), ms(THU, 11) - 1), 160);
        clock = ms(THU, 10, 50);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        QCOMPARE(poster.posted[0].body, QString("90 steps left\nthis hour"));
    }

    // ---- sleep ----
    void lastNightIsStoredOnceTheWatchHasBeenStill()
    {
        const QDate eve = THU.addDays(-1);
        // the night in the heart rate database: 22:30 to 06:30 at 55-60
        HeartrateDb db(heartrateDbPath());
        QVector<HeartrateDb::Reading> rows;
        for (int i = 0; i < 480; i++)
            rows.append({ms(eve, 22, 30) + (qint64)i * 60000 + 15000, 55 + (i % 6), 4});
        QVERIFY(db.add(rows));

        // 06:45: the night ended 15 minutes ago: not yet
        HealthMonitor *m = make(ms(THU, 6, 45));
        m->evaluate();
        QVERIFY(!QFile::exists(sleepFilePath(THU)));
        // 07:00: not looked at again within 30 minutes
        clock = ms(THU, 7, 0);
        m->evaluate();
        QVERIFY(!QFile::exists(sleepFilePath(THU)));
        // 07:20: stored
        clock = ms(THU, 7, 20);
        m->evaluate();
        QVERIFY(QFile::exists(sleepFilePath(THU)));
        QCOMPARE(testenv::setting("HealthPrivate/sleepStored").toString(), QString("2026-11-12"));
        bool stored = false;
        const SleepNight n = loadSleep(THU, HealthProfile(), 60, &stored);
        QVERIFY(stored);
        QVERIFY(n.valid);
        QCOMPARE(n.main.startMs, ms(eve, 22, 30));
        QCOMPARE(n.main.asleepMin, 480);
        QCOMPARE(n.wakeDate, THU);
        // once stored it is not looked at again
        QFile f(sleepFilePath(THU));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{\"valid\":false}");
        f.close();
        clock = ms(THU, 9, 0);
        m->evaluate();
        QFile again(sleepFilePath(THU));
        QVERIFY(again.open(QIODevice::ReadOnly));
        QCOMPARE(again.readAll(), QByteArray("{\"valid\":false}"));
    }

    void noSleepIsEstimatedAtNightOrWithoutAHeartRate()
    {
        HealthMonitor *m = make(ms(THU, 3, 0));
        m->evaluate(); // 03:00: the night is not over
        QVERIFY(m->m_sleepAttemptMs == 0);
        clock = ms(THU, 8, 0);
        m->evaluate(); // no data: nothing to store, tried again later
        QVERIFY(!QFile::exists(sleepFilePath(THU)));
        QVERIFY(m->m_sleepAttemptMs != 0);
        clock = ms(THU, 21, 0);
        m->m_sleepAttemptMs = 0;
        m->evaluate(); // evening: not looked at
        QVERIFY(m->m_sleepAttemptMs == 0);
    }
};

SENSORLOGD_TEST_MAIN(TestMonitor)
#include "tst_monitor.moc"
