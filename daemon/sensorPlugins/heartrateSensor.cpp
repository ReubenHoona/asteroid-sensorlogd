/*
 * Copyright (C) 2023 Arseniy Movshev <dodoradio@outlook.com>
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */


#include <QDateTime>
#include <QTimer>
#include <QtSensors/QHrmSensor>
#include <QDebug>
#include <QString>

#include "../logger.h"
#include "../../common.h"

#include "heartrateSensor.h"

HeartrateSensorPlugin::HeartrateSensorPlugin(QObject *parent, int initInterval)  :
    QObject(parent){
    interval = initInterval;

    hrmSensor = new QHrmSensor(this);
    hrmSensor->setAlwaysOn(true); // sensorfw stops sensors in standby (display off) otherwise
    connect(hrmSensor,SIGNAL(readingChanged()),this,SLOT(finishRecording()));

    setupFilePath(sensorPathPrefix);

    qDebug() << "heartrate sensor is enabled. interval is (ms) " << interval;
    recordIntervalTimer = new QTimer(this);
    connect(recordIntervalTimer,SIGNAL(timeout()),this,SLOT(triggerRecording()));

    // give up on a recording that gets no usable reading (e.g. off the wrist)
    // rather than keeping the sensor on until the next one
    recordTimeoutTimer = new QTimer(this);
    recordTimeoutTimer->setSingleShot(true);
    recordTimeoutTimer->setInterval(60000);
    connect(recordTimeoutTimer,SIGNAL(timeout()),this,SLOT(abortRecording()));
    recordIntervalTimer->setSingleShot(true);
    recordIntervalTimer->start(interval);
    lastRecordTime = QDateTime::currentDateTime();
}

void HeartrateSensorPlugin::timeUpdate() {
    uint elapsed = QDateTime::currentMSecsSinceEpoch() - lastRecordTime.toMSecsSinceEpoch();
    qDebug() << "time until next steps recording" << recordIntervalTimer->remainingTime() << " elapsed = " << elapsed << " lastRecordTime " << lastRecordTime.toMSecsSinceEpoch();
    if (elapsed > interval) { //if too much time has passed, reset the timer and record
        triggerRecording();
    } else { //otherwise, restart the timer and compensate for time spent in suspend
        recordIntervalTimer->start(interval - elapsed);
    }
}

void HeartrateSensorPlugin::triggerRecording() {
    qDebug() << "heartrate interval recording";
    recordIntervalTimer->start(interval);
    lastRecordTime = QDateTime::currentDateTime();
    recordTimeoutTimer->start();
    hrmSensor->start();
}

void HeartrateSensorPlugin::finishRecording() {
    qDebug() << "bpm update received";
    int bpm = hrmSensor->reading()->bpm();
    qDebug() << QDateTime::currentDateTime().toString("hh:mm:ss") << " : " << bpm << hrmSensor->status() << hrmSensor->isActive();
    if ((bpm == 0) || (hrmSensor->status() < 3)) {
        qDebug() << "hrm sensor accuracy insufficient. waiting.";
        return;
    }
    fileAddRecord(sensorPathPrefix,QString::number(bpm));
    recordTimeoutTimer->stop();
    hrmSensor->stop();
}

void HeartrateSensorPlugin::abortRecording() {
    qDebug() << "no usable heartrate reading, stopping until the next recording";
    hrmSensor->stop();
}
