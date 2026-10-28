/*
 * Copyright (C) 2023 Arseniy Movshev <dodoradio@outlook.com>
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>

#include <QDateTime>
#include <QDebug>
#include <QSettings>
#include <QString>
#include <QTimer>
#include <QtSensors/QHrmSensor>

#include "../logger.h"
#include "../../common.h"

#include "heartrateSensor.h"
#include "sampleTime.h"

// a batch is over when no reading came for this long
static const int BATCH_IDLE_MS = 2000;
static const qint64 MINUTE_MS = 60000;

HeartrateSensorPlugin::HeartrateSensorPlugin(QObject *parent, QSettings *settings, int interval) :
    QObject(parent), m_settings(settings)
{
    m_interval = qMax(60000, interval);
    // QHrmSensor::Status: 0 no contact, 1 unreliable, 2 low, 3 medium, 4 high
    // (the hub's accuracy 0..4, as the stock HAL maps it); medium and up is logged
    m_minStatus = qBound(1, m_settings->value("heartrateSensor/minStatus", 3).toInt(), 4);
    m_activeSpread = qMax(1, m_settings->value("heartrateSensor/activeSpread", 15).toInt());
    m_lastLoggedMs = m_settings->value("HeartratePrivate/lastReadingMs", 0).toLongLong();
    if (m_lastLoggedMs > QDateTime::currentMSecsSinceEpoch())
        m_lastLoggedMs = 0; // the clock went back

    setupFilePath(sensorPathPrefix);

    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &HeartrateSensorPlugin::retryStart);
    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    m_idleTimer->setInterval(BATCH_IDLE_MS);
    connect(m_idleTimer, &QTimer::timeout, this, &HeartrateSensorPlugin::batchIdle);

    if (m_settings->contains("heartrateSensor/continuous"))
        qDebug() << "heartrateSensor/continuous is no longer used: the heart rate is measured all the time";
    qDebug() << "heart rate sensor is enabled, one record per" << m_interval / 1000 << "s at rest";
    makeSensor();
    startSensor();
}

HeartrateSensorPlugin::~HeartrateSensorPlugin()
{
    closeInterval(); // what was collected so far is not lost on a restart
}

void HeartrateSensorPlugin::makeSensor()
{
    delete m_sensor;
    m_sensor = new QHrmSensor(this);
    m_sensor->setAlwaysOn(true); // sensorfw stops sensors in standby (display off) otherwise
    connect(m_sensor, &QSensor::readingChanged, this, &HeartrateSensorPlugin::readingReceived);
}

// QtSensors' sensorfw backend does not set a sensor up again once its first
// set-up failed (sensorfwd not ready): make a new one for every retry
void HeartrateSensorPlugin::startSensor()
{
    if (m_sensor->start() && m_sensor->isActive()) {
        m_retryDelay = 30000;
        return;
    }
    qWarning() << "heart rate sensor did not start, retrying in" << m_retryDelay / 1000 << "s";
    m_retryTimer->start(m_retryDelay);
    m_retryDelay = qMin(m_retryDelay * 2, 30 * 60 * 1000);
}

void HeartrateSensorPlugin::retryStart()
{
    makeSensor();
    startSensor();
}

void HeartrateSensorPlugin::readingReceived()
{
    QHrmReading *reading = m_sensor->reading();
    if (!reading)
        return;
    const qint64 taken = readingWallMs(reading->timestamp());
    const int bpm = reading->bpm();
    m_idleTimer->start();
    if (taken <= m_lastLoggedMs)
        return; // already in the files (sensorfw replays the last reading at a start)
    m_readings++;
    if (bpm <= 0 || (int)m_sensor->status() < m_minStatus) {
        m_unusable++;
        return;
    }
    const qint64 interval = taken / m_interval;
    if (m_open >= 0 && interval != m_open)
        closeInterval();
    m_open = interval;
    m_samples.append({taken, bpm});
}

void HeartrateSensorPlugin::batchIdle()
{
    // the interval the last batch ended in is complete once its time is over
    if (m_open >= 0 && (m_open + 1) * m_interval <= QDateTime::currentMSecsSinceEpoch())
        closeInterval();
}

void HeartrateSensorPlugin::timeUpdate()
{
    batchIdle();
}

void HeartrateSensorPlugin::triggerRecording()
{
    // asked over D-Bus: write what the open interval has so far
    closeInterval();
}

void HeartrateSensorPlugin::closeInterval()
{
    if (!m_samples.isEmpty()) {
        int lo = m_samples.first().bpm, hi = lo;
        for (const Sample &s : m_samples) {
            lo = qMin(lo, s.bpm);
            hi = qMax(hi, s.bpm);
        }
        const qint64 span = m_samples.last().takenMs - m_samples.first().takenMs;
        if (hi - lo > m_activeSpread && span >= 2 * MINUTE_MS) {
            // not at rest: one record per minute
            QVector<Sample> minute;
            qint64 current = m_samples.first().takenMs / MINUTE_MS;
            for (const Sample &s : m_samples) {
                if (s.takenMs / MINUTE_MS != current) {
                    writeRecord(minute);
                    minute.clear();
                    current = s.takenMs / MINUTE_MS;
                }
                minute.append(s);
            }
            writeRecord(minute);
        } else {
            writeRecord(m_samples);
        }
        m_lastLoggedMs = m_samples.last().takenMs;
        m_settings->setValue("HeartratePrivate/lastReadingMs", m_lastLoggedMs);
    } else if (m_readings) {
        qDebug() << "heart rate:" << m_readings << "readings," << m_unusable << "below the accuracy logged";
    }
    m_samples.clear();
    m_open = -1;
    m_readings = 0;
    m_unusable = 0;
}

// the median reading of a run, with the time it was taken
void HeartrateSensorPlugin::writeRecord(const QVector<Sample> &samples)
{
    if (samples.isEmpty())
        return;
    QVector<Sample> sorted = samples;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Sample &a, const Sample &b) { return a.bpm < b.bpm; });
    const Sample &median = sorted.at(sorted.size() / 2);
    fileAddRecord(sensorPathPrefix, QString::number(median.bpm), QDateTime::fromMSecsSinceEpoch(median.takenMs));
}
