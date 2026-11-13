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
#include <QFileSystemWatcher>
#include <QObject>
#include <QVariantMap>

/*
 * The Sleep card and detail's data (health-fitness-17): the night the daemon estimated and stored in the
 * morning (sleep/<wake date>.json), or one estimated now from the logs if the daemon has not yet.
 *
 *   { valid, stored, wakeDate, main: { startMs, endMs, durationMin, asleepMin, awakeMin, remMin, lightMin,
 *     deepMin, awakePct, remPct, lightPct, deepPct, avgHr }, naps: [ same ], napMin, score, scoreLabel,
 *     goalMin, goalMet }
 *
 * valid false: "No recent data. Wear your watch to sleep."
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

private:
    QFileSystemWatcher *m_fileWatcher;
};

#endif // SLEEPDATALOADER_H
