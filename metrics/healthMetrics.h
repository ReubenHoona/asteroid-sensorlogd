/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HEALTHMETRICS_H
#define HEALTHMETRICS_H

#include <QList>
#include <QMap>
#include <QPointF>
#include <QVector>

#include "healthProfile.h"

/*
 * The numbers stock's Health Services computes on the watch (health-fitness-11): distance, energy burned,
 * floors, heart rate zones, Active Zone Minutes and hourly activity. Everything here is a pure function of
 * what sensorlogd logs (the step count, the heart rate readings, the barometer) and of the Health profile,
 * computed when asked: no sensor is woken for it.
 *
 * The common ground is a MinuteSeries: for every minute that has data, the steps taken in it and the mean heart
 * rate of its readings. The daemon fills one from the readings as the sensor hub delivers them (exact times);
 * the readers fill one from the day files and the heart rate database (the step day files hold the count at
 * the end of every interval and of every batch, so a step count is spread over the interval it came from).
 */
namespace Health {

struct Minute {
    int steps = 0;
    double hr = 0;   // mean bpm of the minute's readings; 0: none
    int hrCount = 0; // readings in the minute
};

// one reading of the heart rate database, as a minute's mean
struct HrMinute {
    qint64 minute; // ms since the epoch / 60000
    double bpm;
    int count;
};

class MinuteSeries
{
public:
    static qint64 minuteOf(qint64 ms) { return ms >= 0 ? ms / 60000 : -((-ms + 59999) / 60000); }
    static qint64 msOf(qint64 minute) { return minute * 60000; }

    bool isEmpty() const { return m_data.isEmpty(); }
    qint64 firstMinute() const { return m_data.isEmpty() ? 0 : m_data.firstKey(); }
    qint64 lastMinute() const { return m_data.isEmpty() ? 0 : m_data.lastKey(); }
    Minute at(qint64 minute) const { return m_data.value(minute); }
    bool has(qint64 minute) const { return m_data.contains(minute); }
    const QMap<qint64, Minute> &data() const { return m_data; }
    void clear() { m_data.clear(); }
    // forget the minutes before this one
    void trimBefore(qint64 minute);

    void addSteps(qint64 ms, int steps);
    void addHr(qint64 ms, int bpm);
    void addHrMinute(const HrMinute &m);
    // The step day file's records (time in seconds, the day's count so far), as per-minute steps. A record's
    // increase over the one before it came in its own interval, so it is spread over the time from the earlier
    // of the two: the previous record or the start of the record's interval (intervalMs is stepCounter/interval)
    // to the record. `before` is the count the first record's day started from (0).
    void addStepRecords(const QList<QPointF> &records, qint64 intervalMs, int before = 0);

    // the steps of the minutes from the one of fromMs to the one of toMs, both included
    int stepsBetween(qint64 fromMs, qint64 toMs) const;

private:
    QMap<qint64, Minute> m_data;
};

enum class Zone { Light, Moderate, Vigorous, Peak };

// the zone of a heart rate by the heart rate reserve (stock: Light below 40 %, Moderate 40-59 %, Vigorous
// 60-84 %, Peak 85 % and up, "based on age and your RHR")
Zone zoneForBpm(double bpm, int maxHr, int restingHr);
// Active Zone Minutes a minute in this zone earns: 1 moderate, 2 vigorous or peak (stock)
int azmForZone(Zone zone);
// the bpm at which a zone begins
int zoneStartBpm(Zone zone, int maxHr, int restingHr);

struct ZoneMinutes {
    int light = 0, moderate = 0, vigorous = 0, peak = 0;
    int azm = 0;
};
// the minutes [fromMinute, toMinute) that have a heart rate
ZoneMinutes zoneMinutes(const MinuteSeries &series, qint64 fromMinute, qint64 toMinute, const HealthProfile &profile,
                        int restingHr);

// The resting heart rate stock shows: from age and the readings (a typical resting rate is the lowest part of a
// week's minutes, mostly sleep): the 5th percentile of the minute means, 0 if there are not enough minutes
int estimateRestingHr(const QVector<double> &minuteMeans);
// the resting heart rate to use: the profile's if set, else the estimate, else 60
int effectiveRestingHr(const HealthProfile &profile, int estimate);

// the default thresholds of the high and low heart rate alerts (stock: "from age and typical resting heart
// rate", the numbers are not published: here half the heart rate reserve above resting for high, 20 bpm below
// resting for low, both to 5 bpm)
int defaultHighBpm(const HealthProfile &profile, int restingHr);
int defaultLowBpm(const HealthProfile &profile, int restingHr);

// stride from height and sex (the usual 0.415 / 0.413 of the height, in between if the gender is not set)
double strideM(const HealthProfile &profile);
double distanceM(int steps, const HealthProfile &profile);
// Mifflin-St Jeor, kcal a day (the mean of the male and female constants if the gender is not set)
double bmrKcalPerDay(const HealthProfile &profile);
// The kcal a minute adds to the resting energy: walking costs 0.5 kcal per kg per km, running (140 steps a
// minute and up) 1.0; a heart rate in the moderate zone or above counts too (Keytel et al.), the larger of the
// two is taken.
double activeKcalForMinute(const Minute &minute, const HealthProfile &profile, int restingHr);
// Energy burned over [fromMs, toMs): the resting energy for the whole time (stock includes it) plus the active
// energy of the series' minutes in it
double energyKcal(const MinuteSeries &series, qint64 fromMs, qint64 toMs, const HealthProfile &profile, int restingHr);

// Floors climbed from the barometer day file's records (time in seconds, pressure; Pa or hPa) and the steps:
// the altitude gained between two readings that had steps between them, 3.048 m (10 ft) a floor. A gain
// below 1.5 m is weather, not a climb. The barometer is read every ten minutes, so this is a coarse count.
int floorsClimbed(const QList<QPointF> &pressure, const MinuteSeries &steps);
// the altitude difference in metres between two pressures in hPa (the barometric formula near sea level)
double altitudeDifferenceM(double fromHpa, double toHpa);

// Hourly activity: the hours of [firstHour, endHour) of the local day that have 250 steps or more (stock:
// 9:00-18:00, 250 steps an hour)
struct HourlyActivity {
    int hoursMet = 0;
    int hoursTotal = 0;
    QVector<int> hourSteps = QVector<int>(24, 0);
};
HourlyActivity hourlyActivity(const MinuteSeries &series, const QDate &day, int firstHour = 9, int endHour = 18,
                              int stepsPerHour = 250);

static const int HOURLY_STEPS = 250;
static const int AZM_WEEKLY_GOAL = 150;

} // namespace Health

#endif // HEALTHMETRICS_H
