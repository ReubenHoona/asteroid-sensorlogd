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
#include <time.h>
#include <QTimer>
#include <QtSensors/QHrmSensor>
#include <QDebug>
#include <QString>

#include "../logger.h"
#include "../../common.h"

#include "heartrateSensor.h"

HeartrateSensorPlugin::HeartrateSensorPlugin(QObject *parent, int initInterval, bool initContinuous)  :
    QObject(parent){
    interval = initInterval;
    continuous = initContinuous;

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

    if (continuous) {
        qDebug() << "heartrate sensor is continuous";
        recordIntervalTimer->stop();
        hrmSensor->start();
    }
}

void HeartrateSensorPlugin::timeUpdate() {
    if (continuous)
        return;
    uint elapsed = QDateTime::currentMSecsSinceEpoch() - lastRecordTime.toMSecsSinceEpoch();
    qDebug() << "time until next steps recording" << recordIntervalTimer->remainingTime() << " elapsed = " << elapsed << " lastRecordTime " << lastRecordTime.toMSecsSinceEpoch();
    if (elapsed > interval) { //if too much time has passed, reset the timer and record
        triggerRecording();
    } else { //otherwise, restart the timer and compensate for time spent in suspend
        recordIntervalTimer->start(interval - elapsed);
    }
}

void HeartrateSensorPlugin::triggerRecording() {
    if (continuous)
        return;
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
    if (continuous) {
        addContinuousSample(bpm);
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

void HeartrateSensorPlugin::addContinuousSample(int bpm) {
    // the reading's timestamp is CLOCK_MONOTONIC in microseconds (sensorfw);
    // batched samples arrive late, so date them by it rather than by arrival
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    qint64 ageMs = ((qint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 - (qint64)hrmSensor->reading()->timestamp()) / 1000;
    if (ageMs < 0 || ageMs > 2 * 3600 * 1000) // not a time base we know
        ageMs = 0;
    qint64 taken = QDateTime::currentMSecsSinceEpoch() - ageMs;
    qint64 minute = taken / 60000 * 60000;

    if (minuteCount && minute != minuteStart) {
        fileAddRecord(sensorPathPrefix, QString::number(qRound((double)minuteSum / minuteCount)),
                      QDateTime::fromMSecsSinceEpoch(minuteStart));
        minuteCount = 0;
        minuteSum = 0;
    }
    if (!minuteCount)
        minuteStart = minute;
    minuteSum += bpm;
    minuteCount++;
}
