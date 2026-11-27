/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SKINTEMP_H
#define SKINTEMP_H

#include <QDate>
#include <QList>
#include <QPair>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include "healthMetrics.h"
#include "sleepEstimator.h"

namespace Health {

/*
 * Skin temperature variation (M4 WO-13). Stock shows, in the morning, how the skin temperature of the last sleep
 * varied from the wearer's own baseline: the Today card "Skin temp variation" with "In range", "Above range" or
 * "Below range" and "Last sleep session" under it, "Wear to sleep N more times" while the baseline is made,
 * "Tracking..." asleep, "Analyzing..." just after (Fitbit aanu.java, aaeh.java, acbf.java o()); the value is the
 * variation from the baseline (SkinTempDataEntity date, value, upper, lower). Stock computes the baseline, the
 * value and the personal range in the cloud; Google's help page: "taken from your wrist while you sleep", "requires
 * 3 nights of data to establish a baseline", a personal range "based on the average of your recent data", "up to 30
 * days of data". Here the watch computes them; the rules below are ours (guesses where marked).
 *
 * Sampling: the sensor hub's skin temperature (AS6221, USF 0x3d) only inside a night's sleep, never in the day.
 * Nothing on the watch knows "asleep" as it happens (the night is estimated in the morning), so the sampling runs
 * from a look at the data (a delivery of the hub's batch, a display on) that finds the wearer asleep-like to one that
 * finds them up (skinTempWanted). What it takes in is used only for the minutes of the night's sleep: the first look
 * comes up to 45 minutes after falling asleep (the batch), so the first part of a night is missed, every night alike.
 *
 * The night's value: the mean of the minute readings of the main sleep's asleep minutes (its timeline without the
 * Awake spans), worn (the off-body records) and within the range a wrist has (SKIN_MIN_C..SKIN_MAX_C), if there are
 * SKIN_MIN_MINUTES of them. The baseline of a night is the mean of the values of the nights before it in the last
 * SKIN_BASELINE_DAYS (at least SKIN_BASELINE_NIGHTS of them), the variation its value minus the baseline, the
 * personal range plus and minus SKIN_RANGE_SD times the spread of those nights' values, at least SKIN_RANGE_MIN_C
 * and at most SKIN_RANGE_MAX_C (guess). The fourth night with values is the first with a variation.
 */

static const int SKIN_BASELINE_NIGHTS = 3;      // Google: 3 nights of data establish the baseline
static const int SKIN_BASELINE_DAYS = 30;       // Google: up to 30 days of data
static const int SKIN_MIN_MINUTES = 60;         // a night with fewer asleep minutes measured has no value (guess)
static const double SKIN_MIN_C = 25.0;          // a minute outside this is not the skin of a wrist (guess)
static const double SKIN_MAX_C = 41.0;
static const double SKIN_RANGE_SD = 2.0;        // the personal range: +- 2 standard deviations (guess)
static const double SKIN_RANGE_MIN_C = 0.5;
static const double SKIN_RANGE_MAX_C = 1.5;

// When the sampling should run (pure; the Health monitor asks at every look). Not on the wrist: no. From
// SKIN_STOP_HOUR to SKIN_FROM_HOUR (the day): no. Not running yet: it starts from SKIN_FROM_HOUR to
// SKIN_START_UNTIL_HOUR when the last SKIN_LOOK_MIN minutes have at most SKIN_START_MAX_STEPS steps and at least
// SKIN_MIN_HR_MINUTES minutes with a heart rate whose mean is within the lower fifth of the heart rate reserve above
// resting (the sleep estimate's asleep rule). Running: it goes on until SKIN_STOP_STEPS steps in the last
// SKIN_LOOK_MIN minutes, or their mean heart rate above SKIN_AWAKE_RESERVE of the reserve (up and about).
static const int SKIN_FROM_HOUR = 20;
static const int SKIN_START_UNTIL_HOUR = 8;
static const int SKIN_STOP_HOUR = 12;
static const int SKIN_LOOK_MIN = 20;
static const int SKIN_START_MAX_STEPS = 10;
static const int SKIN_MIN_HR_MINUTES = 5;
static const double SKIN_ASLEEP_RESERVE = 0.2;
static const int SKIN_STOP_STEPS = 100;
static const double SKIN_AWAKE_RESERVE = 0.35;
bool skinTempWanted(bool running, const MinuteSeries &series, qint64 nowMs, bool worn, int restingHr, int maxHr);

// A night's skin temperature as health.db's skin_temp_night keeps it (one row a morning)
struct SkinTempNight {
    QDate wakeDate;
    qint64 startMs = 0;       // the main sleep
    qint64 endMs = 0;
    int minutes = 0;          // asleep, worn minutes with a reading
    bool hasMean = false;     // at least SKIN_MIN_MINUTES of them
    double meanC = 0;
    int nights = 0;           // nights with a value before it in the baseline's days
    bool hasVariation = false;
    double baselineC = 0;
    double devC = 0;          // meanC - baselineC
    double loC = 0;           // the personal range of devC
    double hiC = 0;
    qint64 seq = 0;
};

// The night of `main` (the night's main sleep with its stage timeline; an empty timeline: all of it asleep) from the
// minute readings (minute start ms, deg C; health.db skin_temp), the off-body records (offBody/: seconds, value) and
// the nights before it (any order; those of the SKIN_BASELINE_DAYS days before wakeDate with a value count)
SkinTempNight skinTempNight(const QDate &wakeDate, const SleepSession &main,
                            const QVector<QPair<qint64, double>> &minutes, const QList<QPointF> &offBody,
                            const QList<SkinTempNight> &earlier);

// What the Health app shows for the morning `date` (stock's states): from the nights of the SKIN_BASELINE_DAYS days
// up to it (any order), whether the sampling runs (trackingSinceMs > 0) and when it last stopped
enum SkinTempState {
    SkinNoData = 0,       // "Wear your watch to sleep"
    SkinCalibrating = 1,  // "Wear to sleep N more times" (nightsLeft)
    SkinTracking = 2,     // "Tracking..."
    SkinAnalyzing = 3,    // "Analyzing..."
    SkinInRange = 4,
    SkinAboveRange = 5,
    SkinBelowRange = 6,
};
static const qint64 SKIN_TRACKING_MAX_MS = 16LL * 3600 * 1000; // a "running" mark older than this is stale
static const qint64 SKIN_ANALYZING_MS = 3600 * 1000;           // stock: up to an hour after the sleep
struct SkinTempView {
    int state = SkinNoData;
    int nightsLeft = 0;
    SkinTempNight night;   // the morning's night, if it has a row
    QVariantMap toMap() const;
};
SkinTempView skinTempView(const QDate &date, const QList<SkinTempNight> &nights, qint64 trackingSinceMs,
                          qint64 trackingEndMs, qint64 nowMs);
// "noData", "calibrating", "tracking", "analyzing", "inRange", "aboveRange", "belowRange"
QString skinTempStateName(int state);

} // namespace Health

#endif // SKINTEMP_H
