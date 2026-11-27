// The Health monitor (daemon/healthMonitor.*) on recorded readings: the high and low heart rate alerts with stock's
// wording and the 12/24-hour time of the detail text, reminders to move with their celebrations and the state that
// survives a restart, the steps goal reached once a day, the exercise prompts, the nightly sleep estimate stored
// in health.db, the days closed there on the first delivery after midnight, the one-time import run from start(),
// the Exercise app's new workout taken in when it ends, and the plumbing from the step counter and heart rate
// plugins (stepsTaken with the time of each reading, heartReading, batchSettled) to the evaluation. The readings
// are synthetic. The battery rules are assertions too: the monitor has no periodic timer, its settle timer and the
// reminder timer are single-shot and armed only by data, display on or an enabled reminder setting; the import
// goes through the event loop without a timer.
#include <time.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <keepalive/backgroundactivity.h>

#include "peek.h"
#include "../common.h"
#include "../daemon/keepaliveWake.h"
#include "../metrics/healthData.h"
#include "../metrics/restingHr.h"
#include "../store/healthDb.h"
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

// the monitor's wake (R5 E-04) as the tests drive it: what was asked of it; the test makes the time come
class FakeWake : public HealthWake
{
public:
    QList<QPair<qint64, int>> schedules;
    int releases = 0;
    int flushes = 0;
    void schedule(qint64 atMs, int slackMs) override { schedules << qMakePair(atMs, slackMs); }
    void release() override { releases++; }
    void flushHub() override { flushes++; }
    qint64 at() const { return schedules.isEmpty() ? -2 : schedules.last().first; }
    int slack() const { return schedules.isEmpty() ? -2 : schedules.last().second; }
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

    FakeWake wake;
    HealthMonitor *makeWithWake(qint64 startAt)
    {
        monitor.reset();
        wake.schedules.clear();
        wake.releases = 0;
        wake.flushes = 0;
        clock = startAt;
        monitor.reset(new HealthMonitor(settings, &poster));
        monitor->setClock([this] { return clock; });
        monitor->setWake(&wake);
        monitor->m_hrAttached = true; // as attach() with the daemon's heart rate plugin: worn() from its readings
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
                 QString("Your heart rate rose above 120 bpm while you appeared to be inactive from 2:00 PM - 2:10 PM."));
        QCOMPARE(n.hints.value("x-asteroid-category").toString(), QString("health-alert"));
        QCOMPARE(n.hints.value("x-nemo-preview-summary").toString(), n.summary);
        QCOMPARE(n.hints.value("x-nemo-preview-body").toString(), n.body);
        QCOMPARE(n.hints.value("x-nemo-feedback").toString(), QString("notif_strong"));
        QCOMPARE(n.icon, QString("ios-heart"));
        QCOMPARE(n.replaces, 0u);
        const QJsonObject j = healthJson(n);
        QCOMPARE(j["type"].toString(), QString("hr-high"));
        QCOMPARE(j["bpm"].toInt(), 130);
        QCOMPARE(j["threshold"].toInt(), 120);
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
                 QString("Your heart rate fell below 50 bpm while you appeared to be inactive from 15:00 - 15:10."));
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

    void theDefaultThresholdsAreStocksWhateverTheProfile()
    {
        // stock's client-side defaults, 120 and 50 (D/abgd.java:431,449), not from age or resting rate
        MGConfItem::setTestValue("/org/asteroidos/health/profile/age", 60);
        MGConfItem::setTestValue("/org/asteroidos/health/profile/resting-hr", 70);
        HealthMonitor *m = make(ms(THU, 13, 59));
        heart(THU, 14, 0, 10, 119); // not above 120
        clock = ms(THU, 14, 11);
        m->evaluate();
        QVERIFY(poster.posted.isEmpty());
        heart(THU, 14, 11, 10, 121);
        clock = ms(THU, 14, 22);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        QVERIFY(poster.posted[0].hints.value("x-asteroid-detail").toString().contains("rose above 120 bpm"));
        heart(THU, 15, 0, 10, 51); // not below 50
        heart(THU, 15, 10, 10, 49);
        clock = ms(THU, 15, 21);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 2);
        QCOMPARE(poster.posted[1].summary, QString("Low heart rate"));
        QVERIFY(poster.posted[1].hints.value("x-asteroid-detail").toString().contains("fell below 50 bpm"));
    }

    void theUsersOwnThresholdsStayInForce()
    {
        // saved before (Health settings): kept, the defaults only fill in what is not set
        settings->setValue("hrAlerts/highBpm", 130);
        HealthMonitor *m = make(ms(THU, 13, 59));
        heart(THU, 14, 0, 10, 125);
        clock = ms(THU, 14, 11);
        m->evaluate();
        QVERIFY(poster.posted.isEmpty());
        heart(THU, 15, 0, 10, 48); // the low one is not set: 50
        clock = ms(THU, 15, 11);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        QVERIFY(poster.posted[0].hints.value("x-asteroid-detail").toString().contains("fell below 50 bpm"));
        QCOMPARE(testenv::setting("hrAlerts/highBpm").toInt(), 130);
    }

    void noAlertForTheTimeOnTheCharger()
    {
        // on the charger the hub pauses the heart rate: no reading, so no minute of a run
        HealthMonitor *m = make(ms(THU, 13, 59));
        heart(THU, 14, 0, 6, 135);
        heart(THU, 14, 26, 6, 135); // 20 minutes on the charger between
        clock = ms(THU, 14, 33);
        m->evaluate();
        QVERIFY(poster.posted.isEmpty());
        heart(THU, 16, 0, 9, 40);
        heart(THU, 16, 30, 9, 40);
        clock = ms(THU, 16, 40);
        m->evaluate();
        QVERIFY(poster.posted.isEmpty());
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
        // stock's notification text of HourlyReminderToMoveCelebration (trj.java:166-176): the title, the value
        QCOMPARE(poster.posted[1].body, QString("Goal complete!\n250/250"));
        QCOMPARE(healthJson(poster.posted[1])["type"].toString(), QString("reminder-to-move-complete"));
        QCOMPARE(healthJson(poster.posted[1])["steps"].toInt(), 250);
        // full screen in the launcher (x-asteroid-category celebration), no card left, its vibration the post's
        QCOMPARE(poster.posted[1].hints.value("x-asteroid-category").toString(), QString("celebration"));
        QVERIFY(poster.posted[1].hints.value("transient").toBool());
        QCOMPARE(poster.posted[1].hints.value("x-nemo-feedback").toString(), QString("notif_strong"));
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
        QCOMPARE(poster.posted[0].body, QString("Goal complete!\n9 hr"));
        QCOMPARE(healthJson(poster.posted[0])["hours"].toInt(), 9);
        // a goal achieved (DailyReminderToMoveCelebration): the launcher's screen vibrates, the post does not
        QVERIFY(poster.posted[0].hints.contains("x-nemo-feedback"));
        QVERIFY(poster.posted[0].hints.value("x-nemo-feedback").toString().isEmpty());
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

    // R5 E-05: the goal reached is a celebration post too (stock's GoalAchievedCelebration of the main goal's type)
    void theStepsGoalReachedIsPostedAsItsCelebration()
    {
        MGConfItem::setTestValue(GOAL, 500);
        HealthMonitor *m = make(ms(THU, 8, 0));
        steps(THU, 8, 0, 6, 100);
        clock = ms(THU, 8, 7);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        const HealthNotification &n = poster.posted[0];
        QCOMPARE(n.summary, QString("Steps"));
        QCOMPARE(n.body, QString("Goal complete!\n600"));
        QCOMPARE(n.hints.value("x-asteroid-category").toString(), QString("celebration"));
        QVERIFY(n.hints.value("transient").toBool());
        QVERIFY(n.hints.value("x-nemo-feedback").toString().isEmpty()); // the launcher's screen vibrates
        const QJsonObject j = healthJson(n);
        QCOMPARE(j["type"].toString(), QString("goal"));
        QCOMPARE(j["metric"].toString(), QString("steps"));
        QCOMPARE(j["value"].toInt(), 600);
        QCOMPARE(j["goal"].toInt(), 500);
        clock = ms(THU, 9, 0);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1); // once a day
    }

    // R5 E-18: the main goal's type is the phone's; its value of the day from the day's summary
    void theMainGoalOfAnotherTypeIsItsMetric()
    {
        MGConfItem::setTestValue("/org/asteroidos/health/profile/height-cm", 170);
        MGConfItem::setTestValue("/org/asteroidos/health/goals/main", "distance");
        MGConfItem::setTestValue("/org/asteroidos/health/goals/distance-m", 1000);
        // 1300 steps at 0.70555 m (0.415 x 170 cm): 917 m; then 1500: 1058 m
        testenv::writeDay("stepCounter", THU, QByteArray::number(ms(THU, 8, 10) / 1000) + ":1300\n");
        HealthMonitor *m = make(ms(THU, 8, 0));
        QSignalSpy reached(m, &HealthMonitor::goalReached);
        steps(THU, 8, 0, 10, 60); // what the minutes in memory say does not count for distance (no walk offer)
        clock = ms(THU, 8, 20);
        m->evaluate();
        QCOMPARE(reached.count(), 0);
        // the day's summary at most every 10 minutes
        testenv::writeDay("stepCounter", THU, QByteArray::number(ms(THU, 8, 10) / 1000) + ":1300\n"
                                                  + QByteArray::number(ms(THU, 8, 25) / 1000) + ":1500\n");
        clock = ms(THU, 8, 26);
        m->evaluate();
        QCOMPARE(reached.count(), 0);
        clock = ms(THU, 8, 31);
        m->evaluate();
        QCOMPARE(reached.count(), 1);
        QCOMPARE(reached[0][0].toString(), QString("distance"));
        QCOMPARE(reached[0][2].toInt(), 1000);
        QCOMPARE(poster.posted.size(), 1);
        QCOMPARE(poster.posted[0].summary, QString("Distance"));
        QCOMPARE(poster.posted[0].body, QString("Goal complete!\n1.06 km"));
        const QJsonObject j = healthJson(poster.posted[0]);
        QCOMPARE(j["metric"].toString(), QString("distance"));
        QVERIFY(j["value"].toDouble() > 1000 && j["value"].toDouble() < 1100);
        QCOMPARE(j["goal"].toDouble(), 1000.0);
        MGConfItem::setTestValue("/org/asteroidos/health/goals/main", QVariant());
        MGConfItem::setTestValue("/org/asteroidos/health/goals/distance-m", QVariant());
    }

    void theGoalsKeysAndTheirDefaults()
    {
        QCOMPARE(mainGoalKeys().size(), 6);
        QCOMPARE(mainGoalKeys()[0], QString("/org/asteroidos/health/goals/main"));
        // none set: steps, 10000
        MainGoal g = mainGoalFrom({QVariant(), QVariant(), QVariant(), QVariant(), QVariant(), QVariant()});
        QCOMPARE(g.metric(), QString("steps"));
        QCOMPARE(g.target, 10000.0);
        const QList<QPair<QString, double>> defaults = {{"calories", 2000}, {"distance", 8047}, {"floors", 10}, {"azm", 22}};
        for (const auto &d : defaults) {
            g = mainGoalFrom({d.first, QVariant(), QVariant(), QVariant(), QVariant(), QVariant()});
            QCOMPARE(g.metric(), d.first);
            QCOMPARE(g.target, d.second);
        }
        // the type's own value; 0 or less is the default; an unknown type is steps; case and blanks do not matter
        g = mainGoalFrom({"floors", 8000, 1800, 5000, 14, 30});
        QCOMPARE(g.target, 14.0);
        g = mainGoalFrom({" AZM ", 8000, 1800, 5000, 14, -3});
        QCOMPARE(g.metric(), QString("azm"));
        QCOMPARE(g.target, 22.0);
        g = mainGoalFrom({"sleep", 8000, 1800, 5000, 14, 30});
        QCOMPARE(g.metric(), QString("steps"));
        QCOMPARE(g.target, 8000.0);
        DaySummary d;
        d.steps = 1;
        d.energyKcal = 2;
        d.distanceM = 3;
        d.floors = 4;
        d.zones.azm = 5;
        QCOMPARE(goalValue(GoalType::Steps, d), 1.0);
        QCOMPARE(goalValue(GoalType::Calories, d), 2.0);
        QCOMPARE(goalValue(GoalType::Distance, d), 3.0);
        QCOMPARE(goalValue(GoalType::Floors, d), 4.0);
        QCOMPARE(goalValue(GoalType::Azm, d), 5.0);
    }

    // stock holds a celebration while an exercise is on (trj.java:160-176) and shows it after: so here, posted when
    // the workout ends; a reminder for an hour that is over by then is dropped
    void celebrationsWaitForTheWorkoutsEnd()
    {
        settings->setValue("reminders/enabled", true);
        MGConfItem::setTestValue(GOAL, 1000);
        HealthMonitor *m = make(ms(THU, 10, 0));
        m->setWorkoutActive(true);
        steps(THU, 10, 0, 10, 10);
        clock = ms(THU, 10, 50);
        m->evaluate(); // the reminder of 10:50: held
        steps(THU, 10, 51, 9, 100);
        clock = ms(THU, 10, 59, 59);
        m->evaluate(); // the goal (1000) and the hour's 250: held
        QVERIFY(poster.posted.isEmpty());
        QCOMPARE(m->m_held.size(), 3);
        clock = ms(THU, 11, 5);
        m->setWorkoutActive(false);
        QCOMPARE(m->m_held.size(), 0);
        QCOMPARE(poster.posted.size(), 2); // the 10:50 reminder's hour is over
        QCOMPARE(healthJson(poster.posted[0])["type"].toString(), QString("reminder-to-move-complete"));
        QCOMPARE(healthJson(poster.posted[1])["type"].toString(), QString("goal"));
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
    void aWalkOffersAndAWorkoutMakesNoOffer()
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

        // the Exercise app starts the workout; the walk goes on, then stops: 'Finished working out?' is the app's
        // (M4 WO-07), nothing here after 5, 10 or 20 quiet minutes, with 'End workout reminders' on
        m->setWorkoutActive(true);
        steps(THU, 10, 10, 10, 100);
        clock = ms(THU, 10, 20);
        m->evaluate();
        QCOMPARE(poster.posted.size(), 1);
        for (int quiet : {5, 10, 20}) {
            clock = ms(THU, 10, 20 + quiet, 1);
            m->evaluate();
        }
        QCOMPARE(poster.posted.size(), 1);
        QCOMPARE(offered.count(), 1);
        QVERIFY(settings->value("autoExercise/stop", true).toBool());
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
        QVERIFY(!HealthDb::readNight(healthDbPath(), THU, nullptr));
        // 07:00: not looked at again within 30 minutes
        clock = ms(THU, 7, 0);
        m->evaluate();
        QVERIFY(!HealthDb::readNight(healthDbPath(), THU, nullptr));
        // 07:20: stored, in health.db (no file)
        clock = ms(THU, 7, 20);
        m->evaluate();
        SleepNight inDb;
        QVERIFY(HealthDb::readNight(healthDbPath(), THU, &inDb));
        QVERIFY(!QFile::exists(sleepFilePath(THU)));
        QCOMPARE(testenv::setting("HealthPrivate/sleepStored").toString(), QString("2026-11-12"));
        bool stored = false;
        const SleepNight n = loadSleep(THU, HealthProfile(), 60, &stored);
        QVERIFY(stored);
        QVERIFY(n.valid);
        QCOMPARE(n.main.startMs, ms(eve, 22, 30));
        QCOMPARE(n.main.asleepMin, 480);
        QCOMPARE(n.wakeDate, THU);
        QCOMPARE(n.toMap(), inDb.toMap());
        // once stored it is not looked at again
        const qint64 seq = m->store()->seq();
        clock = ms(THU, 9, 0);
        m->evaluate();
        QCOMPARE(m->store()->seq(), seq);
    }

    // ---- the resting heart rate: once a day, in health.db ----
    void theDaysRestingRateIsEstimatedWithTheNight()
    {
        const QDate eve = THU.addDays(-1);
        HeartrateDb db(heartrateDbPath());
        QVector<HeartrateDb::Reading> rows;
        for (int i = 0; i < 480; i++)
            rows.append({ms(eve, 22, 30) + (qint64)i * 60000 + 15000, 55 + (i % 6), 4});
        QVERIFY(db.add(rows));
        HealthMonitor *m = make(ms(THU, 7, 20));
        QVERIFY(m->m_restingEstimate == 0);
        m->evaluate(); // the night is stored, and with it the day's resting rate
        DailyRecord r;
        QVERIFY(HealthDb::readDaily(healthDbPath(), THU, &r));
        QCOMPARE(r.restingHr, 58); // 480 minutes at 55-60, 80 of each: the median 57.5
        QVERIFY(!r.closed);        // the day is not over
        QCOMPARE(m->m_restingEstimate, 58); // in force from now on
        // estimated once: more readings that day do not change it
        QVector<HeartrateDb::Reading> later;
        for (int i = 0; i < 120; i++)
            later.append({ms(THU, 9, 0) + (qint64)i * 60000 + 15000, 75, 4});
        QVERIFY(db.add(later));
        clock = ms(THU, 12, 0);
        m->evaluate();
        QVERIFY(HealthDb::readDaily(healthDbPath(), THU, &r));
        QCOMPARE(r.restingHr, 58);
        // the day's close keeps it
        QTRY_VERIFY(m->importDone());
        clock = ms(THU.addDays(1), 0, 30);
        m->evaluate();
        QVERIFY(HealthDb::readDaily(healthDbPath(), THU, &r));
        QVERIFY(r.closed);
        QCOMPARE(r.restingHr, 58);
    }

    void aDayWithoutANightGetsItsRateAtTheClose()
    {
        HeartrateDb db(heartrateDbPath());
        QVector<HeartrateDb::Reading> rows;
        for (int i = 0; i < 120; i++)
            rows.append({ms(THU, 10, 0) + (qint64)i * 60000 + 15000, 66, 4});
        QVERIFY(db.add(rows));
        HealthMonitor *m = make(ms(THU, 22, 0));
        QTRY_VERIFY(m->importDone());
        m->evaluate();
        QVERIFY(!HealthDb::readDaily(healthDbPath(), THU, nullptr));
        clock = ms(THU.addDays(1), 0, 40); // the first batch after midnight
        m->evaluate();
        DailyRecord r;
        QVERIFY(HealthDb::readDaily(healthDbPath(), THU, &r));
        QVERIFY(r.closed);
        QCOMPARE(r.restingHr, 66); // over the day itself
        QCOMPARE(estimateRestingHrForDay(THU, 0), 66);
        QCOMPARE(m->m_restingEstimate, 66);
    }

    // ---- health.db: the import at start, the days closed after midnight, the Exercise app's workouts ----
    void theImportRunsFromStartThroughTheEventLoop()
    {
        const QDate wed = THU.addDays(-1);
        testenv::writeDay("stepCounter", wed, QByteArray::number(ms(wed, 9, 9) / 1000) + ":300\n"
                                                  + QByteArray::number(ms(wed, 14, 10) / 1000) + ":1100\n");
        QDir().mkpath(testenv::root() + "exercise");
        QFile log(testenv::root() + "exercise/" + wed.toString("yyyy-MM-dd") + ".log");
        QVERIFY(log.open(QIODevice::WriteOnly));
        log.write(QByteArray::number(ms(wed, 7, 0) / 1000) + ":1800:walk:2400:160:110:130:3300\n");
        log.close();
        // the daemon's store, which tells the readers (HealthChanged) after its writes
        HealthDb store(healthDbPath());
        QList<qint64> told;
        store.setChangeHandler([&](qint64 seq) { told << seq; });
        monitor.reset();
        clock = ms(THU, 9, 0);
        monitor.reset(new HealthMonitor(settings, &poster));
        monitor->setClock([this] { return clock; });
        monitor->setStore(&store);
        monitor->start();
        HealthMonitor *m = monitor.data();
        QVERIFY(!m->importDone()); // not inside start()
        QCOMPARE(m->findChildren<QTimer *>().size(), 2); // and with no timer of its own
        QTRY_VERIFY(m->importDone());
        // a workout and a day written: the readers are told once, at the end
        QCOMPARE(told.size(), 1);
        QCOMPARE(told[0], store.seq());
        QVERIFY(store.seq() >= 2);
        QVERIFY(store.beginWorkout(ms(THU, 8, 0), "walk") > 0); // and after it, every write again
        QCOMPARE(told.size(), 2);
        DailyRecord r;
        QVERIFY(HealthDb::readDaily(healthDbPath(), wed, &r));
        QVERIFY(r.closed);
        QCOMPARE(r.totals.steps, 1100);
        QCOMPARE(HealthDb::readWorkouts(healthDbPath(), 0, ms(THU, 0)).size(), 1);
        QVERIFY(!HealthDb::readDaily(healthDbPath(), THU, nullptr)); // today is not over
        QVERIFY(!m->m_settle->isActive());
        QVERIFY(!m->m_reminderTimer->isActive());
        monitor.reset(); // before the store goes
    }

    // ---- the monitor's own wake (R5 E-04) ----
    // stock wakes for the minute-50 reminder (an exact RTC wakeup alarm) and has the hub's alert events; here the
    // wake carries the reminder minute and a heart rate check 15 minutes after the last look, whichever is first
    void theWakeCarriesTheReminderMinuteAndTheChecks()
    {
        settings->setValue("reminders/enabled", true);
        HealthMonitor *m = makeWithWake(ms(THU, 10, 0));
        heart(THU, 10, 0, 12, 70);
        clock = ms(THU, 10, 12);
        m->evaluate();
        QVERIFY(!m->m_reminderTimer->isActive()); // the wake has it
        QCOMPARE(wake.at(), ms(THU, 10, 27));     // the check before the reminder minute
        QCOMPARE(wake.slack(), (int)HealthMonitor::CHECK_SLACK_MS);
        // the alerts off: the reminder minute, as exact as stock's alarm
        settings->setValue("hrAlerts/highEnabled", false);
        settings->setValue("hrAlerts/lowEnabled", false);
        m->reloadSettings();
        QCOMPARE(wake.at(), ms(THU, 10, 50));
        QCOMPARE(wake.slack(), (int)HealthMonitor::WAKE_SLACK_MS);
        // and nothing else asks for one: no wake
        settings->setValue("reminders/enabled", false);
        m->reloadSettings();
        QCOMPARE(wake.at(), qint64(-1));
        QCOMPARE(m->findChildren<QTimer *>().size(), 3); // the settle, the reminder (idle) and the wake's guard
        QVERIFY(!m->m_wakeGuard->isActive());
    }

    void aWakeFlushesTheHubAndLooksOnceTheBatchIsIn()
    {
        settings->setValue("reminders/enabled", true);
        HealthMonitor *m = makeWithWake(ms(THU, 10, 0));
        heart(THU, 10, 0, 35, 70);
        steps(THU, 10, 0, 10, 10);
        clock = ms(THU, 10, 35);
        m->evaluate();
        QCOMPARE(wake.at(), ms(THU, 10, 50)); // the check would be 10:50 too
        const int scheduled = wake.schedules.size();
        // the time comes, in a pocket: the hub has 15 minutes in its FIFO
        clock = ms(THU, 10, 50, 1);
        emit wake.woke();
        QCOMPARE(wake.flushes, 1);
        QVERIFY(m->m_inWake);
        QVERIFY(poster.posted.isEmpty()); // nothing is looked at before the batch
        emit wake.flushed(2);
        QCOMPARE(wake.releases, 0);
        heart(THU, 10, 35, 15, 72);
        m->batchSettled();
        QTRY_COMPARE(wake.releases, 1);
        QVERIFY(!m->m_inWake);
        QCOMPARE(poster.posted.size(), 1);
        QCOMPARE(poster.posted[0].summary, QString("Reminder to move"));
        QCOMPARE(poster.posted[0].body, QString("150 steps left\nthis hour"));
        // the next: the check 15 minutes after this look (before 11:50)
        QVERIFY(wake.schedules.size() > scheduled);
        QCOMPARE(wake.at(), ms(THU, 11, 5, 1));
    }

    void aRunUnderWayIsLookedAtWhenItsTenthMinuteIsOver()
    {
        HealthMonitor *m = makeWithWake(ms(THU, 13, 59));
        heart(THU, 14, 0, 5, 131); // five minutes over 120 at the last look
        clock = ms(THU, 14, 5, 10);
        m->evaluate();
        QVERIFY(poster.posted.isEmpty());
        QCOMPARE(m->m_alerts.dueMs(), ms(THU, 14, 10));
        QCOMPARE(wake.at(), ms(THU, 14, 10) + HealthMonitor::WAKE_DUE_MARGIN_MS); // not the check at 14:20
        QCOMPARE(wake.slack(), (int)HealthMonitor::WAKE_SLACK_MS);
        clock = wake.at();
        emit wake.woke();
        emit wake.flushed(1);
        heart(THU, 14, 5, 5, 133);
        m->batchSettled();
        QTRY_COMPARE(wake.releases, 1);
        // stock's latency: the alert when the 10 minutes are over, not a check period later
        QCOMPARE(poster.posted.size(), 1);
        QCOMPARE(poster.posted[0].summary, QString("High heart rate"));
        QCOMPARE((qint64)healthJson(poster.posted[0])["end"].toDouble(), ms(THU, 14, 10));
        QCOMPARE(m->m_alerts.dueMs(), qint64(-1)); // the run alerted: the checks go on
        QCOMPARE(wake.at(), ms(THU, 14, 25) + HealthMonitor::WAKE_DUE_MARGIN_MS);
    }

    void noWakeOffTheWristInAWorkoutOrWithTheChecksOff()
    {
        settings->setValue("reminders/enabled", true);
        HealthMonitor *m = makeWithWake(ms(THU, 10, 0));
        heart(THU, 10, 0, 5, 70);
        // the last reading 20 minutes ago: off the wrist (or on the charger, where the hub measures none)
        clock = ms(THU, 10, 25);
        m->evaluate();
        QCOMPARE(wake.at(), qint64(-1));
        // the off-body sensor says on the wrist: both again
        m->setOffBody(0);
        m->evaluate();
        QCOMPARE(wake.at(), ms(THU, 10, 40));
        m->setOffBody(1);
        m->evaluate();
        QCOMPARE(wake.at(), qint64(-1));
        m->setOffBody(-1);
        // worn, a workout on: the reminder minute only (the workout has the hub's 10-second latency)
        heart(THU, 10, 25, 5, 90);
        clock = ms(THU, 10, 30);
        m->setWorkoutActive(true);
        QCOMPARE(wake.at(), ms(THU, 10, 50));
        m->setWorkoutActive(false);
        QCOMPARE(wake.at(), ms(THU, 10, 40)); // 15 minutes after the last look
        // hrAlerts/checkMinutes 0: the alerts at the hub's deliveries only
        settings->setValue("hrAlerts/checkMinutes", 0);
        m->reloadSettings();
        QCOMPARE(wake.at(), ms(THU, 10, 50));
        QVERIFY(poster.posted.isEmpty());
    }

    void aWakeWithNothingBatchedOrNoHubLooksAtOnce()
    {
        HealthMonitor *m = makeWithWake(ms(THU, 9, 0));
        heart(THU, 9, 0, 10, 70);
        clock = ms(THU, 9, 10);
        m->evaluate();
        clock = ms(THU, 9, 25);
        emit wake.woke();
        emit wake.flushed(0); // the display is on: everything is live
        QCOMPARE(wake.releases, 1);
        QVERIFY(!m->m_wakeGuard->isActive());
        QCOMPARE(m->m_lastLookMs, ms(THU, 9, 25));
        heart(THU, 9, 10, 15, 70);
        clock = ms(THU, 9, 40);
        emit wake.woke();
        emit wake.flushed(-1); // no sensorfwd
        QCOMPARE(wake.releases, 2);
        // a flush answered twice or late: nothing more
        emit wake.flushed(0);
        QCOMPARE(wake.releases, 2);
    }

    void aBatchThatDoesNotComeIsLookedAtAfterTheGuard()
    {
        HealthMonitor *m = makeWithWake(ms(THU, 9, 0));
        heart(THU, 9, 0, 10, 70);
        clock = ms(THU, 9, 10);
        m->evaluate();
        clock = ms(THU, 9, 25);
        emit wake.woke();
        emit wake.flushed(3);
        QVERIFY(m->m_wakeGuard->isActive());
        QCOMPARE(m->m_wakeGuard->interval(), (int)HealthMonitor::WAKE_MAX_MS);
        QTRY_COMPARE_WITH_TIMEOUT(wake.releases, 1, HealthMonitor::WAKE_MAX_MS + 2000);
        QVERIFY(!m->m_inWake);
    }

    // battery: worn all the time with the alerts on (the default), 4 wakes an hour; each takes the hub's FIFO, so
    // its own 45-minute wake never comes; none on the charger (no heart rate) after the first check that finds none
    void fourWakesAnHourWhileWornWithTheAlertsOnNoneOnTheCharger()
    {
        HealthMonitor *m = makeWithWake(ms(THU, 8, 0));
        heart(THU, 8, 0, 1, 70);
        clock = ms(THU, 8, 1);
        m->evaluate();
        int wakes = 0;
        qint64 last = clock;
        while (wake.at() > 0 && wake.at() <= ms(THU, 10, 1)) {
            clock = wake.at();
            const int minutes = (int)((clock - last) / 60000);
            emit wake.woke();
            emit wake.flushed(1);
            for (int i = 0; i < minutes; i++)
                monitor->ingestHeart(last + (qint64)i * 60000 + 30000, 68);
            m->batchSettled();
            wakes++;
            QTRY_COMPARE(wake.releases, wakes);
            last = clock;
        }
        QCOMPARE(wakes, 8); // 08:16 ... 10:01: two hours from 08:01
        // on the charger from 10:01: one check finds no new reading, the next finds the watch not worn and stops
        int more = 0;
        while (wake.at() > 0 && more < 5) {
            clock = wake.at();
            emit wake.woke();
            emit wake.flushed(0);
            more++;
        }
        QVERIFY(more <= 2);
        QCOMPARE(wake.at(), qint64(-1));
        QVERIFY(poster.posted.isEmpty());
    }

    // the daemon's wake on libkeepalive (keepaliveWake.cpp): an iphb range from the time and the slack; a time asked
    // while it runs takes effect at the release; none stops it
    void theKeepaliveWakeWaitsInARangeAndHoldsWhileRunning()
    {
        KeepaliveWake k;
        BackgroundActivity *a = BackgroundActivity::last;
        QVERIFY(a);
        QSignalSpy woke(&k, &HealthWake::woke);
        k.schedule(QDateTime::currentMSecsSinceEpoch() + 120000, 30000);
        QVERIFY(a->isWaiting());
        QCOMPARE(a->waits.size(), 1);
        QVERIFY(a->waits[0].first >= 119 && a->waits[0].first <= 120);
        QCOMPARE(a->waits[0].second - a->waits[0].first, 30);
        a->fire();
        QCOMPARE(woke.size(), 1);
        QVERIFY(a->isRunning());
        k.schedule(QDateTime::currentMSecsSinceEpoch() + 900000, 120000);
        QVERIFY(a->isRunning()); // held until the look is done
        QCOMPARE(a->waits.size(), 1);
        k.release();
        QVERIFY(a->isWaiting());
        QCOMPARE(a->waits.size(), 2);
        QCOMPARE(a->waits[1].second - a->waits[1].first, 120);
        // a time in the past: as soon as dsme can
        k.schedule(QDateTime::currentMSecsSinceEpoch() - 5000, 0);
        QCOMPARE(a->waits.last().first, 1);
        k.schedule(-1, 0);
        QVERIFY(a->isStopped());
        k.release(); // not running: nothing
        QVERIFY(a->isStopped());
    }

    void aDayIsClosedOnTheFirstDeliveryAfterMidnight()
    {
        HealthMonitor *m = make(ms(THU, 22, 0));
        QTRY_VERIFY(m->importDone());
        // Thursday has steps; Friday 00:40 the first batch comes
        testenv::writeDay("stepCounter", THU, QByteArray::number(ms(THU, 10, 0) / 1000) + ":4200\n"
                                                  + QByteArray::number(ms(THU, 23, 59, 59) / 1000) + ":6100\n");
        m->evaluate();
        QVERIFY(!HealthDb::readDaily(healthDbPath(), THU, nullptr)); // 22:00: not over
        clock = ms(THU.addDays(1), 0, 40);
        m->evaluate();
        DailyRecord r;
        QVERIFY(HealthDb::readDaily(healthDbPath(), THU, &r));
        QVERIFY(r.closed);
        QCOMPARE(r.totals.steps, 6100);
        QCOMPARE(r.closedMs, ms(THU.addDays(1), 0, 40));
        QCOMPARE(r.totals.distanceM, distanceM(6100, HealthProfile()));
        // closed once: the next deliveries leave it
        const qint64 seq = m->store()->seq();
        clock = ms(THU.addDays(1), 1, 30);
        m->evaluate();
        QCOMPARE(m->store()->seq(), seq);
        // three days off (no daemon): the next start closes the ones with data
        const QDate sat = THU.addDays(2);
        testenv::writeDay("stepCounter", sat, QByteArray::number(ms(sat, 11, 0) / 1000) + ":2500\n");
        HealthMonitor *again = make(ms(THU.addDays(4), 8, 0));
        QVERIFY(again->importDone()); // marked done in the database
        QVERIFY(HealthDb::readDaily(healthDbPath(), sat, &r));
        QCOMPARE(r.totals.steps, 2500);
        QVERIFY(!HealthDb::readDaily(healthDbPath(), THU.addDays(1), nullptr)); // no data: no row
        QVERIFY(!HealthDb::readDaily(healthDbPath(), THU.addDays(3), nullptr));
    }

    // WO-02: the Exercise app writes through the D-Bus methods; a line added to its old log after the one-time
    // import is not taken in, neither when a workout ends nor at the next start
    void aLogLineAfterTheImportIsNotTakenIn()
    {
        HealthMonitor *m = make(ms(THU, 18, 0));
        QTRY_VERIFY(m->importDone());
        m->setWorkoutActive(true);
        QDir().mkpath(testenv::root() + "exercise");
        QFile log(testenv::root() + "exercise/" + THU.toString("yyyy-MM-dd") + ".log");
        QVERIFY(log.open(QIODevice::Append));
        log.write(QByteArray::number(ms(THU, 17, 15) / 1000) + ":2400:run:6000:400:150:170:7000\n");
        log.close();
        m->setWorkoutActive(false);
        QCOMPARE(HealthDb::readWorkouts(healthDbPath(), 0, ms(THU.addDays(1), 0)).size(), 0);
        m = make(ms(THU, 19, 0));
        QTRY_VERIFY(m->importDone());
        QCOMPARE(HealthDb::readWorkouts(healthDbPath(), 0, ms(THU.addDays(1), 0)).size(), 0);
    }

    void noSleepIsEstimatedAtNightOrWithoutAHeartRate()
    {
        HealthMonitor *m = make(ms(THU, 3, 0));
        m->evaluate(); // 03:00: the night is not over
        QVERIFY(m->m_sleepAttemptMs == 0);
        clock = ms(THU, 8, 0);
        m->evaluate(); // no data: nothing to store, tried again later
        QVERIFY(!HealthDb::readNight(healthDbPath(), THU, nullptr));
        QVERIFY(m->m_sleepAttemptMs != 0);
        clock = ms(THU, 21, 0);
        m->m_sleepAttemptMs = 0;
        m->evaluate(); // evening: not looked at
        QVERIFY(m->m_sleepAttemptMs == 0);
    }
};

SENSORLOGD_TEST_MAIN(TestMonitor)
#include "tst_monitor.moc"
