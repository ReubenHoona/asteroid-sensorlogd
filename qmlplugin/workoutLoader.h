/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef WORKOUTLOADER_H
#define WORKOUTLOADER_H

#include <QDate>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

/*
 * The workouts in health.db (store/healthDb.h) for QML: the Exercise app's history, the launcher's Last activity
 * tile and complications (WO-10), the Health app. Read when asked, nothing kept; deleted workouts are left out.
 * A workout:
 *
 *   { id, startMs, endMs (0: on), activeS, type, distanceM, kcal, steps, avgBpm, maxBpm, lightS, moderateS,
 *     vigorousS, peakS, azm, elevGainM, source ("manual", "auto"), gps, imported (from the old exercise log),
 *     ended, seq }
 *
 * workout(id) adds laps: [{ n, startMs, endMs, distanceM, avgBpm, kind }] and pauses: [{ startMs, endMs, auto }].
 * dataChanged: the daemon wrote to health.db (its D-Bus signal HealthChanged).
 */
class WorkoutLoader : public QObject
{
    Q_OBJECT
    // the change counter of health.db when last asked (a binding on it reads again after every write)
    Q_PROPERTY(qlonglong seq READ seq NOTIFY dataChanged)

public:
    explicit WorkoutLoader(QObject *parent = nullptr);

    qlonglong seq() const;

    // the workouts that began on the local days [from, to], newest first
    Q_INVOKABLE QVariantList workouts(QDate from, QDate to);
    // the newest n workouts
    Q_INVOKABLE QVariantList recent(int n);
    // the newest finished workout ({} if none)
    Q_INVOKABLE QVariantMap last();
    // one workout with its laps and pauses ({} if there is none or it was deleted)
    Q_INVOKABLE QVariantMap workout(qlonglong id);
    // its track: [{ t (ms), lat, lon (degrees), alt (m), hacc (m), speed (m/s) }]
    Q_INVOKABLE QVariantList track(qlonglong id);

signals:
    void dataChanged();

private slots:
    void onHealthChanged(qlonglong) { emit dataChanged(); }
};

#endif // WORKOUTLOADER_H
