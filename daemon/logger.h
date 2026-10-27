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

class BackgroundActivity;

class Logger : public QObject
{
    Q_OBJECT
public:
    explicit Logger(QObject *parent = 0);
    virtual ~Logger() {};
public slots:
    void setup();
    void resetup();
    void triggerRecording();
private slots:
    void displayOn(QString displayState);
    void sensorfwRegistered();
    void wakeRunning();
    void wakeWork();
    void wakeCheckDone();
    void wakeDone();

private:
    QDBusServiceWatcher *m_sensorfwWatcher;
    bool m_setupDone = false;
    bool heartrateSensorEnabled = false;
    HeartrateSensorPlugin *m_heartrateSensor = nullptr;
    bool stepCounterEnabled = false;
    StepCounterPlugin *m_stepCounter = nullptr;
    bool barometerEnabled = false;
    BarometerSensorPlugin *m_barometerSensor = nullptr;
    bool daemonFresh;
    QSettings *settings;
    bool getDaemonFresh();

    // Periodic system wake (dsme iphb through libkeepalive): the recordings
    // run on it, so they happen with the system suspended too, and the sensor
    // hub keeps heart rate and steps batched in between (non-wake-up)
    void scheduleWake(int seconds);
    void flushHub();
    BackgroundActivity *m_wake = nullptr;
    QTimer *m_wakeSettle;   // flush -> batched samples delivered
    QTimer *m_wakePoll;     // a short recording (barometer) still running
    QTimer *m_wakeLimit;    // the keepalive never lasts longer than this
    int m_wakeInterval = 600; // [s]
    bool m_flushWarned = false;

};
    bool dayFileExists(QString sensorPrefix, QDateTime date = QDateTime::currentDateTime()); //check if today has a log file for the given sensor
    QStringList fileGetPrevRecord(QString sensorPrefix, QDateTime recordTime = QDateTime::currentDateTime()); //works backwards to find the last record in today's file before the given time - returns nothing if no file is found.

#endif // LOGGER_H
