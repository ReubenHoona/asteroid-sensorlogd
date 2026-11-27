/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <limits>

#include <QDateTime>
#include <QTime>

#include "workoutLoader.h"
#include "../common.h"
#include "../store/healthDb.h"

using namespace Health;

WorkoutLoader::WorkoutLoader(QObject *parent) : QObject(parent)
{
    connectHealthChanged(this, SLOT(onHealthChanged(qlonglong)));
}

qlonglong WorkoutLoader::seq() const
{
    return HealthDb::readSeq(healthDbPath());
}

static QVariantList toList(const QList<WorkoutRecord> &rows)
{
    QVariantList out;
    for (const WorkoutRecord &w : rows)
        out << w.toMap();
    return out;
}

QVariantList WorkoutLoader::workouts(QDate from, QDate to)
{
    if (!from.isValid() || !to.isValid() || to < from)
        return {};
    return toList(HealthDb::readWorkouts(healthDbPath(), QDateTime(from, QTime(0, 0)).toMSecsSinceEpoch(),
                                         QDateTime(to.addDays(1), QTime(0, 0)).toMSecsSinceEpoch()));
}

QVariantList WorkoutLoader::recent(int n)
{
    if (n < 1)
        return {};
    return toList(HealthDb::readWorkouts(healthDbPath(), 0, std::numeric_limits<qint64>::max(), n));
}

QVariantMap WorkoutLoader::last()
{
    // a workout that is on is the newest; the one before it then
    for (const WorkoutRecord &w : HealthDb::readWorkouts(healthDbPath(), 0, std::numeric_limits<qint64>::max(), 2)) {
        if (w.endMs > 0)
            return w.toMap();
    }
    return {};
}

QVariantMap WorkoutLoader::workout(qlonglong id)
{
    WorkoutRecord w;
    QVector<LapRecord> laps;
    QVector<PauseRecord> pauses;
    if (!HealthDb::readWorkout(healthDbPath(), id, &w, &laps, &pauses) || w.deleted)
        return {};
    QVariantMap m = w.toMap();
    QVariantList lapList, pauseList;
    for (const LapRecord &l : laps)
        lapList << QVariantMap{{"n", l.n}, {"startMs", l.startMs}, {"endMs", l.endMs}, {"distanceM", l.distanceM},
                               {"avgBpm", l.avgBpm}, {"kind", l.kind}};
    for (const PauseRecord &p : pauses)
        pauseList << QVariantMap{{"startMs", p.startMs}, {"endMs", p.endMs}, {"auto", p.automatic}};
    m["laps"] = lapList;
    m["pauses"] = pauseList;
    return m;
}

QVariantList WorkoutLoader::track(qlonglong id)
{
    WorkoutRecord w;
    if (!HealthDb::readWorkout(healthDbPath(), id, &w) || w.deleted)
        return {};
    QVariantList out;
    for (const TrackPoint &p : HealthDb::readTrack(healthDbPath(), id))
        out << QVariantMap{{"t", p.tMs}, {"lat", p.latE7 / 1e7}, {"lon", p.lonE7 / 1e7}, {"alt", p.altDm / 10.0},
                           {"hacc", p.haccDm / 10.0}, {"speed", p.speedCms / 100.0}};
    return out;
}
