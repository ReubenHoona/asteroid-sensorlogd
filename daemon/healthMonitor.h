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
#include <QVariantMap>

#include <MDConfItem>

#include "healthNotifier.h"
#include "../metrics/autoExercise.h"
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
 * (health-fitness-16), reminders to move and their celebrations (-15), the steps goal reached (-10, -14),
 * auto-detected exercise prompts (-28), the nightly sleep estimate (-17; version 2 with the hub's motion and
 * off-body streams and a stage timeline, M4 WO-06, and the day's naps once they are over), and the rows of health.db
 * it writes: the
 * night, the day's resting heart rate (restingHr.h: in the morning with the night, else at the day's close), and
 * the day closed on the first delivery after its midnight (stock WHS PRE_MIDNIGHT_FLUSH).
 *
 * The monitor is fed by the readings the sensor hub delivers (the plugins' stepsTaken and heartReading, with
 * the time each reading was taken) and looks at them when a delivery is over (the plugins' batchSettled), when
 * a live heart rate reading begins a new minute, and at display on. It has no periodic timer. The only timer
 * besides the short settle delay is one single-shot to the next reminder minute of the window, started only
 * while reminders are on; like every Qt timer it does not run while the system is suspended, so a reminder
 * whose minute passed in suspend is made, if its hour is not over, when data arrives next: the sensor hub's
 * batch or a display on. The one-time import into health.db (store/healthImport.h) runs from start(), one file or
 * day at a time through the event loop (queued calls, no timer), so the daemon goes on answering meanwhile.
 *
 * Settings (QSettings asteroid/sensorlogd, written by the Health app through LoggerSettings):
 *   reminders/enabled (false), reminders/startHour (9), reminders/endHour (18), reminders/days (127)
 *   hrAlerts/highEnabled (true), hrAlerts/highBpm (0: none set, stock's default 120),
 *   hrAlerts/lowEnabled (true), hrAlerts/lowBpm (0: none set, stock's default 50)
 *   autoExercise/start (true); autoExercise/stop (true) is "End workout reminders", which the Exercise app reads
 *   for its own "Finished working out?" (WorkoutStore::endReminders; no stop offer here, autoExercise.h)
 * and the Exercise app's per-type "Start workout reminders": the dconf key /org/asteroidos/exercise/auto-start-off
 * (the types turned off, comma separated; read at each look).
 *
 * The offer last made is kept for the Exercise app (exerciseOffer(), the D-Bus ExerciseOffer): stock starts the
 * workout of an auto-start prompt at the detected start, not at the tap (ExerciseAutoRecognitionService.java:
 * AR_START_TIME_DURATION_FROM_BOOT_MS; xux.java: the start Instant is the boot time plus that duration), so the app
 * asks for the activity's start and the minutes counted since (steps and mean heart rate a minute) to count them
 * in. It is given for OFFER_VALID_MS after the offer and never once a workout has begun.
 * The steps goal is the dconf key /org/asteroidos/ux/steps-goal (10000), the profile the keys of healthProfile.h.
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
    static const int OFFER_VALID_MS = 10 * 60 * 1000; // the offer can start a workout at its start this long

    explicit HealthMonitor(QSettings *settings, NotificationPoster *poster, QObject *parent = nullptr);
    ~HealthMonitor() override;

    // reads today's steps and the recent heart rate from what is logged, and begins to look at the minutes from now
    void start();
    void attach(StepCounterPlugin *steps, HeartrateSensorPlugin *heart);
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
    // the start offer last made, for a workout begun at the activity's start: activity, sinceMs, offeredMs and the
    // minutes since the start that are over (minuteMs, steps, bpm: lists of one length; bpm 0: no heart rate);
    // {} when there is none, it is older than OFFER_VALID_MS or a workout began since
    QVariantMap exerciseOffer() const;

    void ingestSteps(qint64 takenMs, int steps);
    void ingestHeart(qint64 takenMs, int bpm);
    // looks at the minutes that are over and does what they call for
    void evaluate();

    const Health::MinuteSeries &series() const { return m_series; }

signals:
    // a daily goal was reached for the first time today (metric "steps"): the launcher's celebration
    void goalReached(const QString &metric, int value, int goal);
    // auto-detected exercise: action "start", activity "walking" / "running" (the stop is the Exercise app's)
    void exerciseOffered(const QString &action, const QString &activity, qlonglong sinceMs);
    // the hub's skin temperature should be sampled now (inside a night's sleep) or not
    void skinTempSamplingChanged(bool on);

private slots:
    void batchSettled();
    void importStep();
    // the morning's skin temperature night, once its night is stored and the sampling is off
    void checkSkinTemp();

private:
    struct Settings {
        Health::MoveReminderSettings reminders;
        Health::HrAlertThresholds hrAlerts;
        Health::AutoExerciseSettings exercise;
    };
    qint64 now() const { return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch(); }
    void loadProfile(bool force = false);
    void armReminderTimer();
    void rollDay(const QDate &today);
    void postAlert(const Health::HrAlert &alert);
    void postMove(const Health::MoveEvent &event);
    void postExercise(const Health::ExerciseOffer &offer);
    uint post(HealthNotification n);
    void checkGoal();
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
    Health::ExerciseOffer m_offer = {QString(), 0};
    qint64 m_offeredMs = 0; // when m_offer was made (0: none)
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
    MDConfItem *m_goalItem;
    MDConfItem *m_twelveHourItem;
    MDConfItem *m_startOffItem;
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
