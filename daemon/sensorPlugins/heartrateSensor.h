/*
 * Copyright (C) 2023 Arseniy Movshev <dodoradio@outlook.com>
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HEARTRATESENSOR_H
#define HEARTRATESENSOR_H

#include <QObject>
#include <QDateTime>
#include <QDBusInterface>
#include <QTimer>

#include <QtSensors/QHrmSensor>

class HeartrateSensorPlugin : public QObject
{
    Q_OBJECT
public:
    explicit HeartrateSensorPlugin(QObject *parent = 0, int initInterval = 600000, bool continuous = false);
    virtual ~HeartrateSensorPlugin() {};

    void timeUpdate();

public slots:
    void triggerRecording();

private slots:
    void finishRecording();
    void abortRecording();

private:
    QDateTime lastRecordTime;
    int interval;
    QTimer *recordIntervalTimer;
    QTimer *recordTimeoutTimer;
    QHrmSensor *hrmSensor;

    // continuous mode: the sensor stays on (a sensor hub can batch the samples
    // while the system sleeps) and one average per minute is logged
    void addContinuousSample(int bpm);
    bool continuous;
    qint64 minuteStart = 0;
    int minuteSum = 0;
    int minuteCount = 0;

    const QString sensorPathPrefix = "heartrateMonitor";
};

#endif // HEARTRATESENSOR_H
