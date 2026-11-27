/*
 * Copyright (C) 2023 Arseniy Movshev <dodoradio@outlook.com>
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <QObject>
#include <QDateTime>
#include <QDate>
#include <QDBusInterface>
#include <QTimer>
#include <QString>
#include <QSettings>
#include <QDBusAbstractAdaptor>
#include <QDBusServiceWatcher>

#include "sensorPlugins/stepCounter.h"
#include "sensorPlugins/heartrateSensor.h"
#include "sensorPlugins/barometerSensor.h"
#include "healthMonitor.h"
#include "healthNotifier.h"
#include "../store/healthDb.h"

/*
 * Heart rate and steps are measured all the time by their sensors, which on
 * a sensor hub batch the readings while the system sleeps (the hub wakes the
 * system itself only when a batch must be delivered); the plugins write the
 * records when readings arrive, dated by the readings. Nothing here wakes
 * the system: the barometer is read on an interval timer that only runs
 * while the system is awake, and at display on.
 *
 * The Health monitor (healthMonitor.h) works on the same readings: heart rate
 * alerts, reminders to move, the steps goal, exercise prompts and the sleep
 * estimate, looked at when the hub delivers a batch.
 *
 * The health database (health.db, store/healthDb.h) has one writer, this
 * daemon: the monitor writes the nights and the days, the methods below the
 * workouts (the Exercise app's, WO-02). After every write that changed it
 * the signal HealthChanged(x seq) goes out (the QML plugin's loaders read
 * again then); nothing is sent when nothing was written.
 */
class Logger : public QObject
{
    Q_OBJECT
    // the D-Bus interface name the clients use; without it Qt names the interface after the
    // application's domain and name (org.asteroidos.sensorlogd.Logger)
    Q_CLASSINFO("D-Bus Interface", "org.asteroid.sensorlogd.logger")
public:
    explicit Logger(QObject *parent = 0);
    virtual ~Logger();
public slots:
    void setup();
    void resetup();
    void triggerRecording();
    // the Health app changed reminders/*, hrAlerts/* or autoExercise/* (or the profile): read them again
    void healthSettingsChanged();
    // the Exercise app has a workout going or not
    void setWorkoutActive(bool active);
    // the time of the next reminder to move (ms since the epoch), -1 if none
    qlonglong nextReminderAt();

    // ---- workouts in health.db (D-Bus: org.asteroid.sensorlogd.logger) ----
    // A workout begins (source "manual" or "auto", type: letters, digits, '-', '_'): its id, 0 if refused (a
    // workout with that start, bad values). It counts as the workout going (setWorkoutActive).
    qlonglong BeginWorkout(qlonglong startMs, const QString &type, const QString &source, bool gps);
    // GPS fixes, batched: six numbers a fix (ax): t_ms, lat_e7, lon_e7, alt_dm, hacc_dm, speed_cms
    bool AddTrackPoints(qlonglong id, const QList<qlonglong> &points);
    // kind: "split-km", "split-mile", "manual"; n from 1
    bool AddLap(qlonglong id, int n, qlonglong startMs, qlonglong endMs, double distanceM, int avgBpm,
                const QString &kind);
    bool SetPaused(qlonglong id, bool paused, qlonglong atMs, bool automatic);
    // the end, with the summary (a{sv}): activeS, distanceM, kcal, steps, avgBpm, maxBpm, lightS, moderateS,
    // vigorousS, peakS, azm, elevGainM; the workout is no longer going
    bool EndWorkout(qlonglong id, qlonglong endMs, const QVariantMap &summary);
    // a tombstone: the row stays (marked deleted, for a sync), its track, laps and pauses go
    bool DeleteWorkout(qlonglong id);
    // the change counter of health.db
    qlonglong HealthSeq();
signals:
    // the steps goal was reached for the first time today (metric "steps")
    void goalReached(const QString &metric, int value, int goal);
    // auto-detected exercise: action "start" (activity "walking" / "running") or "stop"
    // D-Bus signal ExerciseOffered (s action, s activity, x sinceMs): asteroid-exercise's rcExerciseOffered
    void ExerciseOffered(const QString &action, const QString &activity, qlonglong sinceMs);
    // health.db changed; seq: its change counter after the write
    void HealthChanged(qlonglong seq);
private slots:
    void displayOn(QString displayState);
    void sensorfwRegistered();

private:
    void deletePlugins();
    QDBusServiceWatcher *m_sensorfwWatcher;
    bool m_setupDone = false;
    bool heartrateSensorEnabled = false;
    HeartrateSensorPlugin *m_heartrateSensor = nullptr;
    bool stepCounterEnabled = false;
    StepCounterPlugin *m_stepCounter = nullptr;
    bool barometerEnabled = false;
    BarometerSensorPlugin *m_barometerSensor = nullptr;
    DBusNotificationPoster m_poster;
    HealthMonitor *m_health = nullptr;
    Health::HealthDb *m_store = nullptr;
    bool daemonFresh;
    QSettings *settings;
    bool getDaemonFresh();
};
    bool dayFileExists(QString sensorPrefix, QDateTime date = QDateTime::currentDateTime()); //check if today has a log file for the given sensor
    QStringList fileGetPrevRecord(QString sensorPrefix, QDateTime recordTime = QDateTime::currentDateTime()); //works backwards to find the last record in today's file before the given time - returns nothing if no file is found.

#endif // LOGGER_H
