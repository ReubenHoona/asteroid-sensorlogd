/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <MDConfItem>

#include "goals.h"

namespace Health {

const char *const MAIN_GOAL_KEY = "/org/asteroidos/health/goals/main";

static const struct {
    GoalType type;
    const char *name;
    const char *key;
    double fallback;
} GOALS[] = {
    {GoalType::Steps, "steps", "/org/asteroidos/ux/steps-goal", 10000},
    {GoalType::Calories, "calories", "/org/asteroidos/health/goals/calories", 2000},
    {GoalType::Distance, "distance", "/org/asteroidos/health/goals/distance-m", 8047},
    {GoalType::Floors, "floors", "/org/asteroidos/health/goals/floors", 10},
    {GoalType::Azm, "azm", "/org/asteroidos/health/goals/azm", 22},
};

QString MainGoal::metric() const
{
    for (const auto &g : GOALS)
        if (g.type == type)
            return QString::fromLatin1(g.name);
    return QStringLiteral("steps");
}

const char *goalKey(GoalType type)
{
    for (const auto &g : GOALS)
        if (g.type == type)
            return g.key;
    return GOALS[0].key;
}

double defaultGoal(GoalType type)
{
    for (const auto &g : GOALS)
        if (g.type == type)
            return g.fallback;
    return GOALS[0].fallback;
}

GoalType goalTypeFrom(const QVariant &name)
{
    const QString n = name.toString().trimmed().toLower();
    for (const auto &g : GOALS)
        if (n == QLatin1String(g.name))
            return g.type;
    return GoalType::Steps;
}

QStringList mainGoalKeys()
{
    QStringList keys{QString::fromLatin1(MAIN_GOAL_KEY)};
    for (const auto &g : GOALS)
        keys << QString::fromLatin1(g.key);
    return keys;
}

MainGoal mainGoalFrom(const QVariantList &values)
{
    MainGoal goal;
    goal.type = goalTypeFrom(values.value(0));
    int i = 1;
    for (const auto &g : GOALS) {
        if (g.type == goal.type) {
            bool ok = false;
            const double v = values.value(i).toDouble(&ok);
            goal.target = ok && v > 0 ? v : g.fallback;
        }
        i++;
    }
    return goal;
}

MainGoal loadMainGoal()
{
    QVariantList values;
    for (const QString &key : mainGoalKeys())
        values << MDConfItem(key).value();
    return mainGoalFrom(values);
}

double goalValue(GoalType type, const DaySummary &day)
{
    switch (type) {
    case GoalType::Steps:
        return day.steps;
    case GoalType::Calories:
        return day.energyKcal;
    case GoalType::Distance:
        return day.distanceM;
    case GoalType::Floors:
        return day.floors;
    case GoalType::Azm:
        return day.zones.azm;
    }
    return day.steps;
}

} // namespace Health
