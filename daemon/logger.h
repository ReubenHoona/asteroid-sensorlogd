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
 */
class Logger : public QObject
{
    Q_OBJECT
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
signals:
    // the steps goal was reached for the first time today (metric "steps")
    void goalReached(const QString &metric, int value, int goal);
    // auto-detected exercise: action "start" (activity "walking" / "running") or "stop"
    void exerciseOffered(const QString &action, const QString &activity, qlonglong sinceMs);
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
    bool daemonFresh;
    QSettings *settings;
    bool getDaemonFresh();
};
    bool dayFileExists(QString sensorPrefix, QDateTime date = QDateTime::currentDateTime()); //check if today has a log file for the given sensor
    QStringList fileGetPrevRecord(QString sensorPrefix, QDateTime recordTime = QDateTime::currentDateTime()); //works backwards to find the last record in today's file before the given time - returns nothing if no file is found.

#endif // LOGGER_H
