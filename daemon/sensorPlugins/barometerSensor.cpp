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
#include <QTimer>
#include <QtSensors/QPressureSensor>
#include <QDebug>
#include <QString>
#include <MGConfItem>

#include "../logger.h"
#include "../../common.h"

#include "barometerSensor.h"
#include "sampleTime.h"

BarometerSensorPlugin::BarometerSensorPlugin(QObject *parent, int recordInterval)  :
    QObject(parent){
    m_interval = qMax(10000, recordInterval);

    setupFilePath(sensorPathPrefix);
    setupFilePath(sensorPathPrefixCompensated);
    m_barometerOffsetGconf = new MGConfItem("/org/asteroidos/sensors/barometer-offset", this);

    // what is in today's file already is not written again (a restart; sensorfw replays the last reading)
    const QStringList last = fileGetPrevRecord(sensorPathPrefix);
    m_lastMs = last.value(0).toLongLong() * 1000;
    if (m_lastMs > QDateTime::currentMSecsSinceEpoch())
        m_lastMs = 0; // the clock went back

    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &BarometerSensorPlugin::retryStart);

    qDebug() << "barometer sensor is enabled, one record per" << m_interval / 1000 << "s, all the time";
    makeSensor();
    startSensor();
}

BarometerSensorPlugin::~BarometerSensorPlugin()
{
    writeInterval(); // what was collected so far is not lost on a restart
}

void BarometerSensorPlugin::makeSensor()
{
    delete barometerSensor;
    barometerSensor = new QPressureSensor(this);
    barometerSensor->setAlwaysOn(true); // sensorfw stops sensors in standby (display off) otherwise
    connect(barometerSensor, &QSensor::readingChanged, this, &BarometerSensorPlugin::readingReceived);
}

// QtSensors' sensorfw backend does not set a sensor up again once its first
// set-up failed (sensorfwd not ready): a new one for every retry
void BarometerSensorPlugin::startSensor()
{
    if (barometerSensor->start() && barometerSensor->isActive()) {
        m_retryDelay = 30000;
        return;
    }
    qWarning() << "barometer did not start, retrying in" << m_retryDelay / 1000 << "s";
    m_retryTimer->start(m_retryDelay);
    m_retryDelay = qMin(m_retryDelay * 2, 30 * 60 * 1000);
}

void BarometerSensorPlugin::retryStart()
{
    makeSensor();
    startSensor();
}

void BarometerSensorPlugin::timeUpdate()
{
    if (!barometerSensor->isActive() && !m_retryTimer->isActive())
        retryStart();
}

void BarometerSensorPlugin::readingReceived()
{
    QPressureReading *reading = barometerSensor->reading();
    if (!reading)
        return;
    const qint64 taken = readingWallMs(reading->timestamp());
    const int pressure = (int)reading->pressure();
    if (pressure <= 0)
        return; // no usable reading (the sensor's accuracy)
    if (taken <= m_lastMs)
        return; // already looked at (a replay at a start, a reading out of order)
    m_lastMs = taken;
    const qint64 interval = taken / m_interval;
    if (m_open >= 0 && interval != m_open)
        writeInterval();
    m_open = interval;
    m_values.append(pressure);
    m_openLastMs = taken;
}

void BarometerSensorPlugin::triggerRecording()
{
    writeInterval();
}

void BarometerSensorPlugin::writeInterval()
{
    if (m_values.isEmpty()) {
        m_open = -1;
        return;
    }
    QVector<int> v = m_values;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    const int median = v[v.size() / 2];
    const QDateTime at = QDateTime::fromMSecsSinceEpoch(m_openLastMs);
    fileAddRecord(sensorPathPrefix, QString::number(median), at);
    fileAddRecord(sensorPathPrefixCompensated, QString::number(median - m_barometerOffsetGconf->value(0).toInt()), at);
    qDebug() << "barometer: record of" << m_values.size() << "readings";
    m_values.clear();
    m_open = -1;
}
