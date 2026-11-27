/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HEALTHMONITOR_H
#define HEALTHMONITOR_H

#include <functional>

#include <QDate>
#include <QDateTime>
#include <QObject>
#include <QScopedPointer>
#include <QSettings>
#include <QTimer>

#include <MDConfItem>

#include "healthNotifier.h"
#include "healthWake.h"
#include "../metrics/autoExercise.h"
#include "../metrics/goals.h"
#include "../metrics/healthMetrics.h"
#include "../metrics/hrAlerts.h"
#include "../metrics/moveReminder.h"
#include "../metrics/skinTemp.h"
#include "../store/healthDb.h"
#include "../store/healthImport.h"

class StepCounterPlugin;
class HeartrateSensorPlugin;

/*
 * What the Health app does on its own account while the watch is worn: high and low heart rate alerts
 * (health-fitness-16), reminders to move and their celebrations (-15), the main goal reached (-10, -14; its type
 * steps, calories, distance, floors or Active Zone Minutes: metrics/goals.h, R5 E-05 / E-18),
 * auto-detected exercise prompts (-28), the nightly sleep estimate (-17; version 2 with the hub's motion and
 * off-body streams and a stage timeline, M4 WO-06, and the day's naps once they are over), and the rows of health.db
 * it writes: the
 * night, the day's resting heart rate (restingHr.h: in the morning with the night, else at the day's close), and
 * the day closed on the first delivery after its midnight (stock WHS PRE_MIDNIGHT_FLUSH).
 *
 * The monitor is fed by the readings the sensor hub delivers (the plugins' stepsTaken and heartReading, with
 * the time each reading was taken) and looks at them when a delivery is over (the plugins' batchSettled), when
 * a live heart rate reading begins a new minute, and at display on. It has no periodic timer. The one-time import
 * into health.db (store/healthImport.h) runs from start(), one file or day at a time through the event loop
 * (queued calls, no timer), so the daemon goes on answering meanwhile.
 *
 * Its own wake (R5 E-04, healthWake.h): with the screen off the hub delivers its batch every 45 minutes, so a
 * reminder to move or a heart rate alert made only at a delivery came up to 45 minutes late, and the minute-50
 * reminder never came in a pocket. Stock wakes for both: the reminder is an exact RTC wakeup alarm at minute 50
 * (RtmManager.java:190-201), the alerts are the hub's own events, due when the 10 inactive minutes are over
 * (WHS ELEVATED_HR_DETECTED / LOW_HR_DETECTED; PLAN F03). Here, with a HealthWake (setWake), the monitor wakes the
 * system at the next of:
 *   the reminder minute (reminders on and worn at the last look),
 *   a heart rate check HR_CHECK_MIN (hrAlerts/checkMinutes) after the last look (an alert on, worn at the last look,
 *     no workout: the workout's own latency is 10 s, WO-08),
 *   the end of the 10th minute of a run of the alerts' that the last look saw begin (so an alert comes when stock's
 *     would, not a check period later);
 * has the hub hand over its batch (flushHub), looks once the batch is in (or WAKE_MAX_MS after the wake at the
 * latest), and lets the system sleep again (release). Off the wrist or on the charger the hub measures no heart rate,
 * so worn() turns false within a check or two and the wakes stop until a delivery or a display on finds it worn
 * again. Each check takes the hub's FIFO, so its own 45-minute wake does not come while they run: 4 wakes an hour
 * instead of 1.33 while worn with the alerts on (the default), no more for the reminders (the minute-50 look is a
 * check too), none at all with both off. Without a HealthWake (the tests', health/wake false) the reminder minute
 * is a single-shot QTimer as before (awake time only).
 *
 * Settings (QSettings asteroid/sensorlogd, written by the Health app through LoggerSettings):
 *   reminders/enabled (false), reminders/startHour (9), reminders/endHour (18), reminders/days (127)
 *   hrAlerts/highEnabled (true), hrAlerts/highBpm (0: none set, stock's default 120),
 *   hrAlerts/lowEnabled (true), hrAlerts/lowBpm (0: none set, stock's default 50),
 *   hrAlerts/checkMinutes (15: the heart rate check's period with the screen off; 0: none, the alerts at the
 *   hub's deliveries only)
 *   autoExercise/start (true); autoExercise/stop (true) is "End workout reminders", which the Exercise app reads
 *   for its own "Finished working out?" (WorkoutStore::endReminders; no stop offer here, autoExercise.h)
 * The main goal is the dconf keys of goals.h (the type, steps by default: /org/asteroidos/ux/steps-goal, 10000), the
 * profile the keys of healthProfile.h.
 *
 * Celebrations (R5 E-05, E-06): the main goal reached, a reminder to move, its hour's goal reached and every hour
 * of the window met are posted with x-asteroid-category "celebration" (transient: no card is left), the launcher's
 * full screen (stock's CelebrationActivity, started directly: trj.java:115-185). During a workout stock holds them
 * until the exercise is over (trj.java:160-176, the queue c() empties after it): so here, posted at its end (a
 * reminder whose hour is over by then is dropped).
 *
 * Worn (no reminder to move off the wrist): the sensor hub's off-body sensor when it knows (setOffBody, from the
 * logger's OffBodyPlugin, M4 WO-03; stock gates on it too: sysui's LlobDetector, the hub's algorithms); when it does
 * not (the provider off in eos-usf.conf, an older sensorfwd, the hub not up), a heart rate within WORN_MS, as
 * before (the hub measures only on the wrist).
 *
 * Skin temperature (M4 WO-13, metrics/skinTemp.h): at every look the monitor decides whether the hub's skin
 * temperature should be sampled now (skinTempWanted: inside a night's sleep) and says so (skinTempSamplingChanged;
 * the logger's SkinTempPlugin subscribes then); health.db meta 'skin_temp.on_ms' (since when it runs, 0: not) and
 * 'skin_temp.off_ms' (when it last stopped) are what the Health app shows "Tracking..." and "Analyzing..." by. Once the
 * morning's night is stored and the sampling is off (SKIN_SETTLE_MS after it stopped: the hub's last batch is in),
 * the night's value, baseline and variation go to health.db skin_temp_night (once a morning, 05:00-20:00).
 */
class HealthMonitor : public QObject
{
    Q_OBJECT
public:
    static const int SETTLE_MS = 500;          // after a batch is over
    static const int DISPLAY_SETTLE_MS = 2500; // after display on, if no batch follows
    static const int LIVE_AGE_MS = 5000;       // a reading younger than this is live, not from a batch
    static const int WORN_MS = 15 * 60 * 1000; // a heart rate this recent: the watch is on the wrist
    static const int HISTORY_HOURS = 6;        // minutes before today kept in memory
    static const int SKIN_SETTLE_MS = 3000;    // after the skin temperature stopped: its last batch is in
    static const int HR_CHECK_MIN = 15;        // the heart rate check's period (hrAlerts/checkMinutes)
    static const int WAKE_MAX_MS = 4000;       // a wake looks at the data this long after it began at the latest
    static const int WAKE_MIN_MS = 30000;      // the earliest wake from now
    static const int WAKE_DUE_MARGIN_MS = 5000; // after a run's 10th minute: its last reading is with the hub
    static const int WAKE_SLACK_MS = 30000;    // a reminder's or a run's wake may come this much later
    static const int CHECK_SLACK_MS = 120000;  // a check's: room for dsme to align it with another wake

    explicit HealthMonitor(QSettings *settings, NotificationPoster *poster, QObject *parent = nullptr);
    ~HealthMonitor() override;

    // reads today's steps and the recent heart rate from what is logged, and begins to look at the minutes from now
    void start();
    void attach(StepCounterPlugin *steps, HeartrateSensorPlugin *heart);
    // the system wake of its own (R5 E-04, healthWake.h; not owned); without one, the reminder minute's QTimer
    void setWake(HealthWake *wake);
    // the daemon's health database (one writer: the Logger's); without one, start() opens its own
    void setStore(Health::HealthDb *store) { m_store = store; }
    Health::HealthDb *store() const { return m_store; }
    // the one-time import is through (the days are closed from then on)
    bool importDone() const { return m_import && m_import->done(); }
    // the clock the monitor goes by (ms since the epoch); the system's by default
    void setClock(std::function<qint64()> clock) { m_clock = clock; }

    void reloadSettings();
    // the display came on: what the hub batched is delivered now; look at it once it is in
    void displayOn();
    // the Exercise app has a workout going (no "Start tracking?" offer during it)
    void setWorkoutActive(bool active);
    // the off-body sensor's state (OffBodyPlugin): -1 unknown, 0 on the wrist, 1 off it
    void setOffBody(int state) { m_offBody = state; }
    // the hub's skin temperature can be sampled (the logger has its plugin): the monitor decides when (M4 WO-13)
    void setSkinTempAvailable(bool available);
    bool skinTempSampling() const { return m_skinTempOn; }
    // the time of the next reminder minute, ms since the epoch; -1: none
    qint64 nextReminderMs() const;
    // the time the monitor wants to look at the data next by its own wake (R5 E-04), ms since the epoch; -1: none.
    // slackMs: how much later it may come
    qint64 nextWakeMs(int *slackMs = nullptr) const;

    void ingestSteps(qint64 takenMs, int steps);
    void ingestHeart(qint64 takenMs, int bpm);
    // looks at the minutes that are over and does what they call for
    void evaluate();

    const Health::MinuteSeries &series() const { return m_series; }

signals:
    // the main goal was reached for the first time today (metric "steps", "calories", "distance", "floors", "azm";
    // value and goal rounded): its celebration is posted with it
    void goalReached(const QString &metric, int value, int goal);
    // auto-detected exercise: action "start", activity "walking" / "running" (the stop is the Exercise app's)
    void exerciseOffered(const QString &action, const QString &activity, qlonglong sinceMs);
    // the hub's skin temperature should be sampled now (inside a night's sleep) or not
    void skinTempSamplingChanged(bool on);

private slots:
    void batchSettled();
    void importStep();
    // the wake (R5 E-04): it came, the hub answered the flush, the batch did not come in time
    void woke();
    void hubFlushed(int samplings);
    void wakeTimedOut();
    // the morning's skin temperature night, once its night is stored and the sampling is off
    void checkSkinTemp();
    // Bedtime mode on or off: a record in bedtime/ when it changed (R5 E-30)
    void recordBedtime();

private:
    struct Settings {
        Health::MoveReminderSettings reminders;
        Health::HrAlertThresholds hrAlerts;
        Health::AutoExerciseSettings exercise;
        int hrCheckMin = HR_CHECK_MIN;
    };
    qint64 now() const { return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch(); }
    void loadProfile(bool force = false);
    void armReminderTimer();
    // an alert is on and the watch was worn at the last look, no workout: the heart rate checks run
    bool hrChecksWanted(qint64 nowMs) const;
    void rollDay(const QDate &today);
    void postAlert(const Health::HrAlert &alert);
    void postMove(const Health::MoveEvent &event);
    void postExercise(const Health::ExerciseOffer &offer);
    void postGoal(const Health::MainGoal &goal, double value);
    // a celebration: posted now, or held while a workout is on (stock's queue)
    void postCelebration(const HealthNotification &n);
    void releaseCelebrations();
    uint post(HealthNotification n);
    void checkGoal();
    Health::MainGoal mainGoal() const;
    void checkSleep();
    // the skin temperature sampling on or off by now
    void updateSkinTemp();
    // the days before today that have data and no closed row in health.db: closed now
    void closeDays();
    bool dayTotals(const QDate &day, Health::DailyTotals *totals);
    // the day's resting heart rate, estimated once (nightEndMs: the end of the night that ended on it, 0: none)
    int restingOf(const QDate &day, qint64 nightEndMs);
    void storeRestingHr(const QDate &day, qint64 nightEndMs);
    qint64 nightEndOf(const QDate &wakeDate) const;
    QString clockText(qint64 ms) const;
    bool worn(qint64 nowMs) const;

    QSettings *m_settings;
    NotificationPoster *m_poster;
    std::function<qint64()> m_clock;
    Settings m_config;
    Health::HealthProfile m_profile;
    qint64 m_profileAt = 0;
    int m_restingEstimate = 0;
    Health::MinuteSeries m_series;
    Health::HrAlertDetector m_alerts;
    Health::MoveReminder m_move;
    Health::AutoExercise m_exercise;
    bool m_workoutActive = false;
    int m_offBody = -1;
    bool m_hrAttached = false;
    qint64 m_lastHrMs = 0;
    qint64 m_lastLiveMinute = -1;
    QDate m_day;
    qint64 m_sleepAttemptMs = 0;
    uint m_alertId = 0; // the heart rate alert on screen: the next one replaces it (stock: one notification id)
    QTimer *m_settle;
    QTimer *m_reminderTimer;
    HealthWake *m_wake = nullptr;
    QTimer *m_wakeGuard = nullptr;   // made with the wake: WAKE_MAX_MS
    bool m_inWake = false;           // a wake is running: released after the look
    bool m_wakeAwaitingBatch = false; // the hub flushed: its batch is on the way
    qint64 m_lastLookMs = 0;         // the last evaluate()
    qint64 m_wakeAt = -1;            // what was asked of the wake last
    QList<MDConfItem *> m_goalItems; // goals.h mainGoalKeys()
    qint64 m_goalCheckMs = 0;        // the last day summary for a goal of another type than steps
    struct HeldCelebration {
        HealthNotification notification;
        qint64 atMs;
    };
    QList<HeldCelebration> m_held;   // celebrations of a workout's time, posted at its end
    MDConfItem *m_twelveHourItem;
    MDConfItem *m_bedtimeItem;
    Health::HealthDb *m_store = nullptr;
    QScopedPointer<Health::HealthDb> m_ownStore;
    QScopedPointer<Health::HealthImport> m_import;
    bool m_holdingChanges = false; // the import's writes tell the readers once, at its end
    QDate m_closedThrough; // the days up to it are closed (or have no data)
    bool m_skinTempAvailable = false;
    bool m_skinTempOn = false;
    qint64 m_skinTempOffMs = 0;
    QDate m_skinTempDone; // the morning whose skin temperature night is in health.db
};

#endif // HEALTHMONITOR_H
