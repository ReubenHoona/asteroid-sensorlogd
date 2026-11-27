/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SLEEPDATALOADER_H
#define SLEEPDATALOADER_H

#include <QDate>
#include <QObject>
#include <QVariantMap>

/*
 * The Sleep card and detail's data (health-fitness-17): the night the daemon estimated and stored in the
 * morning (health.db, sleep_session; a night of a sensorlogd from before it: sleep/<wake date>.json until the
 * one-time import took it in), or one estimated now from the logs if the daemon has not yet.
 *
 *   { valid, stored, wakeDate, main: { startMs, endMs, durationMin, asleepMin, awakeMin, remMin, lightMin,
 *     deepMin, awakePct, remPct, lightPct, deepPct, avgHr, stages: [ { startMs, endMs, stage, name } ] },
 *     naps: [ same ], napMin, score, scoreLabel, goalMin, goalMet }
 *
 * stages: the timeline of the session (M4 WO-06), stage 0 Awake, 1 Light, 2 Deep, 3 REM (name: stock's word);
 * empty for a night stored by an older sensorlogd.
 *
 * valid false: "No recent data. Wear your watch to sleep."
 * dataChanged: the daemon wrote to health.db (its D-Bus signal HealthChanged).
 */
class SleepDataLoader : public QObject
{
    Q_OBJECT
public:
    explicit SleepDataLoader();
    // the night that ended on that day
    Q_INVOKABLE QVariantMap forDate(QDate wakeDate);
    // the latest night: ending today if there is one, else yesterday
    Q_INVOKABLE QVariantMap lastNight();

signals:
    void dataChanged();

private slots:
    void onHealthChanged(qlonglong) { emit dataChanged(); }
};

#endif // SLEEPDATALOADER_H
