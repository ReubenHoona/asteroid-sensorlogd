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

/*
 * The shared metrics module of the Health app and the launcher's tiles and complications (health-fitness-11):
 * distance, energy burned, floors, heart rate zones, Active Zone Minutes and hourly activity for a day, from
 * what sensorlogd has logged and the Health profile (metrics/healthMetrics.h). Computed when asked; nothing
 * is kept and no sensor is woken.
 *
 *   summary(date): { steps, distanceM, energyKcal, floors, zoneLight, zoneModerate, zoneVigorous, zonePeak,
 *                    azm, hoursMet, hoursTotal, hourSteps[24], restingHr, maxHr, strideM }
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
    // Active Zone Minutes of a day, and of the week (Monday to the date)
    Q_INVOKABLE int azm(QDate date);
    Q_INVOKABLE int azmWeek(QDate date);
    // { hoursMet, hoursTotal, hourSteps[24], stepsPerHour }
    Q_INVOKABLE QVariantMap hourly(QDate date);

    // the heart rate zones for the profile: [{ name, startBpm, endBpm }], Light first ("Heart rate zones based
    // on age and your N bpm RHR")
    Q_INVOKABLE QVariantList zones();
    // 0 Light, 1 Moderate, 2 Vigorous, 3 Peak
    Q_INVOKABLE int zoneForBpm(int bpm);
    Q_INVOKABLE int restingHr();
    Q_INVOKABLE int maxHr();
    // the default thresholds of the high and low heart rate alerts for the profile (what the Health settings
    // show while the threshold is automatic)
    Q_INVOKABLE int defaultHighBpm();
    Q_INVOKABLE int defaultLowBpm();
    Q_INVOKABLE void refresh() { emit dataChanged(); }

signals:
    void dataChanged();

private:
    QFileSystemWatcher *m_fileWatcher;
};

#endif // HEALTHMETRICSLOADER_H
