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
#include <QPointF>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include "healthMetrics.h"

namespace Health {

/*
 * Sleep estimated on the watch (health-fitness-17; version 2: M4 WO-06). Stock shows the last night's sleep:
 * duration, a sleep score (Excellent 90-100 / Good 80-89 / Fair 60-79 / Poor 0-59: SleepScoreLevel.java), the
 * stages Awake / REM / Light / Deep (StagesSleepLevel WAKE, REM, LIGHT, DEEP) as a timeline (the sleep log's
 * levels: dateTime, level, seconds), bedtime and wake time, naps ("Nap from %1$s to %2$s") and the sleep goal;
 * stock's stages and score are computed off the watch (SleepDetailsViewModel.java:90-99), so everything here is
 * our approximation from what the watch logs, never the accelerometer itself (it would cost battery):
 *
 *   the heart rate readings (heartrate.db), the step records (stepCounter/), and since M4 the sensor hub's own
 *   streams: motion and stationary detect (motion/), step cadence (stepCadence/) and the off-body sensor
 *   (offBody/). Without them (an older sensorlogd, a night before M4, the streams or the provider off) the
 *   estimate goes by the steps and the heart rate alone, as version 1 did.
 *
 * A minute (of the window from 18:00 the evening before to 18:00 of the morning's day):
 *   worn      the off-body sensor says on the wrist; or, where it does not know, the minute has a heart rate
 *             reading or readings on both sides within HR_REACH_MIN. Off the wrist: not worn, and any heart rate
 *             reading of the minute is left out (the hub measures only on the wrist; one from then is not the
 *             wearer's). With the sensor saying on the wrist, a minute without a reading takes the nearest one's
 *             heart rate up to HR_REACH_WORN_MIN away (guess: the hub's readings pause when the fit is poor)
 *   stepping  a step in the minute (the step records, spread over at most their 10-minute interval) or a minute
 *             of the hub's step cadence
 *   still     the hub's stationary detect reported stillness and nothing reported movement since (motionMinutes)
 *   busy      four or more motion records within three minutes: the detectors are re-armed at once only while
 *             the display is on, so this is someone using the watch (guess)
 *   asleep    worn, not stepping, not busy, and the heart rate within the lower fifth of the heart rate reserve
 *             above resting (from 08:00 of the morning's day within the lower tenth, as near resting as a nap is:
 *             an hour at the desk is no sleep; guess); a still, worn minute without any heart rate counts too
 * Asleep minutes separated by no more than 15 minutes make a session, the gap minutes awake time inside it; a gap
 * with the watch off the wrist ends the session. A session begins at its first minute whose heart rate and the mean
 * of the 10 minutes from it are no more than 10 bpm above the session's lower quartile, and ends at the last such
 * minute: what is before and after is awake in bed or an evening on the sofa (guess). The main sleep is the
 * longest session that began between 20:00 and 08:00 with at least an hour asleep (stock recognises sleep after
 * about an hour still: Google's sleep help page). Naps: other sessions that begin from 08:00 with at least NAP_MIN
 * asleep minutes, a heart rate near resting and readings in most of their minutes; 20 minutes from Google's page
 * (CP3A's strings name no length; version 1 had 30).
 *
 * Stages (guesses, in the stock names, per minute, then as a timeline): Awake for the session's minutes that are
 * not asleep. Among the asleep ones, by the 5-minute mean heart rate: the lowest fifth is Deep, the highest fifth
 * after the first hour REM, the rest Light; a minute within MOVE_NEAR_MIN of a movement, a busy minute or a step
 * is Light (deep sleep and REM are still sleep); a sleep stage held for less than STAGE_MIN_RUN minutes takes the
 * one before it.
 *
 * Score (guess, 0-100, the main sleep only: stock scores the main sleep, and naps show 'No score'): 50 for the time
 * asleep against the goal, 25 for the share of Deep and REM (full at 40 %), 25 for restoration: 60 % by the heart
 * rate asleep against resting (full at resting, none 20 bpm above), 40 % by the restless share (the awake and the
 * moving minutes of the session: full at none, none at 25 %). Bands as stock's (sleepScoreLabel).
 */

// The stages, as health.db's sleep_stage.stage stores them; the names are stock's (StagesSleepLevel)
enum SleepStage { StageAwake = 0, StageLight = 1, StageDeep = 2, StageRem = 3 };
// "Awake", "Light", "Deep", "REM"
QString sleepStageName(int stage);

struct SleepStageSpan {
    qint64 startMs = 0;
    qint64 endMs = 0;
    int stage = StageLight;
    bool operator==(const SleepStageSpan &o) const
    {
        return startMs == o.startMs && endMs == o.endMs && stage == o.stage;
    }
};

struct SleepSession {
    qint64 startMs = 0;
    qint64 endMs = 0;
    int asleepMin = 0;
    int awakeMin = 0; // inside the session, between the start and the end
    int remMin = 0;
    int lightMin = 0;
    int deepMin = 0;
    double avgHr = 0;
    // the stage timeline from startMs to endMs, in time order, one span per run (empty for a night of version 1)
    QVector<SleepStageSpan> stages;
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
    QString algo;            // the estimate that made it, as stored (HealthDb::readNight); not in toMap()

    QVariantMap toMap() const;
    QJsonObject toJson() const;
    static SleepNight fromJson(const QJsonObject &object);
};

// What the watch logged besides steps and heart rate for a night (M4 WO-03, WO-05): the day files' records in time
// order (seconds since the epoch, value), from the last one before the window on. Empty lists: not logged.
struct SleepContext {
    QList<QPointF> motion;  // motion/: 1 movement began, 0 stillness began, 2 the detectors armed again
    QList<QPointF> cadence; // stepCadence/: a minute's start, its steps a minute
    QList<QPointF> offBody; // offBody/: 1 off the wrist, 0 on, -1 unknown
};

// what the motion records say of each minute of [fromMinute, fromMinute + n)
enum MotionMinute { MotionUnknown = 0, MotionStill = 1, MotionMoved = 2, MotionBusy = 3 };
QVector<char> motionMinutes(const QList<QPointF> &records, qint64 fromMinute, int n);
// what the off-body records say of each minute: 1 off the wrist in some of it, 0 on the wrist all of it, -1 unknown
QVector<signed char> offBodyMinutes(const QList<QPointF> &records, qint64 fromMinute, int n);

// The night that ended on wakeDate, from the series (it should cover the evening before to that evening) and, if
// there is one, the night's context
SleepNight estimateSleep(const MinuteSeries &series, const QDate &wakeDate, const HealthProfile &profile,
                         int restingHr, int goalMin = 480, const SleepContext &context = SleepContext());

QString sleepScoreLabel(int score);

// the algorithm's name in health.db's sleep_session.algo (version 1's: HealthImport::SLEEP_ALGO)
extern const char *const SLEEP_ALGO_V2;

} // namespace Health

#endif // SLEEPESTIMATOR_H
