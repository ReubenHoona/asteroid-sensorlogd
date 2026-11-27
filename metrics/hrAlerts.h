/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HRALERTS_H
#define HRALERTS_H

#include <QList>

#include "healthMetrics.h"

namespace Health {

/*
 * High and low heart rate alerts (health-fitness-16). Stock: when the heart rate stays outside the high or the
 * low threshold during at least 10 minutes of inactivity, a notification "High heart rate" / "Low heart rate"
 * with "N bpm" appears, its detail "Your heart rate rose above N bpm while you appeared to be inactive from
 * T1 - T2." (or "fell below"); on by default, thresholds customisable (HrAlertService.java:77-94,175).
 *
 * The detector takes the minutes of a MinuteSeries in time order, once each, when they are complete, and is
 * asked when data has been delivered (the sensor hub's batch, a live minute that ended): it never runs on a
 * timer. A minute counts toward a run if it has a heart rate (every minute of the run does), fewer than
 * INACTIVE_STEPS steps, and its mean heart rate is above the high threshold (or below the low one). A run of
 * RUN_MINUTES makes one alert; the next one of that kind needs a minute that does not belong to a run.
 */
// Stock's defaults, used while the user has set no threshold of their own: low 50, high 120 bpm
// (D/abgd.java:431,449: HeartRateAlertThresholdType.CLIENT_SIDE_DEFAULT). Stock's precedence is the user's
// custom value, then a personal (derived) one, then the server's default, then these; the personal and the
// server's come from the cloud, which this watch has not, so here it is the user's value, else these.
static const int DEFAULT_HIGH_BPM = 120;
static const int DEFAULT_LOW_BPM = 50;

struct HrAlertThresholds {
    bool highEnabled = true;
    int high = 0; // bpm; 0: none set, DEFAULT_HIGH_BPM
    bool lowEnabled = true;
    int low = 0;  // bpm; 0: none set, DEFAULT_LOW_BPM

    // the thresholds in force
    int highBpm() const { return high > 0 ? high : DEFAULT_HIGH_BPM; }
    int lowBpm() const { return low > 0 ? low : DEFAULT_LOW_BPM; }
};

struct HrAlert {
    enum Kind { High, Low };
    Kind kind;
    int bpm;          // the mean of the run, what the notification shows
    int threshold;    // the threshold that was crossed
    qint64 startMs;   // the run's first minute
    qint64 endMs;     // the end of its last minute
};

class HrAlertDetector
{
public:
    static const int RUN_MINUTES = 10;
    static const int INACTIVE_STEPS = 10; // a minute with fewer steps is inactive

    // Looks at the minutes not seen yet that ended before nowMs. highBpm and lowBpm are the thresholds
    // in force (never 0). Returns the alerts that begin now, in time order.
    QList<HrAlert> feed(const MinuteSeries &series, qint64 nowMs, int highBpm, bool highEnabled, int lowBpm,
                        bool lowEnabled);

    // the minute (ms / 60000) up to which the minutes were looked at; the next to look at is after it
    qint64 seenUpTo() const { return m_seen; }
    // begin after this minute (the minutes up to it are not looked at)
    void setSeen(qint64 minute) { m_seen = minute; }
    void reset();

private:
    struct Run {
        int length = 0;
        qint64 start = 0;
        double sum = 0;
        bool alerted = false;
    };
    void advance(Run &run, bool member, qint64 minute, double hr);

    qint64 m_seen = -1; // minute index; -1: nothing looked at yet
    Run m_high, m_low;
};

} // namespace Health

#endif // HRALERTS_H
