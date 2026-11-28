/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cmath>

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTime>

#include "healthMonitor.h"
#include "healthLanguage.h"
#include "sensorPlugins/heartrateSensor.h"
#include "sensorPlugins/stepCounter.h"
#include "../common.h"
#include "../metrics/healthData.h"
#include "../metrics/restingHr.h"

using namespace Health;

static const char *const TWELVE_HOUR_KEY = "/org/asteroidos/settings/use-12h-format";
// the Exercise app's types with 'Start workout reminders' off (its ExerciseSettings.qml)
static const char *const START_OFF_KEY = "/org/asteroidos/exercise/auto-start-off";
static const char *const BEDTIME_KEY = "/org/asteroidos/modes/bedtime"; // the launcher's Bedtime mode (modes.h)
static const int MORNING_FROM_HOUR = 4; // the launcher's morning (modescore.h MorningFromHour, MorningToHour)
static const int MORNING_TO_HOUR = 12;
static const int GOAL_CHECK_MS = 10 * 60 * 1000; // a goal of another type than steps: its day summary at most this often
static const int PROFILE_CACHE_MS = 10 * 60 * 1000;
static const int SLEEP_RETRY_MS = 30 * 60 * 1000;
static const int SLEEP_QUIET_MS = 30 * 60 * 1000; // awake this long: the night is over
static const int CLOSE_DAYS_BACK = 31;            // the most days closed at once after a long time off

static QString json(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

HealthMonitor::HealthMonitor(QSettings *settings, NotificationPoster *poster, QObject *parent)
    : QObject(parent), m_settings(settings), m_poster(poster)
{
    for (const QString &key : mainGoalKeys())
        m_goalItems << new MDConfItem(key, this);
    m_twelveHourItem = new MDConfItem(TWELVE_HOUR_KEY, this);
    m_startOffItem = new MDConfItem(START_OFF_KEY, this);
    m_bedtimeItem = new MDConfItem(BEDTIME_KEY, this);
    connect(m_bedtimeItem, &MDConfItem::valueChanged, this, &HealthMonitor::recordBedtime);
    m_settle = new QTimer(this);
    m_settle->setSingleShot(true);
    connect(m_settle, &QTimer::timeout, this, &HealthMonitor::evaluate);
    m_reminderTimer = new QTimer(this);
    m_reminderTimer->setSingleShot(true);
    connect(m_reminderTimer, &QTimer::timeout, this, &HealthMonitor::evaluate);
    reloadSettings();
}

HealthMonitor::~HealthMonitor()
{
    if (m_holdingChanges && m_store)
        m_store->releaseChanges();
}

void HealthMonitor::reloadSettings()
{
    m_settings->sync();
    Settings s;
    s.reminders.enabled = m_settings->value("reminders/enabled", false).toBool();
    s.reminders.startHour = qBound(0, m_settings->value("reminders/startHour", 9).toInt(), 23);
    s.reminders.endHour = qBound(1, m_settings->value("reminders/endHour", 18).toInt(), 24);
    s.reminders.days = m_settings->value("reminders/days", 0x7f).toInt() & 0x7f;
    s.hrAlerts.highEnabled = m_settings->value("hrAlerts/highEnabled", true).toBool();
    s.hrAlerts.high = m_settings->value("hrAlerts/highBpm", 0).toInt();
    s.hrAlerts.lowEnabled = m_settings->value("hrAlerts/lowEnabled", true).toBool();
    s.hrAlerts.low = m_settings->value("hrAlerts/lowBpm", 0).toInt();
    s.exercise.start = m_settings->value("autoExercise/start", true).toBool();
    s.hrCheckMin = qBound(0, m_settings->value("hrAlerts/checkMinutes", HR_CHECK_MIN).toInt(), 24 * 60);
    m_config = s;
    m_profileAt = 0; // the profile too may have changed
    armReminderTimer();
}

void HealthMonitor::loadProfile(bool force)
{
    const qint64 t = now();
    if (!force && m_profileAt != 0 && t - m_profileAt < PROFILE_CACHE_MS && t >= m_profileAt)
        return;
    m_profile = HealthProfile::load(QDateTime::fromMSecsSinceEpoch(t).date());
    m_profileAt = t;
}

void HealthMonitor::start()
{
    const qint64 t = now();
    m_day = QDateTime::fromMSecsSinceEpoch(t).date();
    m_series.clear();
    loadSteps(m_series, m_day);
    // the recent heart rate shows whether the watch is on the wrist; the minutes before now are not looked at
    // for alerts (a restart is no reason to alert for them again)
    loadHeartRate(m_series, t - WORN_MS, t);
    for (auto it = m_series.data().constBegin(); it != m_series.data().constEnd(); ++it)
        if (it->hr > 0)
            m_lastHrMs = qMax(m_lastHrMs, MinuteSeries::msOf(it.key()));
    m_alerts.setSeen(MinuteSeries::minuteOf(t) - 1);
    m_exercise.setSeen(MinuteSeries::minuteOf(t) - 1);
    m_restingEstimate = estimatedRestingHr(t);
    MoveReminder::State state;
    state.remindedHour = m_settings->value("HealthPrivate/rtmRemindedHour", -1).toLongLong();
    state.completedHour = m_settings->value("HealthPrivate/rtmCompletedHour", -1).toLongLong();
    state.celebratedDay = m_settings->value("HealthPrivate/rtmCelebratedDay", -1).toLongLong();
    m_move.setState(state);
    loadProfile(true);
    armReminderTimer();
    recordBedtime();

    // health.db: the daemon's (one writer), or one of our own
    if (!m_store) {
        m_ownStore.reset(new HealthDb(healthDbPath()));
        m_store = m_ownStore.data();
    }
    m_closedThrough = QDate();
    m_import.reset(new HealthImport(m_store, loggerRootPath(),
                                    [this](const QDate &day, DailyTotals *totals) { return dayTotals(day, totals); },
                                    [this](const QDate &day) { return restingOf(day, nightEndOf(day)); }, m_day));
    if (!m_import->done()) {
        // one HealthChanged for the whole import, not one for every file and day
        if (!m_holdingChanges) {
            m_store->holdChanges();
            m_holdingChanges = true;
        }
        QMetaObject::invokeMethod(this, "importStep", Qt::QueuedConnection);
    } else {
        closeDays();
    }
}

void HealthMonitor::importStep()
{
    if (!m_import || m_import->done())
        return;
    if (m_import->step()) {
        QMetaObject::invokeMethod(this, "importStep", Qt::QueuedConnection);
        return;
    }
    if (m_holdingChanges) {
        m_holdingChanges = false;
        m_store->releaseChanges();
    }
    if (!m_import->done())
        return; // could not be marked: the next start goes on
    m_restingEstimate = estimatedRestingHr(now());
    closeDays();
}

void HealthMonitor::attach(StepCounterPlugin *steps, HeartrateSensorPlugin *heart)
{
    if (steps) {
        connect(steps, &StepCounterPlugin::stepsTaken, this, &HealthMonitor::ingestSteps);
        connect(steps, &StepCounterPlugin::batchSettled, this, &HealthMonitor::batchSettled);
    }
    if (heart) {
        m_hrAttached = true;
        connect(heart, &HeartrateSensorPlugin::heartReading, this, &HealthMonitor::ingestHeart);
        connect(heart, &HeartrateSensorPlugin::batchSettled, this, &HealthMonitor::batchSettled);
    }
}

void HealthMonitor::setWorkoutActive(bool active)
{
    // (the Exercise app's workouts come through the daemon's D-Bus methods since WO-02: no log lines to take in)
    m_workoutActive = active;
    // no heart rate checks of the monitor's own during a workout (its latency is 10 s, WO-08); again after it
    armReminderTimer();
    if (!active)
        releaseCelebrations();
    // a workout began: the offer was taken (or another workout is the activity)
    if (active)
        m_offeredMs = 0;
}

QVariantMap HealthMonitor::exerciseOffer() const
{
    const qint64 t = now();
    if (m_offeredMs <= 0 || m_workoutActive || t - m_offeredMs > OFFER_VALID_MS || t < m_offeredMs)
        return QVariantMap();
    QVariantList minutes, steps, bpm;
    const qint64 lastDone = MinuteSeries::minuteOf(t) - 1;
    for (qint64 minute = MinuteSeries::minuteOf(m_offer.sinceMs); minute <= lastDone; minute++) {
        const Minute m = m_series.at(minute);
        minutes << (qlonglong)MinuteSeries::msOf(minute);
        steps << m.steps;
        bpm << (m.hrCount > 0 ? m.hr : 0.0);
    }
    QVariantMap o;
    o.insert("activity", m_offer.activity);
    o.insert("sinceMs", (qlonglong)m_offer.sinceMs);
    o.insert("offeredMs", (qlonglong)m_offeredMs);
    o.insert("minuteMs", minutes);
    o.insert("steps", steps);
    o.insert("bpm", bpm);
    return o;
}

void HealthMonitor::batchSettled()
{
    m_wakeAwaitingBatch = false;
    m_settle->start(SETTLE_MS);
}

void HealthMonitor::setWake(HealthWake *wake)
{
    m_wake = wake;
    if (!m_wake)
        return;
    m_reminderTimer->stop();
    if (!m_wakeGuard) {
        m_wakeGuard = new QTimer(this);
        m_wakeGuard->setSingleShot(true);
        connect(m_wakeGuard, &QTimer::timeout, this, &HealthMonitor::wakeTimedOut);
    }
    connect(m_wake, &HealthWake::woke, this, &HealthMonitor::woke);
    connect(m_wake, &HealthWake::flushed, this, &HealthMonitor::hubFlushed);
}

// The system is up for the monitor's own look (R5 E-04): the hub hands over its batch first, so the look sees the
// minutes up to now; held awake until evaluate() is through (armReminderTimer releases it)
void HealthMonitor::woke()
{
    m_inWake = true;
    m_wakeAwaitingBatch = false;
    m_wakeAt = -1;
    m_wakeGuard->start(WAKE_MAX_MS);
    m_wake->flushHub();
}

void HealthMonitor::hubFlushed(int samplings)
{
    if (!m_inWake)
        return;
    if (samplings > 0) {
        // the readings come as a batch: its batchSettled looks at them (the guard if none comes)
        m_wakeAwaitingBatch = true;
        return;
    }
    // nothing waits on the hub (the display is on: all of it is live) or no answer: what there is
    evaluate();
}

void HealthMonitor::wakeTimedOut()
{
    m_wakeAwaitingBatch = false;
    if (m_inWake)
        evaluate();
}

void HealthMonitor::displayOn()
{
    // the hub's batch comes after this (its batchSettled shortens the wait); with none, look at what there is
    if (!m_settle->isActive())
        m_settle->start(DISPLAY_SETTLE_MS);
}

void HealthMonitor::ingestSteps(qint64 takenMs, int steps)
{
    m_series.addSteps(takenMs, steps);
}

void HealthMonitor::ingestHeart(qint64 takenMs, int bpm)
{
    m_series.addHr(takenMs, bpm);
    m_lastHrMs = qMax(m_lastHrMs, takenMs);
    // a live reading in a new minute: the minute before it is complete
    const qint64 minute = MinuteSeries::minuteOf(takenMs);
    if (now() - takenMs < LIVE_AGE_MS && minute > m_lastLiveMinute) {
        m_lastLiveMinute = minute;
        evaluate();
    }
}

bool HealthMonitor::worn(qint64 nowMs) const
{
    // the off-body sensor when it knows (M4 WO-03)
    if (m_offBody == 0)
        return true;
    if (m_offBody == 1)
        return false;
    return !m_hrAttached || (m_lastHrMs > 0 && nowMs - m_lastHrMs <= WORN_MS);
}

void HealthMonitor::rollDay(const QDate &today)
{
    if (m_day == today)
        return;
    m_day = today;
    // keep the evening before for a night's sleep
    m_series.trimBefore(MinuteSeries::minuteOf(QDateTime(today, QTime(0, 0)).toMSecsSinceEpoch()) - HISTORY_HOURS * 60);
    m_restingEstimate = estimatedRestingHr(now());
    loadProfile(true);
}

QString HealthMonitor::clockText(qint64 ms) const
{
    const bool twelve = m_twelveHourItem->value(true).toBool();
    return QLocale::c().toString(QDateTime::fromMSecsSinceEpoch(ms).time(), twelve ? "h:mm AP" : "HH:mm");
}

void HealthMonitor::evaluate()
{
    const qint64 t = now();
    m_lastLookMs = t;
    rollDay(QDateTime::fromMSecsSinceEpoch(t).date());
    loadProfile();

    // 1. heart rate alerts: the user's thresholds, else stock's defaults
    for (const HrAlert &alert : m_alerts.feed(m_series, t, m_config.hrAlerts.highBpm(), m_config.hrAlerts.highEnabled,
                                              m_config.hrAlerts.lowBpm(), m_config.hrAlerts.lowEnabled))
        postAlert(alert);

    // 2. reminders to move
    for (const MoveEvent &event : m_move.evaluate(t, m_series, m_config.reminders, worn(t)))
        postMove(event);
    const MoveReminder::State state = m_move.state();
    m_settings->setValue("HealthPrivate/rtmRemindedHour", state.remindedHour);
    m_settings->setValue("HealthPrivate/rtmCompletedHour", state.completedHour);
    m_settings->setValue("HealthPrivate/rtmCelebratedDay", state.celebratedDay);

    // 3. the steps goal
    checkGoal();

    // 4. auto-detected exercise (the types whose start reminder the Exercise app has off, read as they are now)
    Health::AutoExerciseSettings exercise = m_config.exercise;
    for (const QString &type : m_startOffItem->value(QString()).toString().split(',', Qt::SkipEmptyParts))
        exercise.startOff << type.trimmed();
    for (const ExerciseOffer &offer : m_exercise.feed(m_series, t, exercise, m_workoutActive))
        postExercise(offer);

    // 5. the skin temperature, sampled inside a night's sleep only (M4 WO-13)
    updateSkinTemp();

    // 6. last night's sleep
    checkSleep();

    // 7. last night's skin temperature, once the night is stored and the sampling is off
    checkSkinTemp();

    // 8. the days that are over
    closeDays();

    armReminderTimer();
}

qint64 HealthMonitor::nextReminderMs() const
{
    return MoveReminder::nextReminderMs(now(), m_config.reminders);
}

bool HealthMonitor::hrChecksWanted(qint64 nowMs) const
{
    return m_config.hrCheckMin > 0 && m_hrAttached && !m_workoutActive
           && (m_config.hrAlerts.highEnabled || m_config.hrAlerts.lowEnabled) && worn(nowMs);
}

qint64 HealthMonitor::nextWakeMs(int *slackMs) const
{
    const qint64 t = now();
    qint64 next = -1;
    int slack = 0;
    auto take = [&](qint64 at, int s) {
        if (at < 0)
            return;
        at = qMax(at, t + WAKE_MIN_MS);
        if (next < 0 || at < next) {
            next = at;
            slack = s;
        }
    };
    // the reminder minute (a reminder is no use off the wrist: MoveReminder::evaluate does not make one there)
    if (m_config.reminders.enabled && worn(t))
        take(MoveReminder::nextReminderMs(t, m_config.reminders), WAKE_SLACK_MS);
    if (hrChecksWanted(t)) {
        take((m_lastLookMs > 0 ? m_lastLookMs : t) + (qint64)m_config.hrCheckMin * 60000, CHECK_SLACK_MS);
        // a run under way: when its 10th minute is over
        const qint64 due = m_alerts.dueMs();
        if (due >= 0)
            take(due + WAKE_DUE_MARGIN_MS, WAKE_SLACK_MS);
    }
    if (slackMs)
        *slackMs = slack;
    return next;
}

// The next look of the monitor's own: by its wake (R5 E-04; and the running wake, if any, released: the look is
// done), else the reminder minute's single-shot QTimer (awake time only)
void HealthMonitor::armReminderTimer()
{
    m_reminderTimer->stop();
    if (m_wake) {
        // not released while the flushed batch is on its way or about to be looked at: that look releases it
        const bool release = m_inWake && !m_wakeAwaitingBatch && !m_settle->isActive();
        int slack = 0;
        const qint64 next = nextWakeMs(&slack);
        if (release || next != m_wakeAt) {
            m_wakeAt = next;
            m_wake->schedule(next, slack);
        }
        if (release) {
            m_inWake = false;
            m_wakeGuard->stop();
            m_wake->release();
        }
        return;
    }
    const qint64 next = nextReminderMs();
    if (next < 0)
        return;
    m_reminderTimer->start((int)qBound<qint64>(1000, next - now(), 24LL * 3600 * 1000));
}

uint HealthMonitor::post(HealthNotification n)
{
    if (n.icon.isEmpty())
        n.icon = QStringLiteral("ios-heart");
    n.hints.insert("x-nemo-preview-summary", n.summary);
    if (!n.body.isEmpty())
        n.hints.insert("x-nemo-preview-body", n.body);
    // the vibration, unless the post has its own (a goal's: none, the launcher's screen vibrates)
    if (!n.hints.contains("x-nemo-feedback"))
        n.hints.insert("x-nemo-feedback", QStringLiteral("notif_strong"));
    return m_poster->post(n);
}

void HealthMonitor::postCelebration(const HealthNotification &n)
{
    if (m_workoutActive) {
        m_held.append({n, now()});
        return;
    }
    post(n);
}

// The workout is over: what it held, in order; a reminder to move for an hour that is over by now says nothing
void HealthMonitor::releaseCelebrations()
{
    const QList<HeldCelebration> held = m_held;
    m_held.clear();
    const qint64 hour = MinuteSeries::minuteOf(now()) / 60;
    for (const HeldCelebration &h : held) {
        const QJsonObject j = QJsonDocument::fromJson(h.notification.hints.value("x-asteroid-health").toString().toUtf8()).object();
        if (j.value("type").toString() == QLatin1String("reminder-to-move") && MinuteSeries::minuteOf(h.atMs) / 60 != hour)
            continue;
        post(h.notification);
    }
}

// stock's texts: stock/STRINGS.tsv 0x7f14034c/d (titles), 0x7f1409a0 (N bpm), 0x7f1409a1 / 0x7f1409a3 (detail),
// in the watch's language (R6 K-05: healthLanguage.h; the English here is what shows without a catalogue)
void HealthMonitor::postAlert(const HrAlert &alert)
{
    using HealthText::text;
    const bool high = alert.kind == HrAlert::High;
    HealthNotification n;
    //% "High heart rate"
    const QString highTitle = text(QT_TRID_NOOP("id-health-hr-high"), "High heart rate");
    //% "Low heart rate"
    const QString lowTitle = text(QT_TRID_NOOP("id-health-hr-low"), "Low heart rate");
    n.summary = high ? highTitle : lowTitle;
    //% "%1 bpm"
    n.body = text(QT_TRID_NOOP("id-health-bpm"), "%1 bpm").arg(alert.bpm);
    //% "Your heart rate rose above %1 bpm while you appeared to be inactive from %2 - %3."
    const QString rose = text(QT_TRID_NOOP("id-health-hr-high-detail"),
                              "Your heart rate rose above %1 bpm while you appeared to be inactive from %2 - %3.");
    //% "Your heart rate fell below %1 bpm while you appeared to be inactive from %2 - %3."
    const QString fell = text(QT_TRID_NOOP("id-health-hr-low-detail"),
                              "Your heart rate fell below %1 bpm while you appeared to be inactive from %2 - %3.");
    const QString detail = (high ? rose : fell).arg(alert.threshold).arg(clockText(alert.startMs))
            .arg(clockText(alert.endMs));
    n.hints.insert("x-asteroid-category", QStringLiteral("health-alert"));
    n.hints.insert("x-asteroid-detail", detail);
    n.hints.insert("x-asteroid-health", json({{"type", high ? "hr-high" : "hr-low"}, {"bpm", alert.bpm},
                                              {"threshold", alert.threshold}, {"start", (double)alert.startMs},
                                              {"end", (double)alert.endMs}}));
    n.hints.insert("urgency", 2);
    n.replaces = m_alertId; // stock posts every alert under one notification id
    m_alertId = post(n);
}

// stock/STRINGS.tsv 0x7f140199 (Reminder to move), plural 0x7f12000a, 0x7f140193 (Goal complete!), 0x7f140196
// (Hourly Activity), 0x7f140197 (%1$s hr). The launcher draws them full screen from x-asteroid-health (E-06); the
// title and text are stock's notification of a celebration (trj.java:166-176: the label as the title if there is
// one, else the title; the text the title, a new line and the value): HourlyReminderToMoveMessage "Reminder to move"
// / "N steps left\nthis hour", HourlyReminderToMoveCelebration "Reminder to move" / "Goal complete!\n250/250"
// (its b(): current/max), DailyReminderToMoveCelebration "Hourly Activity" / "Goal complete!\n9 hr"
void HealthMonitor::postMove(const MoveEvent &event)
{
    using HealthText::text;
    HealthNotification n;
    n.icon = QStringLiteral("ios-walk");
    n.hints.insert("x-asteroid-category", QStringLiteral("celebration"));
    n.hints.insert("transient", true);
    //% "Reminder to move"
    const QString title = text(QT_TRID_NOOP("id-health-move-title"), "Reminder to move");
    //% "Goal complete!"
    const QString complete = text(QT_TRID_NOOP("id-health-goal-complete"), "Goal complete!");
    switch (event.kind) {
    case MoveEvent::Remind:
        n.summary = title;
        //% "%n steps left\nthis hour"
        n.body = HealthText::textN(QT_TRID_N_NOOP("id-health-steps-left"), event.stepsLeft, "%n step left\nthis hour",
                                   "%n steps left\nthis hour");
        n.hints.insert("x-asteroid-health", json({{"type", "reminder-to-move"}, {"steps", event.steps},
                                                  {"goal", HOURLY_STEPS}}));
        break;
    case MoveEvent::HourComplete:
        n.summary = title;
        n.body = complete + QStringLiteral("\n%1/%1").arg(HOURLY_STEPS);
        n.hints.insert("x-asteroid-health", json({{"type", "reminder-to-move-complete"}, {"steps", event.steps},
                                                  {"goal", HOURLY_STEPS}}));
        break;
    case MoveEvent::DayComplete:
        //% "Hourly Activity"
        n.summary = text(QT_TRID_NOOP("id-health-hourly-activity"), "Hourly Activity");
        //% "%1 hr"
        n.body = complete + QLatin1Char('\n') + text(QT_TRID_NOOP("id-health-hours"), "%1 hr").arg(event.hoursMet);
        n.hints.insert("x-asteroid-health", json({{"type", "hourly-activity"}, {"hours", event.hoursMet}}));
        // a goal achieved: the launcher's screen vibrates (twice), the post does not
        n.hints.insert("x-nemo-feedback", QString());
        break;
    }
    postCelebration(n);
}

// The main goal reached (stock's GoalAchievedCelebration of its type, tui.java:505-580): the names 0x7f14019a Steps,
// 0x7f140188 Calories, 0x7f14018a Distance, 0x7f140191 Floors, 0x7f140186 Active Zone Min; the values "%1$s cal"
// 0x7f140187, "%1$s floors" 0x7f140190, "%1$s Zone Mins" 0x7f140185, the distance in km (the launcher writes it in
// the locale's unit). The title and text as stock's notification of it (trj.java:166-176): "Steps" /
// "Goal complete!\n10,000". No vibration of the post's: the launcher's screen vibrates.
void HealthMonitor::postGoal(const MainGoal &goal, double value)
{
    using HealthText::text;
    HealthNotification n;
    QString valueText;
    switch (goal.type) {
    case GoalType::Steps:
        //% "Steps"
        n.summary = text(QT_TRID_NOOP("id-health-steps"), "Steps");
        valueText = QLocale::c().toString((int)std::lround(value));
        break;
    case GoalType::Calories:
        //% "Calories"
        n.summary = text(QT_TRID_NOOP("id-health-calories"), "Calories");
        //% "%1 cal"
        valueText = text(QT_TRID_NOOP("id-health-cal"), "%1 cal").arg(QLocale::c().toString((int)std::lround(value)));
        break;
    case GoalType::Distance:
        //% "Distance"
        n.summary = text(QT_TRID_NOOP("id-health-distance"), "Distance");
        //% "%1 km"
        valueText = text(QT_TRID_NOOP("id-health-km"), "%1 km").arg(QLocale::c().toString(value / 1000.0, 'f', 2));
        break;
    case GoalType::Floors:
        //% "Floors"
        n.summary = text(QT_TRID_NOOP("id-health-floors-title"), "Floors");
        //% "%1 floors"
        valueText = text(QT_TRID_NOOP("id-health-floors"), "%1 floors").arg((int)std::lround(value));
        break;
    case GoalType::Azm:
        //% "Active Zone Min"
        n.summary = text(QT_TRID_NOOP("id-health-azm"), "Active Zone Min");
        //% "%1 Zone Mins"
        valueText = text(QT_TRID_NOOP("id-health-zone-mins"), "%1 Zone Mins").arg((int)std::lround(value));
        break;
    }
    //% "Goal complete!"
    n.body = text(QT_TRID_NOOP("id-health-goal-complete"), "Goal complete!") + QLatin1Char('\n') + valueText;
    n.icon = QStringLiteral("ios-trophy");
    n.hints.insert("x-asteroid-category", QStringLiteral("celebration"));
    n.hints.insert("transient", true);
    n.hints.insert("x-nemo-feedback", QString());
    n.hints.insert("x-asteroid-health", json({{"type", "goal"}, {"metric", goal.metric()},
                                              {"value", std::round(value * 100) / 100}, {"goal", goal.target}}));
    postCelebration(n);
}

// stock/STRINGS.tsv 0x7f140059 / 0x7f140057 (detected, Start tracking?); the brand in the first one is ours.
// ('Finished working out?', 0x7f1409c0, is the Exercise app's own: autoExercise.h)
void HealthMonitor::postExercise(const ExerciseOffer &offer)
{
    HealthNotification n;
    //% "Activity detected"
    n.summary = HealthText::text(QT_TRID_NOOP("id-health-activity-detected"), "Activity detected");
    //% "Start tracking?"
    n.body = HealthText::text(QT_TRID_NOOP("id-health-start-tracking"), "Start tracking?");
    n.icon = QStringLiteral("ios-walk");
    n.hints.insert("x-asteroid-category", QStringLiteral("exercise-detect"));
    n.hints.insert("transient", true);
    n.hints.insert("x-asteroid-exercise", json({{"action", "start"}, {"activity", offer.activity},
                                                {"since", (double)offer.sinceMs}}));
    post(n);
    m_offer = offer;
    m_offeredMs = now();
    emit exerciseOffered(QStringLiteral("start"), offer.activity, offer.sinceMs);
}

MainGoal HealthMonitor::mainGoal() const
{
    QVariantList values;
    for (MDConfItem *item : m_goalItems)
        values << item->value();
    return mainGoalFrom(values);
}

// The main goal reached (R5 E-05 / E-18): once a day, of the type the phone set (stock: the account's primary goal,
// its passive goal of the hub, PrimaryGoalReceiver -> tui.java j()). Steps from the minutes in memory; another type
// from the day's summary (the day files and the heart rate database), at most every GOAL_CHECK_MS and at the
// monitor's own wake
void HealthMonitor::checkGoal()
{
    const qint64 t = now();
    const QDate today = QDateTime::fromMSecsSinceEpoch(t).date();
    if (m_settings->value("HealthPrivate/goalDay", -1).toLongLong() == today.toJulianDay())
        return;
    const MainGoal goal = mainGoal();
    double value = 0;
    if (goal.type == GoalType::Steps) {
        const qint64 midnight = QDateTime(today, QTime(0, 0)).toMSecsSinceEpoch();
        value = m_series.stepsBetween(midnight, t);
    } else {
        if (!m_inWake && m_goalCheckMs > 0 && t >= m_goalCheckMs && t - m_goalCheckMs < GOAL_CHECK_MS)
            return;
        m_goalCheckMs = t;
        value = goalValue(goal.type, summarizeDay(today, m_profile, effectiveRestingHr(m_profile, m_restingEstimate), t));
    }
    if (value < goal.target)
        return;
    m_settings->setValue("HealthPrivate/goalDay", today.toJulianDay());
    emit goalReached(goal.metric(), (int)std::lround(value), (int)std::lround(goal.target));
    postGoal(goal, value);
}

// Last night's sleep, once it is over: when data is looked at after the watch has been still for
// SLEEP_QUIET_MS past the night's last asleep minute; stored in health.db (sleep_session and its stage timeline in
// sleep_stage, the estimate of version 2: SLEEP_ALGO_V2, sleepEstimator.h). Later that day, a nap that is over (as
// long quiet) is stored with it: the morning's main sleep as it was stored, the day's naps (stock lists them:
// "Naps", "Nap from %1$s to %2$s"); nothing is written when no nap is new. A day of naps and no night keeps its naps
// too (R5 E-30). Tried at most every SLEEP_RETRY_MS (05:00-22:00: the evening's naps up to 20:00 are over and quiet
// by then), on data deliveries only.
void HealthMonitor::checkSleep()
{
    const qint64 t = now();
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(t);
    // to 22:00: an evening nap of the night's window (up to 20:00, R5 E-30) is over and quiet by then
    if (local.time().hour() < 5 || local.time().hour() >= SLEEP_WINDOW_END_HOUR + 2)
        return;
    const QString today = local.date().toString(Qt::ISODate);
    if (m_sleepAttemptMs != 0 && t >= m_sleepAttemptMs && t - m_sleepAttemptMs < SLEEP_RETRY_MS)
        return;
    m_sleepAttemptMs = t;
    const bool stored = m_settings->value("HealthPrivate/sleepStored", QString()).toString() == today;
    const int rhr = effectiveRestingHr(m_profile, m_restingEstimate);
    SleepNight night = estimateNight(local.date(), m_profile, rhr);
    // no night, but naps (R5 E-30): they are kept too, as a night's are
    if (!night.valid && night.naps.isEmpty())
        return;
    if (night.valid && t - night.main.endMs < SLEEP_QUIET_MS)
        return;
    // the naps that are over
    QList<SleepSession> naps;
    for (const SleepSession &nap : night.naps)
        if (t - nap.endMs >= SLEEP_QUIET_MS)
            naps.append(nap);
    auto setNaps = [](SleepNight &n, QList<SleepSession> list) {
        std::sort(list.begin(), list.end(),
                  [](const SleepSession &a, const SleepSession &b) { return a.startMs < b.startMs; });
        n.naps = list;
        n.napMin = 0;
        for (const SleepSession &s : list)
            n.napMin += s.asleepMin;
    };
    if (stored) {
        SleepNight inDb;
        // a stored day of naps alone has no night: its naps still come back
        if (!m_store || (!HealthDb::readNight(m_store->path(), local.date(), &inDb) && inDb.naps.isEmpty()))
            return;
        QList<SleepSession> all = inDb.naps;
        bool added = false;
        for (const SleepSession &nap : naps) {
            bool known = false;
            for (const SleepSession &old : inDb.naps)
                known = known || (nap.startMs < old.endMs && old.startMs < nap.endMs);
            if (!known) {
                all.append(nap);
                added = true;
            }
        }
        if (!added)
            return;
        setNaps(inDb, all);
        m_store->storeNight(inDb, inDb.algo.isEmpty() ? QString::fromLatin1(SLEEP_ALGO_V2) : inDb.algo);
        return;
    }
    setNaps(night, naps);
    if (!night.valid && night.naps.isEmpty())
        return; // the naps are not over yet
    if (m_store && m_store->storeNight(night, QString::fromLatin1(SLEEP_ALGO_V2))) {
        m_settings->setValue("HealthPrivate/sleepStored", today);
        // the day's resting heart rate: once, over the 24 hours up to the night's end
        if (night.valid) {
            storeRestingHr(local.date(), night.main.endMs);
            postMorningBrief(night);
        }
    }
}

// The Morning Brief's wake-up (R5 E-10). Stock starts it at the morning's wake-up (MorningBriefWakeUpReceiver; the
// triggers BTM, ABTM, CHIME_NOTIFICATION: MorningBriefBtmBasedScheduler$Companion$MorningBriefTriggerEvent), not only
// when Bedtime mode ends. The launcher posts "Start your day" when Bedtime ends in the morning (modes.cpp
// postMorningBrief); this posts the same notification when the morning's night is stored (the wake-up as this watch
// knows it: 30 minutes after the last asleep minute, at the next look; guess) and Bedtime was not on in the night or
// now (then the launcher's comes at its end). Once a day, from 04:00 to 12:00 (the launcher's morning, modescore.h).
// Texts: 0x7f140afa "Start your day", 0x7f140afb "Good morning! Here's how your day is shaping up."
void HealthMonitor::postMorningBrief(const SleepNight &night)
{
    const qint64 t = now();
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(t);
    if (local.time().hour() < MORNING_FROM_HOUR || local.time().hour() >= MORNING_TO_HOUR)
        return;
    if (m_settings->value("HealthPrivate/briefDay", -1).toLongLong() == local.date().toJulianDay())
        return;
    if (m_bedtimeItem->value(false).toBool())
        return;
    for (const QPointF &r : loadStateRecords(QStringLiteral("bedtime"), night.main.startMs - 3600 * 1000, t))
        if (r.y() >= 0.5)
            return;
    m_settings->setValue("HealthPrivate/briefDay", local.date().toJulianDay());
    HealthNotification n;
    //% "Start your day"
    n.summary = HealthText::text(QT_TRID_NOOP("id-health-brief-title"), "Start your day");
    //% "Good morning! Here’s how your day is shaping up."
    n.body = HealthText::text(QT_TRID_NOOP("id-health-brief-text"), "Good morning! Here’s how your day is shaping up.");
    n.icon = QStringLiteral("ios-sunny");
    n.hints.insert("x-eos-app", QStringLiteral("asteroid-health"));
    n.hints.insert("x-eos-open-page", QStringLiteral("morning"));
    n.hints.insert("x-nemo-feedback", QStringLiteral("notif_normal"));
    n.hints.insert("urgency", 1);
    post(n);
}

// Bedtime mode as the sleep estimate's hint (R5 E-30): its changes in bedtime/ (1 on, 0 off), a record only when the
// state is another than the last one logged. The launcher switches it while the system is awake (the user, its
// schedule), so the dconf change reaches the daemon then: no wake for it
void HealthMonitor::recordBedtime()
{
    const qint64 t = now();
    const int state = m_bedtimeItem->value(false).toBool() ? 1 : 0;
    const QList<QPointF> last = loadStateRecords(QStringLiteral("bedtime"), t + 1, t + 1, 30);
    if (last.isEmpty() ? state == 0 : (int)last.last().y() == state)
        return;
    setupFilePath(QStringLiteral("bedtime"));
    fileAddRecord(QStringLiteral("bedtime"), QString::number(state), QDateTime::fromMSecsSinceEpoch(t));
}

void HealthMonitor::setSkinTempAvailable(bool available)
{
    m_skinTempAvailable = available;
    // a mark left by a run that ended while it was sampling: not sampling now
    if (m_store && m_store->meta(QStringLiteral("skin_temp.on_ms")).toLongLong() > 0) {
        m_store->setMeta(QStringLiteral("skin_temp.on_ms"), 0LL);
        m_store->setMeta(QStringLiteral("skin_temp.off_ms"), (qlonglong)now());
    }
}

// The hub's skin temperature runs inside a night's sleep only (skinTempWanted); decided at every look, so it begins
// with the first delivery that finds the wearer asleep and ends with the first that finds them up
void HealthMonitor::updateSkinTemp()
{
    if (!m_skinTempAvailable)
        return;
    const qint64 t = now();
    const bool want = skinTempWanted(m_skinTempOn, m_series, t, worn(t), effectiveRestingHr(m_profile, m_restingEstimate),
                                     m_profile.maxHr());
    if (want == m_skinTempOn)
        return;
    m_skinTempOn = want;
    if (m_store) {
        m_store->setMeta(QStringLiteral("skin_temp.on_ms"), want ? (qlonglong)t : 0LL);
        if (!want)
            m_store->setMeta(QStringLiteral("skin_temp.off_ms"), (qlonglong)t);
    }
    if (!want) {
        m_skinTempOffMs = t;
        // the hub's last batch comes within the plugin's settle time: the night is looked at after it
        QTimer::singleShot(SKIN_SETTLE_MS, this, &HealthMonitor::checkSkinTemp);
    }
    qDebug() << "skin temperature:" << (want ? "sampling (asleep)" : "not sampling");
    emit skinTempSamplingChanged(want);
}

// The morning's skin temperature (skinTemp.h): the main sleep stored, the sampling off and its last batch in; once a
// morning (a row in skin_temp_night marks it done, also one without a value)
void HealthMonitor::checkSkinTemp()
{
    if (!m_skinTempAvailable || !m_store || m_skinTempOn)
        return;
    const qint64 t = now();
    if (m_skinTempOffMs > 0 && t >= m_skinTempOffMs && t - m_skinTempOffMs < SKIN_SETTLE_MS)
        return;
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(t);
    if (local.time().hour() < 5 || local.time().hour() >= 20)
        return;
    const QDate today = local.date();
    if (m_skinTempDone == today)
        return;
    SleepNight night;
    if (!HealthDb::readNight(m_store->path(), today, &night))
        return; // not stored yet
    const QList<SkinTempNight> nights =
            HealthDb::readSkinTempNights(m_store->path(), today.addDays(-SKIN_BASELINE_DAYS), today);
    for (const SkinTempNight &n : nights) {
        if (n.wakeDate == today) {
            m_skinTempDone = today;
            return;
        }
    }
    const QVector<QPair<qint64, double>> minutes =
            HealthDb::readSkinTemp(m_store->path(), night.main.startMs, night.main.endMs);
    const QList<QPointF> offBody =
            loadStateRecords(QStringLiteral("offBody"), night.main.startMs, night.main.endMs);
    const SkinTempNight n = skinTempNight(today, night.main, minutes, offBody, nights);
    if (m_store->storeSkinTempNight(n))
        m_skinTempDone = today;
    qDebug() << "skin temperature: the night has" << n.minutes << "minutes," << (n.hasMean ? "a value" : "no value")
             << "," << n.nights << "nights before it" << (n.hasVariation ? "(a variation)" : "");
}

qint64 HealthMonitor::nightEndOf(const QDate &wakeDate) const
{
    SleepNight n;
    return m_store && HealthDb::readNight(m_store->path(), wakeDate, &n) ? n.main.endMs : 0;
}

int HealthMonitor::restingOf(const QDate &day, qint64 nightEndMs)
{
    return estimateRestingHrForDay(day, nightEndMs);
}

void HealthMonitor::storeRestingHr(const QDate &day, qint64 nightEndMs)
{
    if (!m_store)
        return;
    DailyRecord row;
    if (HealthDb::readDaily(m_store->path(), day, &row) && row.restingHr > 0)
        return; // estimated once a day
    const int bpm = restingOf(day, nightEndMs);
    if (bpm > 0 && m_store->setDailyRestingHr(day, bpm))
        m_restingEstimate = estimatedRestingHr(now());
}

bool HealthMonitor::dayTotals(const QDate &day, DailyTotals *totals)
{
    if (!hasDataOn(day))
        return false;
    loadProfile();
    // the rate in force now (the import gives the days before it their rates first)
    const int rhr = effectiveRestingHr(m_profile, estimatedRestingHr(now()));
    const DaySummary d = summarizeDay(day, m_profile, rhr, now());
    totals->steps = d.steps;
    totals->kcal = d.energyKcal;
    totals->distanceM = d.distanceM;
    totals->floors = d.floors;
    totals->azm = d.zones.azm;
    totals->hoursMet = d.hourly.hoursMet;
    totals->cardioLoad = d.cardioLoad; // R5 E-09
    return true;
}

// The days that are over get their row in health.db: on the first delivery after midnight (stock's Health
// Services flush the day before midnight; this watch has no wake for it, so it is the first time data is looked
// at), and on the days missed while the daemon was not running. Not before the one-time import is through,
// which closes the days before it.
void HealthMonitor::closeDays()
{
    if (!m_store || !importDone())
        return;
    const QDate yesterday = QDateTime::fromMSecsSinceEpoch(now()).date().addDays(-1);
    if (m_closedThrough.isValid() && m_closedThrough >= yesterday)
        return;
    QDate from = m_store->lastClosedDay();
    from = from.isValid() ? from.addDays(1) : yesterday;
    from = qMax(from, yesterday.addDays(1 - CLOSE_DAYS_BACK));
    for (QDate d = from; d <= yesterday; d = d.addDays(1)) {
        DailyTotals totals;
        if (!dayTotals(d, &totals))
            continue;
        // a day without a night stored for it gets its resting heart rate now, over the day itself
        storeRestingHr(d, nightEndOf(d));
        m_store->setDailyTotals(d, totals, now());
    }
    m_closedThrough = yesterday;
    backfillCardioLoad(yesterday);
}

// The cardio load of the days closed before it was computed (R5 E-09): once, the 28 days a target looks at, so the
// target is there at once and not after a week of new days
void HealthMonitor::backfillCardioLoad(const QDate &yesterday)
{
    if (!m_store || m_store->meta(QStringLiteral("cardio_load.backfill")).toLongLong() == 1)
        return;
    const QList<DailyRecord> rows =
            HealthDb::readDailyRange(m_store->path(), yesterday.addDays(1 - CARDIO_HISTORY_DAYS), yesterday);
    for (const DailyRecord &r : rows) {
        if (!r.closed || r.totals.cardioLoad >= 0)
            continue;
        DailyTotals totals;
        if (dayTotals(r.date, &totals))
            m_store->setDailyTotals(r.date, totals, r.closedMs);
    }
    m_store->setMeta(QStringLiteral("cardio_load.backfill"), 1LL);
}
