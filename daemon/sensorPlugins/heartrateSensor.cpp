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

// after this long without a usable reading the recording gives up; checked
// on the QTimer while awake and against the wall clock at a system wake
static const int RECORD_TIMEOUT_MS = 60000;
static const int RECORD_TIMEOUT_SLACK_MS = 5000;

static quint64 monotonicUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (quint64)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

HeartrateSensorPlugin::HeartrateSensorPlugin(QObject *parent, int initInterval, bool initContinuous)  :
    QObject(parent){
    interval = initInterval;
    continuous = initContinuous;

    hrmSensor = nullptr;
    makeSensor();
    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, SIGNAL(timeout()), this, SLOT(retryContinuous()));

    setupFilePath(sensorPathPrefix);

    qDebug() << "heartrate sensor is enabled. interval is (ms) " << interval;
    recordIntervalTimer = new QTimer(this);
    connect(recordIntervalTimer,SIGNAL(timeout()),this,SLOT(triggerRecording()));

    // give up on a recording that gets no usable reading (e.g. off the wrist)
    // rather than keeping the sensor on until the next one
    recordTimeoutTimer = new QTimer(this);
    recordTimeoutTimer->setSingleShot(true);
    recordTimeoutTimer->setInterval(RECORD_TIMEOUT_MS);
    connect(recordTimeoutTimer,SIGNAL(timeout()),this,SLOT(abortRecording()));
    recordIntervalTimer->setSingleShot(true);
    recordIntervalTimer->start(interval);
    lastRecordTime = QDateTime::currentDateTime();

    if (continuous) {
        qDebug() << "heartrate sensor is continuous";
        recordIntervalTimer->stop();
        retryContinuous();
    }
}

void HeartrateSensorPlugin::makeSensor() {
    delete hrmSensor;
    hrmSensor = new QHrmSensor(this);
    hrmSensor->setAlwaysOn(true); // sensorfw stops sensors in standby (display off) otherwise
    connect(hrmSensor,SIGNAL(readingChanged()),this,SLOT(finishRecording()));
}

// QtSensors' sensorfw backend does not set up a sensor again by itself once
// its first set-up failed (sensorfwd not ready, heart rate not listed by the
// hub yet), so start() kept failing quietly at every recording. Make a new
// sensor when a start fails.
bool HeartrateSensorPlugin::startSensor() {
    if (hrmSensor->start())
        return true;
    qWarning() << "heart rate sensor did not start, making it again";
    makeSensor();
    if (hrmSensor->start())
        return true;
    qWarning() << "heart rate sensor still does not start";
    return false;
}

void HeartrateSensorPlugin::retryContinuous() {
    if (startSensor()) {
        m_retryDelay = 30000;
        return;
    }
    qWarning() << "continuous heart rate: retrying in" << m_retryDelay / 1000 << "s";
    m_retryTimer->start(m_retryDelay);
    m_retryDelay = qMin(m_retryDelay * 2, 30 * 60 * 1000);
}

void HeartrateSensorPlugin::timeUpdate() {
    if (continuous)
        return;
    uint elapsed = QDateTime::currentMSecsSinceEpoch() - lastRecordTime.toMSecsSinceEpoch();
    qDebug() << "time until next steps recording" << recordIntervalTimer->remainingTime() << " elapsed = " << elapsed << " lastRecordTime " << lastRecordTime.toMSecsSinceEpoch();
    if (elapsed + 30000 > (uint)interval) { //if too much time has passed, reset the timer and record
        triggerRecording();
    } else { //otherwise, restart the timer and compensate for time spent in suspend
        recordIntervalTimer->start(interval - elapsed);
    }
}

void HeartrateSensorPlugin::wakeUpdate() {
    if (continuous)
        return; // the hub's batch has been logged as it arrived
    if (m_measuring) {
        // the batch of the last minute came with the flush; nothing usable in it
        if (QDateTime::currentMSecsSinceEpoch() - m_startedMs >= RECORD_TIMEOUT_MS - RECORD_TIMEOUT_SLACK_MS)
            abortRecording();
        return;
    }
    timeUpdate();
}

void HeartrateSensorPlugin::triggerRecording() {
    if (continuous)
        return;
    qDebug() << "heartrate interval recording";
    recordIntervalTimer->start(interval);
    lastRecordTime = QDateTime::currentDateTime();
    m_readings = 0;
    m_bestStatus = -1;
    m_measuring = true;
    m_startedMs = QDateTime::currentMSecsSinceEpoch();
    if (!startSensor()) {
        m_measuring = false;
        return; // tried again at the next recording
    }
    recordTimeoutTimer->start();
}

void HeartrateSensorPlugin::finishRecording() {
    int bpm = hrmSensor->reading()->bpm();
    if (!continuous) // continuous mode: a reading every second, around the clock; no journal lines for each
        qDebug() << "bpm update received" << QDateTime::currentDateTime().toString("hh:mm:ss") << " : " << bpm << hrmSensor->status() << hrmSensor->isActive();
    if (!continuous) {
        m_readings++;
        m_bestStatus = qMax(m_bestStatus, (int)hrmSensor->status());
    }
    if ((bpm == 0) || (hrmSensor->status() < 3)) {
        if (!continuous)
            qDebug() << "hrm sensor accuracy insufficient. waiting.";
        return;
    }
    if (continuous) {
        addContinuousSample(bpm);
        return;
    }
    if (!m_measuring)
        return; // a late sample after the recording ended
    // When it was taken: the reading's CLOCK_MONOTONIC time stamp comes from
    // the hub's boot time clock, so its age counts suspended time too (a
    // batched reading arrives up to a minute late)
    qint64 ageMs = ((qint64)monotonicUs() - (qint64)hrmSensor->reading()->timestamp()) / 1000;
    if (ageMs < 0 || ageMs > 2 * 3600 * 1000) // not a time base we know
        ageMs = 0;
    const qint64 takenMs = QDateTime::currentMSecsSinceEpoch() - ageMs;
    // sensorfw replays the last value it has when a sensor starts: only a
    // reading taken after this recording started counts
    if (takenMs + 1000 < m_startedMs) {
        qDebug() << "stale heart rate reading from before the recording, waiting";
        return;
    }
    fileAddRecord(sensorPathPrefix,QString::number(bpm),QDateTime::fromMSecsSinceEpoch(takenMs));
    m_measuring = false;
    recordTimeoutTimer->stop();
    hrmSensor->stop();
}

void HeartrateSensorPlugin::abortRecording() {
    // status: 0 no contact, 1 unreliable, 2 low, 3 medium, 4 high; >= 3 is recorded
    qWarning() << "no usable heartrate reading in" << recordTimeoutTimer->interval() / 1000 << "s:"
               << m_readings << "readings, best status" << m_bestStatus << "- stopping until the next recording";
    m_measuring = false;
    recordTimeoutTimer->stop();
    hrmSensor->stop();
}

void HeartrateSensorPlugin::addContinuousSample(int bpm) {
    // the reading's timestamp is CLOCK_MONOTONIC in microseconds (sensorfw);
    // batched samples arrive late, so date them by it rather than by arrival
    qint64 ageMs = ((qint64)monotonicUs() - (qint64)hrmSensor->reading()->timestamp()) / 1000;
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
