/*
 * Copyright (C) 2026 eos
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
#include <QTime>

#include "../common.h"
#include "../store/healthDb.h"
#include "healthData.h"
#include "restingHr.h"

namespace Health {

int restingHrOfMinutes(const MinuteSeries &series, qint64 fromMs, qint64 toMs, const QList<QPair<qint64, qint64>> &excluded)
{
    QVector<double> rates;
    const qint64 first = MinuteSeries::minuteOf(fromMs), end = MinuteSeries::minuteOf(toMs);
    for (auto it = series.data().lowerBound(first); it != series.data().end() && it.key() < end; ++it) {
        if (it->hr <= 0)
            continue; // not worn
        bool still = true;
        for (int back = 0; still && back <= RESTING_STILL_MIN; back++)
            still = series.at(it.key() - back).steps == 0;
        if (!still)
            continue;
        const qint64 ms = MinuteSeries::msOf(it.key());
        bool inWorkout = false;
        for (const QPair<qint64, qint64> &r : excluded)
            inWorkout = inWorkout || (ms + 60000 > r.first && ms < r.second);
        if (!inWorkout)
            rates.append(it->hr);
    }
    if (rates.size() < RESTING_MIN_MINUTES)
        return 0;
    std::sort(rates.begin(), rates.end());
    const int n = rates.size();
    const double median = n % 2 ? rates[n / 2] : (rates[n / 2 - 1] + rates[n / 2]) / 2.0;
    return qBound(30, (int)std::lround(median), 120);
}

QPair<qint64, qint64> restingWindow(const QDate &day, qint64 nightEndMs)
{
    if (nightEndMs > 0)
        return {nightEndMs - 24LL * 3600 * 1000, nightEndMs};
    return {QDateTime(day, QTime(0, 0)).toMSecsSinceEpoch(), QDateTime(day.addDays(1), QTime(0, 0)).toMSecsSinceEpoch()};
}

int estimateRestingHrForDay(const QDate &day, qint64 nightEndMs)
{
    const QPair<qint64, qint64> w = restingWindow(day, nightEndMs);
    MinuteSeries series;
    // the steps of the days the window and the still minutes before it touch
    const QDate firstDay = QDateTime::fromMSecsSinceEpoch(w.first - (RESTING_STILL_MIN + 1) * 60000LL).date();
    const QDate lastDay = QDateTime::fromMSecsSinceEpoch(w.second - 1).date();
    for (QDate d = firstDay; d <= lastDay; d = d.addDays(1))
        loadSteps(series, d);
    loadHeartRate(series, w.first, w.second);
    // the workouts on in the window (one may have begun before it)
    QList<QPair<qint64, qint64>> workouts;
    for (const WorkoutRecord &r : HealthDb::readWorkouts(healthDbPath(), w.first - 24LL * 3600 * 1000, w.second)) {
        const qint64 end = r.endMs > 0 ? r.endMs : w.second;
        if (end > w.first)
            workouts.append({r.startMs, end});
    }
    return restingHrOfMinutes(series, w.first, w.second, workouts);
}

} // namespace Health
