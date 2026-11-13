/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <cmath>

#include "hrAlerts.h"

namespace Health {

void HrAlertDetector::reset()
{
    m_seen = -1;
    m_high = Run();
    m_low = Run();
}

// a member extends the run, anything else ends it (and the next run may alert again)
void HrAlertDetector::advance(Run &run, bool member, qint64 minute, double hr)
{
    if (!member) {
        run = Run();
        return;
    }
    if (run.length == 0)
        run.start = minute;
    run.length++;
    run.sum += hr;
}

QList<HrAlert> HrAlertDetector::feed(const MinuteSeries &series, qint64 nowMs, int highBpm, bool highEnabled, int lowBpm,
                                     bool lowEnabled)
{
    QList<HrAlert> alerts;
    if (series.isEmpty())
        return alerts;
    const qint64 lastDone = MinuteSeries::minuteOf(nowMs) - 1; // the current minute is not over
    if (m_seen < 0)
        m_seen = series.firstMinute() - 1;
    if (lastDone - m_seen > 24 * 60) // a long gap (the clock was set, a day of suspend): nothing older than a day
        m_seen = lastDone - 24 * 60;
    for (qint64 minute = m_seen + 1; minute <= lastDone; minute++) {
        const Minute m = series.at(minute);
        const bool usable = series.has(minute) && m.hr > 0 && m.steps < INACTIVE_STEPS;
        const bool high = highEnabled && usable && m.hr > highBpm;
        const bool low = lowEnabled && usable && m.hr < lowBpm;
        advance(m_high, high, minute, m.hr);
        advance(m_low, low, minute, m.hr);
        if (m_high.length >= RUN_MINUTES && !m_high.alerted) {
            m_high.alerted = true;
            alerts.append({HrAlert::High, (int)std::lround(m_high.sum / m_high.length), highBpm,
                           MinuteSeries::msOf(m_high.start), MinuteSeries::msOf(minute + 1)});
        }
        if (m_low.length >= RUN_MINUTES && !m_low.alerted) {
            m_low.alerted = true;
            alerts.append({HrAlert::Low, (int)std::lround(m_low.sum / m_low.length), lowBpm,
                           MinuteSeries::msOf(m_low.start), MinuteSeries::msOf(minute + 1)});
        }
        m_seen = minute;
    }
    return alerts;
}

} // namespace Health
