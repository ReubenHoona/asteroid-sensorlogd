/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef AUTOEXERCISE_H
#define AUTOEXERCISE_H

#include <QList>
#include <QString>

#include "healthMetrics.h"

namespace Health {

/*
 * Auto-detected exercise prompts (health-fitness-28). Stock asks "Start tracking?" when it detects an activity
 * outside a workout (auto-start, ExerciseAutoRecognitionService.java:179-193); the setting is "Start workout
 * reminders" (autoExercise/start). Stock recognises more types; from the step cadence of the sensor hub's
 * batches there are two: walking and running.
 *
 * Start: START_MINUTES in a row with a cadence of WALK_CADENCE steps a minute or more and a mean of
 * WALK_MEAN or more (RUN_MEAN or more: running). One offer per activity: another needs a quiet spell
 * (QUIET_MINUTES with fewer than QUIET_CADENCE steps) in between. While a workout is going (the Exercise app's,
 * in health.db) there is no start offer: the workout is the activity.
 * No stop offer here (M4 WO-07): stock's "Finished working out?" (auto-stop) is an event of the workout itself,
 * asked for by the exercise app in its exercise config when "End workout reminders" is on and the type has it
 * (wxl.java:1601-1602, EXERCISE_EVENT_TYPE_AUTO_STOP) and turned by it into the question (ptx.java:580,
 * nxz.java:372-415, AutoStopNotificationActivity). The Exercise app asks it, after 10 still minutes judged from
 * the step cadence and the GPS speed of the workout's type (a ride has no steps); it reads autoExercise/stop.
 * Like the other detectors it looks at completed minutes when data has been delivered, never on a timer.
 */
struct AutoExerciseSettings {
    bool start = true;
};

struct ExerciseOffer {
    QString activity; // "walking" or "running"
    qint64 sinceMs;   // when the activity began
};

class AutoExercise
{
public:
    static const int START_MINUTES = 10;
    static const int WALK_CADENCE = 70;
    static const int WALK_MEAN = 90;
    static const int RUN_MEAN = 140;
    static const int QUIET_CADENCE = 20;
    static const int QUIET_MINUTES = 5;

    QList<ExerciseOffer> feed(const MinuteSeries &series, qint64 nowMs, const AutoExerciseSettings &settings,
                              bool workoutActive);
    void reset();
    // begin after this minute (the minutes up to it are not looked at)
    void setSeen(qint64 minute) { m_seen = minute; }

private:
    qint64 m_seen = -1;
    int m_active = 0;       // minutes in a row of walking cadence
    double m_activeSum = 0;
    qint64 m_activeStart = 0;
    int m_quiet = 0;        // minutes in a row of quiet
    bool m_armed = true;    // a start offer may be made (a quiet spell came since the last)
};

} // namespace Health

#endif // AUTOEXERCISE_H
