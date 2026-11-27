/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "autoExercise.h"

namespace Health {

QString exerciseTypeOf(const QString &activity)
{
    return activity == QLatin1String("running") ? QStringLiteral("Run") : QStringLiteral("Walk");
}

void AutoExercise::reset()
{
    *this = AutoExercise();
}

QList<ExerciseOffer> AutoExercise::feed(const MinuteSeries &series, qint64 nowMs, const AutoExerciseSettings &settings,
                                        bool workoutActive)
{
    QList<ExerciseOffer> offers;
    if (series.isEmpty())
        return offers;
    const qint64 lastDone = MinuteSeries::minuteOf(nowMs) - 1;
    if (m_seen < 0)
        m_seen = series.firstMinute() - 1;
    if (lastDone - m_seen > 24 * 60)
        m_seen = lastDone - 24 * 60;
    for (qint64 minute = m_seen + 1; minute <= lastDone; minute++) {
        const int steps = series.at(minute).steps; // a minute without data is a minute without steps
        const bool walking = steps >= WALK_CADENCE;
        const bool quiet = steps < QUIET_CADENCE;

        if (walking) {
            if (m_active == 0) {
                m_activeStart = minute;
                m_activeSum = 0;
            }
            m_active++;
            m_activeSum += steps;
        } else {
            m_active = 0;
        }
        if (quiet)
            m_quiet++;
        else
            m_quiet = 0;

        if (m_quiet >= QUIET_MINUTES)
            m_armed = true;

        if (!workoutActive) {
            if (settings.start && m_armed && m_active >= START_MINUTES && m_activeSum / m_active >= WALK_MEAN) {
                m_armed = false;
                const QString activity = m_activeSum / m_active >= RUN_MEAN ? QStringLiteral("running")
                                                                            : QStringLiteral("walking");
                // the type's own 'Start workout reminders' off: no offer for this activity
                if (!settings.startOff.contains(exerciseTypeOf(activity)))
                    offers.append({activity, MinuteSeries::msOf(m_activeStart)});
            }
        } else {
            m_armed = false; // the workout in progress is the activity ("Finished working out?" is the app's)
        }
        m_seen = minute;
    }
    return offers;
}

} // namespace Health
