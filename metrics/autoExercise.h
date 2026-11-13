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
 * (auto-start) and "Finished working out?" when the activity stops during a workout (auto-stop)
 * (ExerciseAutoRecognitionService.java:179-193); the settings are "Auto-Start" and "Auto-Stop"
 * (strings 0x7f14006a-0x7f14006e). Stock recognises more types; from the step cadence of the sensor hub's
 * batches there are two: walking and running.
 *
 * Start: START_MINUTES in a row with a cadence of WALK_CADENCE steps a minute or more and a mean of
 * WALK_MEAN or more (RUN_MEAN or more: running). One offer per activity: another needs a quiet spell
 * (QUIET_MINUTES with fewer than QUIET_CADENCE steps) in between.
 * Stop: while a workout is going (the Exercise app says so), STOP_MINUTES in a row of quiet; one offer per workout
 * and quiet spell.
 * Like the other detectors it looks at completed minutes when data has been delivered, never on a timer.
 */
struct AutoExerciseSettings {
    bool start = true;
    bool stop = true;
};

struct ExerciseOffer {
    enum Kind { Start, Stop };
    Kind kind;
    QString activity; // "walking" or "running" (Start); empty for Stop
    qint64 sinceMs;   // Start: when the activity began; Stop: when it went quiet
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
    static const int STOP_MINUTES = 5;

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
    qint64 m_quietStart = 0;
    bool m_armed = true;    // a start offer may be made (a quiet spell came since the last)
    bool m_stopOffered = false;
};

} // namespace Health

#endif // AUTOEXERCISE_H
