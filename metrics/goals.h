/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef GOALS_H
#define GOALS_H

#include <QString>
#include <QStringList>
#include <QVariant>

#include "healthData.h"

namespace Health {

/*
 * The main goal (R5 E-18, with E-05). Stock has one primary goal of the account, set in the phone app ("Set goal in
 * %1$s mobile app", s:140125): its type is STEPS, CALORIES, DISTANCE, FLOORS or AZM (PrimaryGoalType), its value the
 * account's daily goal of that metric (DailyActivitiesGoalsBody: steps, caloriesOut, distance, floors,
 * activeZoneMinutes). The Main goal tile, the Today goal ring and the goal celebration follow it: the celebration of
 * the type that was reached (tui.java:505-580: DailySteps / Calories / Distance / Floors / Azm Celebration), once a
 * day, when the hub's passive goal of that metric fires (PrimaryGoalReceiver).
 *
 * The keys (the phone sets them; asteroid-btsyncd writes the steps goal today, the others are for it):
 *   /org/asteroidos/health/goals/main        s  steps (default) | calories | distance | floors | azm
 *   /org/asteroidos/ux/steps-goal            i  steps a day (10000; the one steps goal of the watch)
 *   /org/asteroidos/health/goals/calories    i  kcal a day (2000, guess: the account's comes from the cloud)
 *   /org/asteroidos/health/goals/distance-m  i  metres a day (8047: Fitbit's 5 miles, guess)
 *   /org/asteroidos/health/goals/floors      i  floors a day (10, guess: Fitbit's)
 *   /org/asteroidos/health/goals/azm         i  Active Zone Minutes a day (22, guess: the weekly 150 over 7 days)
 * A type that is not one of these is steps; a goal value of 0 or less, or unset, is the default.
 */
enum class GoalType { Steps, Calories, Distance, Floors, Azm };

struct MainGoal {
    GoalType type = GoalType::Steps;
    double target = 10000;
    QString metric() const; // "steps", "calories", "distance", "floors", "azm"
};

extern const char *const MAIN_GOAL_KEY;
// the key of a type's goal value
const char *goalKey(GoalType type);
double defaultGoal(GoalType type);
GoalType goalTypeFrom(const QVariant &name);
// the main goal from the dconf values of keys() (in order: main, steps, calories, distance, floors, azm)
MainGoal mainGoalFrom(const QVariantList &values);
QStringList mainGoalKeys();
// from dconf
MainGoal loadMainGoal();
// the day's value of a type's metric
double goalValue(GoalType type, const DaySummary &day);

} // namespace Health

#endif // GOALS_H
