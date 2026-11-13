/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SLEEPESTIMATOR_H
#define SLEEPESTIMATOR_H

#include <QDate>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVariantMap>

#include "healthMetrics.h"

namespace Health {

/*
 * Sleep estimated on the watch (health-fitness-17). Stock shows the last night's sleep: duration, a sleep score
 * (Excellent / Good / Fair / Poor), the stages Awake / REM / Light / Deep, bedtime and wake time, naps and the
 * sleep goal ("Goal met!" / "Goal not met"); stock's stages and score are computed off the watch and synced
 * (SleepDetailsViewModel.java:90-99), so everything here is an approximation from what the watch has:
 * the step records and the heart rate readings (no accelerometer is logged: it would cost battery).
 *
 * A minute is a sleep candidate when the watch is worn (it has a heart rate, or readings on both sides of it
 * within 5 minutes each), no step was taken in it and the heart rate is within the lower fifth of the heart rate reserve above resting. Candidate minutes
 * separated by no more than 15 minutes make a session; the gap minutes are awake time inside it. The main
 * sleep is the longest session of the night of at least an hour; other sessions of at least 30 minutes with a
 * heart rate near resting are naps. Stages by the heart rate among the asleep minutes: the lowest fifth is deep
 * sleep, the highest fifth after the first hour is REM, the rest light. The score (0-100): 50 for the duration
 * against the goal, 25 for the share of deep and REM sleep (full at 40 %), 25 for the heart rate asleep against
 * resting (full at resting, none 20 bpm above). Bands as the Fitbit app's: 90 Excellent, 80 Good, 60 Fair.
 */
struct SleepSession {
    qint64 startMs = 0;
    qint64 endMs = 0;
    int asleepMin = 0;
    int awakeMin = 0; // inside the session, between the start and the end
    int remMin = 0;
    int lightMin = 0;
    int deepMin = 0;
    double avgHr = 0;
};

struct SleepNight {
    bool valid = false;
    SleepSession main;
    QList<SleepSession> naps;
    int score = 0;
    QString scoreLabel;      // Excellent, Good, Fair, Poor
    int goalMin = 480;
    bool goalMet = false;
    int napMin = 0;          // all the naps
    QDate wakeDate;          // the morning the night ended

    QVariantMap toMap() const;
    QJsonObject toJson() const;
    static SleepNight fromJson(const QJsonObject &object);
};

// The night that ended on wakeDay, from the series (it should cover the evening before to that evening).
SleepNight estimateSleep(const MinuteSeries &series, const QDate &wakeDate, const HealthProfile &profile,
                         int restingHr, int goalMin = 480);

QString sleepScoreLabel(int score);

} // namespace Health

#endif // SLEEPESTIMATOR_H
