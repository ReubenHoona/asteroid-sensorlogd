/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef RESTINGHR_H
#define RESTINGHR_H

#include <QDate>
#include <QList>
#include <QPair>

#include "healthMetrics.h"

namespace Health {

/*
 * The resting heart rate: one definition for everything that shows or uses one (the Today card and its page, the
 * heart rate zones, energy, the sleep estimate, the sync).
 *
 * Stock: "estimated once a day" (Google's heart rate help page), one value a day on the Today card, its page
 * (today, 'Average resting HR' over 'Last 7 days', the line), Health Metrics and Morning Brief
 * (RestingHeartRateDetailsViewModel.java:57); the value comes from the cloud (restingHeartRate/dailySummaries), so
 * its method is not on the watch. Ours, a guess at it, documented as ours:
 *
 *   A resting minute is a minute the watch was worn (it has heart rate readings of medium accuracy or better), with
 *   no step in it or in the RESTING_STILL_MIN minutes before it, and no workout on. Sleep makes up most of them,
 *   the still awake minutes the rest. The day's resting heart rate is the median of the resting minutes' heart
 *   rates over 24 hours, rounded, kept within 30-120; with fewer than RESTING_MIN_MINUTES resting minutes there is
 *   none.
 *
 *   The 24 hours, and when: once for the local day D, in health.db's daily.resting_hr of D, never again after
 *   that. In the morning, when the night that ended on D is stored (the sleep estimate, healthMonitor.cpp), the 24
 *   hours up to the night's end; on a day without such a night, when D is closed (the first delivery after its
 *   midnight), the day D itself.
 *
 * What uses a resting rate takes the one in force: the profile's own if set, else the mean of the daily ones of the
 * last 7 days (estimatedRestingHr(), healthData.h), else 60.
 */

static const int RESTING_STILL_MIN = 10;   // no step in the minute or the 10 before it
static const int RESTING_MIN_MINUTES = 60; // fewer resting minutes: no estimate

// The resting heart rate of the minutes of [fromMs, toMs) of the series (which should have the steps of the
// RESTING_STILL_MIN minutes before fromMs too); the minutes in an excluded range [first, second) (a workout) do
// not count. 0: fewer than RESTING_MIN_MINUTES resting minutes.
int restingHrOfMinutes(const MinuteSeries &series, qint64 fromMs, qint64 toMs,
                       const QList<QPair<qint64, qint64>> &excluded = {});

// The 24 hours day's estimate is made over: up to the end of the night that ended on it (nightEndMs > 0), else the
// local day itself
QPair<qint64, qint64> restingWindow(const QDate &day, qint64 nightEndMs);

// The estimate of the day from what is logged: the step day files, heartrate.db and the workouts of health.db
int estimateRestingHrForDay(const QDate &day, qint64 nightEndMs);

} // namespace Health

#endif // RESTINGHR_H
