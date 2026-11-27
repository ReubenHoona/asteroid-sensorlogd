/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HEALTHMETRICSLOADER_H
#define HEALTHMETRICSLOADER_H

#include <QDate>
#include <QFileSystemWatcher>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <MDConfItem>

#include "../metrics/healthProfile.h"

/*
 * The shared metrics module of the Health app and the launcher's tiles and complications (health-fitness-11):
 * distance, energy burned, floors, heart rate zones, Active Zone Minutes and hourly activity for a day, from
 * what sensorlogd has logged and the Health profile (metrics/healthMetrics.h). Computed when asked; no sensor is
 * woken. The profile is read from dconf at the first call that needs it and kept, its keys watched: a change of
 * one (Settings, the phone) is seen at the next call and told by profileChanged and dataChanged.
 *
 *   summary(date): { steps, distanceM, energyKcal, floors, zoneLight, zoneModerate, zoneVigorous, zonePeak,
 *                    azm, hoursMet, hoursTotal, hourSteps[24], restingHr, maxHr, strideM }
 *   zoneDays(from, to): each day's { date, zoneLight, zoneModerate, zoneVigorous, zonePeak, azm }, the oldest
 *                    first, as summary() has them, from one read (a week of Active Zone Minutes)
 *   daily(date):   the day's row in health.db, as the daemon closed it after midnight (store/healthDb.h):
 *                  { date, restingHr, closed, steps, kcal, distanceM, floors, azm, hoursMet, closedMs, seq }; {}
 *                  if the day has none
 *   skinTemp(date): the morning's skin temperature variation as stock's card shows it (metrics/skinTemp.h):
 *                  { state: noData, calibrating, tracking, analyzing, inRange, aboveRange, belowRange; nightsLeft;
 *                  wakeDate; measured; minutes; hasVariation; devC, loC, hiC (deg C); nights }
 *   skinTempWeek(date): the seven mornings up to it, the oldest first: { date, hasVariation, devC, loC, hiC, state }
 *   mainGoal():     the main goal (metrics/goals.h, R5 E-18): { type: steps, calories, distance, floors or azm;
 *                  target } (its keys watched: dataChanged)
 *   cardioLoad(date): the day's cardio load and its target (healthMetrics.h, R5 E-09): { load (today up to now),
 *                  hasTarget, low, high, daysLeft (days with a load still needed for a target), state: below, in,
 *                  above or "" (no target), week: the seven days up to it, the oldest first, [{ date, load }] (-1:
 *                  none) }
 *
 * dataChanged: a step day file changed, or the daemon wrote to health.db (its D-Bus signal HealthChanged).
 */
class HealthMetricsLoader : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int stepGoal READ stepGoal NOTIFY dataChanged)
    Q_PROPERTY(int azmWeeklyGoal READ azmWeeklyGoal CONSTANT)
    Q_PROPERTY(int hourlyStepGoal READ hourlyStepGoal CONSTANT)

public:
    explicit HealthMetricsLoader();

    int stepGoal() const; // /org/asteroidos/ux/steps-goal, 10000
    int azmWeeklyGoal() const;
    int hourlyStepGoal() const;

    Q_INVOKABLE QVariantMap summary(QDate date);
    Q_INVOKABLE double distanceM(QDate date);
    Q_INVOKABLE int energyKcal(QDate date);
    Q_INVOKABLE int floors(QDate date);
    // Active Zone Minutes of a day, and of the week (Monday to the date: zoneDays())
    Q_INVOKABLE int azm(QDate date);
    Q_INVOKABLE int azmWeek(QDate date);
    // the zone minutes and Active Zone Minutes of each day from `from` to `to` (both included, 31 days at most;
    // today up to now): [{ date, zoneLight, zoneModerate, zoneVigorous, zonePeak, azm }], the oldest first, the
    // numbers of summary(), from one read of the heart rate database and one of the resting rate
    Q_INVOKABLE QVariantList zoneDays(QDate from, QDate to);
    // { hoursMet, hoursTotal, hourSteps[24], stepsPerHour, and the day's longest inactive span (R5 E-21, stock's
    // "Most inactive today", healthMetrics.h longestInactive): inactiveStartMs, inactiveEndMs, inactiveMin (0: none) }
    Q_INVOKABLE QVariantMap hourly(QDate date);

    // the heart rate zones for the profile: [{ name, startBpm, endBpm }], Light first ("Heart rate zones based
    // on age and your N bpm RHR")
    Q_INVOKABLE QVariantList zones();
    // 0 Light, 1 Moderate, 2 Vigorous, 3 Peak
    Q_INVOKABLE int zoneForBpm(int bpm);
    // the resting heart rate in force (the profile's, else the mean of the last 7 days' daily estimates, else 60)
    Q_INVOKABLE int restingHr();
    // the day's resting heart rate, estimated once a day by the daemon (metrics/restingHr.h); -1: none
    Q_INVOKABLE int restingHrOf(QDate date);
    Q_INVOKABLE int maxHr();
    // the sleep goal in force, minutes a night (the profile's: /org/asteroidos/ux/sleep-goal, 60 to 1440, else 480;
    // a change is a profileChanged): the Health app's Sleep card and page show it (R5 E-22)
    Q_INVOKABLE int sleepGoal();
    // the thresholds of the high and low heart rate alerts while the user has set none: stock's 120 and 50
    // (what the Health settings show then)
    Q_INVOKABLE int defaultHighBpm();
    Q_INVOKABLE int defaultLowBpm();
    Q_INVOKABLE void refresh() { emit dataChanged(); }

    // the main goal: { type, target } (metrics/goals.h)
    Q_INVOKABLE QVariantMap mainGoal();
    // the day's cardio load and its target (R5 E-09)
    Q_INVOKABLE QVariantMap cardioLoad(QDate date);
    // the day's row in health.db ({} if none)
    Q_INVOKABLE QVariantMap daily(QDate date);
    // the skin temperature variation of a morning (M4 WO-13) and of the seven mornings up to it
    Q_INVOKABLE QVariantMap skinTemp(QDate date);
    Q_INVOKABLE QVariantList skinTempWeek(QDate date);

    // The energy formula of the Health numbers (metrics/healthMetrics.h), for the Exercise app's workouts: the
    // energy of a time is restingKcal(its length) plus minuteKcal() of each of its minutes (energyKcal())
    // the active kcal of one minute with these steps and this mean heart rate (0: none), for the profile
    Q_INVOKABLE double minuteKcal(int steps, double bpm);
    // the resting energy of a time of this length
    Q_INVOKABLE double restingKcal(double durationMs);

signals:
    void dataChanged();
    // a key of the Health profile (metrics/healthProfile.h: gender, height, weight, birth date, age, resting heart
    // rate, the sleep goal) changed: every day's numbers may be others (dataChanged follows)
    void profileChanged();

private slots:
    void onHealthChanged(qlonglong)
    {
        m_energyAt = 0;
        emit dataChanged();
    }
    void onProfileKeyChanged();

private:
    // the Health profile, kept: read from the watched keys at the first call and again after one of them changed
    // or on a new day (the age is counted to today)
    const Health::HealthProfile &profile();
    // minuteKcal(), restingKcal(): the profile and the resting heart rate in force, taken again after a minute or
    // a write of the daemon (a workout asks every second, a batch of readings many times at once)
    void takeEnergyContext();

    QFileSystemWatcher *m_fileWatcher;
    QList<MDConfItem *> m_profileKeys; // HealthProfile::keys(), made at the first profile()
    QList<MDConfItem *> m_goalKeys;    // goals.h mainGoalKeys(), made at the first mainGoal()
    Health::HealthProfile m_profile;
    QDate m_profileDay; // the day m_profile was read for; invalid: to be read
    Health::HealthProfile m_energyProfile;
    int m_energyResting = 60;
    qint64 m_energyAt = 0;
};

#endif // HEALTHMETRICSLOADER_H
