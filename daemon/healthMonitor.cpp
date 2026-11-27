/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTime>

#include "healthMonitor.h"
#include "sensorPlugins/heartrateSensor.h"
#include "sensorPlugins/stepCounter.h"
#include "../common.h"
#include "../metrics/healthData.h"
#include "../metrics/restingHr.h"

using namespace Health;

static const char *const GOAL_KEY = "/org/asteroidos/ux/steps-goal";
static const char *const TWELVE_HOUR_KEY = "/org/asteroidos/settings/use-12h-format";
static const int DEFAULT_STEPS_GOAL = 10000;
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
    m_goalItem = new MDConfItem(GOAL_KEY, this);
    m_twelveHourItem = new MDConfItem(TWELVE_HOUR_KEY, this);
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
}

void HealthMonitor::batchSettled()
{
    m_settle->start(SETTLE_MS);
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

    // 4. auto-detected exercise
    for (const ExerciseOffer &offer : m_exercise.feed(m_series, t, m_config.exercise, m_workoutActive))
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

void HealthMonitor::armReminderTimer()
{
    m_reminderTimer->stop();
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
    n.hints.insert("x-nemo-feedback", QStringLiteral("notif_strong"));
    return m_poster->post(n);
}

// stock's texts: stock/STRINGS.tsv 0x7f14034c/d (titles), 0x7f1409a0 (N bpm), 0x7f1409a1 / 0x7f1409a3 (detail)
void HealthMonitor::postAlert(const HrAlert &alert)
{
    const bool high = alert.kind == HrAlert::High;
    HealthNotification n;
    n.summary = high ? QStringLiteral("High heart rate") : QStringLiteral("Low heart rate");
    n.body = QStringLiteral("%1 bpm").arg(alert.bpm);
    const QString detail = QStringLiteral("Your heart rate %1 %2 bpm while you appeared to be inactive from %3 - %4.")
            .arg(high ? "rose above" : "fell below").arg(alert.threshold).arg(clockText(alert.startMs))
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
void HealthMonitor::postMove(const MoveEvent &event)
{
    HealthNotification n;
    n.icon = QStringLiteral("ios-walk");
    n.hints.insert("x-asteroid-category", QStringLiteral("celebration"));
    n.hints.insert("transient", true);
    switch (event.kind) {
    case MoveEvent::Remind:
        n.summary = QStringLiteral("Reminder to move");
        n.body = event.stepsLeft == 1 ? QStringLiteral("1 step left\nthis hour")
                                      : QStringLiteral("%1 steps left\nthis hour").arg(event.stepsLeft);
        n.hints.insert("x-asteroid-health", json({{"type", "reminder-to-move"}, {"steps", event.steps},
                                                  {"goal", HOURLY_STEPS}}));
        break;
    case MoveEvent::HourComplete:
        n.summary = QStringLiteral("Reminder to move");
        n.body = QStringLiteral("Goal complete!");
        n.hints.insert("x-asteroid-health", json({{"type", "reminder-to-move-complete"}, {"steps", event.steps},
                                                  {"goal", HOURLY_STEPS}}));
        break;
    case MoveEvent::DayComplete:
        n.summary = QStringLiteral("Hourly Activity");
        n.body = QStringLiteral("%1 hr").arg(event.hoursMet);
        n.hints.insert("x-asteroid-health", json({{"type", "hourly-activity"}, {"hours", event.hoursMet}}));
        break;
    }
    post(n);
}

// stock/STRINGS.tsv 0x7f140059 / 0x7f140057 (detected, Start tracking?); the brand in the first one is ours.
// ('Finished working out?', 0x7f1409c0, is the Exercise app's own: autoExercise.h)
void HealthMonitor::postExercise(const ExerciseOffer &offer)
{
    HealthNotification n;
    n.summary = QStringLiteral("Activity detected");
    n.body = QStringLiteral("Start tracking?");
    n.icon = QStringLiteral("ios-walk");
    n.hints.insert("x-asteroid-category", QStringLiteral("exercise-detect"));
    n.hints.insert("transient", true);
    n.hints.insert("x-asteroid-exercise", json({{"action", "start"}, {"activity", offer.activity},
                                                {"since", (double)offer.sinceMs}}));
    post(n);
    emit exerciseOffered(QStringLiteral("start"), offer.activity, offer.sinceMs);
}

void HealthMonitor::checkGoal()
{
    const QDate today = QDateTime::fromMSecsSinceEpoch(now()).date();
    if (m_settings->value("HealthPrivate/goalDay", -1).toLongLong() == today.toJulianDay())
        return;
    const int goal = qMax(1, m_goalItem->value(DEFAULT_STEPS_GOAL).toInt());
    const qint64 midnight = QDateTime(today, QTime(0, 0)).toMSecsSinceEpoch();
    const int steps = m_series.stepsBetween(midnight, now());
    if (steps < goal)
        return;
    m_settings->setValue("HealthPrivate/goalDay", today.toJulianDay());
    emit goalReached(QStringLiteral("steps"), steps, goal);
}

// Last night's sleep, once it is over: when data is looked at after the watch has been still for
// SLEEP_QUIET_MS past the night's last asleep minute; stored in health.db (sleep_session and its stage timeline in
// sleep_stage, the estimate of version 2: SLEEP_ALGO_V2, sleepEstimator.h). Later that day, a nap that is over (as
// long quiet) is stored with it: the morning's main sleep as it was stored, the day's naps (stock lists them:
// "Naps", "Nap from %1$s to %2$s"); nothing is written when no nap is new. Tried at most every SLEEP_RETRY_MS
// (05:00-20:00), on data deliveries only.
void HealthMonitor::checkSleep()
{
    const qint64 t = now();
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(t);
    if (local.time().hour() < 5 || local.time().hour() >= 20)
        return;
    const QString today = local.date().toString(Qt::ISODate);
    if (m_sleepAttemptMs != 0 && t >= m_sleepAttemptMs && t - m_sleepAttemptMs < SLEEP_RETRY_MS)
        return;
    m_sleepAttemptMs = t;
    const bool stored = m_settings->value("HealthPrivate/sleepStored", QString()).toString() == today;
    const int rhr = effectiveRestingHr(m_profile, m_restingEstimate);
    SleepNight night = estimateNight(local.date(), m_profile, rhr);
    if (!night.valid || t - night.main.endMs < SLEEP_QUIET_MS)
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
        if (!m_store || !HealthDb::readNight(m_store->path(), local.date(), &inDb))
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
    if (m_store && m_store->storeNight(night, QString::fromLatin1(SLEEP_ALGO_V2))) {
        m_settings->setValue("HealthPrivate/sleepStored", today);
        // the day's resting heart rate: once, over the 24 hours up to the night's end
        storeRestingHr(local.date(), night.main.endMs);
    }
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
}
