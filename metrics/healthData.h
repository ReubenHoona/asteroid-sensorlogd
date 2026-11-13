/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HEALTHDATA_H
#define HEALTHDATA_H

#include <QDate>
#include <QList>
#include <QPointF>

#include "healthMetrics.h"
#include "sleepEstimator.h"

namespace Health {

/*
 * What sensorlogd has logged, as the series of healthMetrics.h: the step day files (stepCounter/), the heart
 * rate database (heartrate.db) and the barometer day files (barometer-raw/), read when asked.
 */

// the step interval of the daemon's settings (stepCounter/interval), which the step records are spread by
qint64 stepIntervalMs();

// adds the steps of the day's file to the series
void loadSteps(MinuteSeries &series, const QDate &day);
// adds the heart rate of [fromMs, toMs) from the database (readings of medium accuracy and better)
void loadHeartRate(MinuteSeries &series, qint64 fromMs, qint64 toMs);
// steps and heart rate of the local day
MinuteSeries loadDay(const QDate &day);
// the barometer records of the day (time in seconds, pressure)
QList<QPointF> loadPressure(const QDate &day);

// The resting heart rate estimated from the last `days` days of heart rate readings (0: not enough)
int estimatedRestingHr(qint64 nowMs, int days = 7);

// The night that ended on wakeDate: the stored result (sleep/<wakeDate>.json) if the daemon has written
// one, else estimated now from the logs (stored: false then)
SleepNight loadSleep(const QDate &wakeDate, const HealthProfile &profile, int restingHr, bool *stored = nullptr);
// the night's result, estimated from the logs
SleepNight estimateNight(const QDate &wakeDate, const HealthProfile &profile, int restingHr);
QString sleepFilePath(const QDate &wakeDate);
// writes the result to sleepFilePath() (a temporary file renamed, so a reader never sees half of it)
bool storeSleep(const SleepNight &night);

} // namespace Health

#endif // HEALTHDATA_H
