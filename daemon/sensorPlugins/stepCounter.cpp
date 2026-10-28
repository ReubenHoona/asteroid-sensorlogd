/*
 * Copyright (C) 2023 Arseniy Movshev <dodoradio@outlook.com>
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QString>
#include <QTime>
#include <QTimer>
#include <QtSensors/QStepCounterSensor>
#include <QSettings>

#include "../logger.h"
#include "../../common.h"

#include "stepCounter.h"
#include "sampleTime.h"

// a batch is over when no reading came for this long
static const int BATCH_IDLE_MS = 2000;

StepCounterPlugin::StepCounterPlugin(QObject* parent, int initInterval, bool daemonFresh)
    : QObject(parent)
{
    interval = qMax(60000, initInterval);
    m_daemonFresh = daemonFresh;
    m_settings = new QSettings("asteroid", "sensorlogd", this);

    qDebug() << "step counter sensor is enabled, the count is written at least every" << interval / 1000 << "s of steps";
    setupFilePath(sensorPathPrefix);

    m_day = QDate::fromString(m_settings->value("StepCounterPrivate/offsetDay").toString(), Qt::ISODate);
    if (!m_day.isValid())
        m_day = QDate::currentDate();
    if (!daemonFresh) {
        // restarted within the same boot: the counter went on counting, so the
        // stored offset holds - unless it is above the counter (checked on the
        // first reading)
        m_stepsOffset = m_settings->value("StepCounterPrivate/stepsOffset", 0).toInt();
        m_storedOffset = true;
    }
    // the offset is settled by the first reading

    m_retryDelay = 30000;
    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &StepCounterPlugin::retryStart);
    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    m_idleTimer->setInterval(BATCH_IDLE_MS);
    connect(m_idleTimer, &QTimer::timeout, this, &StepCounterPlugin::batchIdle);
    stepcounterSensor = nullptr;
    startSensor();
}

StepCounterPlugin::~StepCounterPlugin()
{
    writePending();
}

void StepCounterPlugin::startSensor()
{
    stepcounterSensor = new QStepCounterSensor(this);
    stepcounterSensor->setAlwaysOn(true); // sensorfw stops sensors in standby (display off) otherwise
    connect(stepcounterSensor, &QSensor::readingChanged, this, &StepCounterPlugin::readingReceived);
    if (stepcounterSensor->start() && stepcounterSensor->isActive()) {
        m_retryDelay = 30000;
        return;
    }
    qWarning() << "step counter did not start, retrying in" << m_retryDelay / 1000 << "s";
    m_retryTimer->start(m_retryDelay);
    m_retryDelay = qMin(m_retryDelay * 2, 30 * 60 * 1000);
}

void StepCounterPlugin::retryStart()
{
    // a sensorfw backend that failed to initialise does not try again by itself: make a new sensor
    delete stepcounterSensor;
    startSensor();
}

void StepCounterPlugin::saveOffset()
{
    m_settings->setValue("StepCounterPrivate/stepsOffset", m_stepsOffset);
    m_settings->setValue("StepCounterPrivate/offsetDay", m_day.toString(Qt::ISODate));
}

// the first reading: where the day's count starts on the counter
void StepCounterPlugin::settleOffset(int rawSteps)
{
    const QDate today = QDate::currentDate();
    if (m_storedOffset && m_stepsOffset <= rawSteps) {
        // same boot, the counter went on: keep the stored offset; one from an
        // earlier day goes on from that day's last count
        if (m_day < today) {
            m_stepsOffset += fileGetPrevRecord(sensorPathPrefix, QDateTime(m_day, QTime(23, 59, 59)))[1].toInt();
            m_day = today;
        }
    } else if (dayFileExists(sensorPathPrefix)) {
        // (also a stored offset above the counter: it was stored for another
        // count, e.g. by the previous boot) go on from today's last count;
        // steps taken while the daemon was not running are not counted
        m_stepsOffset = rawSteps - fileGetPrevRecord(sensorPathPrefix)[1].toInt();
        m_day = today;
    } else {
        // nothing logged today: the first daemon of this boot (the counter
        // started with it, the hub firmware is loaded at every boot) or a
        // counter that started again; its steps so far are today's
        m_stepsOffset = 0;
        m_day = today;
    }
    saveOffset();
    m_offsetReady = true;
    m_writtenSteps = dayFileExists(sensorPathPrefix) ? fileGetPrevRecord(sensorPathPrefix)[1].toInt() : -1;
    qDebug() << "first step counter reading" << rawSteps << "offset" << m_stepsOffset;
}

// the counter is on-change: its last reading of a day is the day's total
void StepCounterPlugin::newDay(const QDate &day)
{
    writePending();
    m_stepsOffset = m_lastRawSteps;
    m_day = day;
    m_writtenSteps = -1;
    m_open = -1;
    saveOffset();
    qDebug() << "new day" << day.toString(Qt::ISODate) << ": the count starts at counter value" << m_stepsOffset;
}

void StepCounterPlugin::readingReceived()
{
    QStepCounterReading *reading = stepcounterSensor->reading();
    if (!reading || reading->timestamp() == 0)
        return;
    const int rawSteps = reading->steps();
    const qint64 taken = readingWallMs(reading->timestamp());
    m_idleTimer->start();
    if (!m_offsetReady) {
        if (rawSteps > 0)
            m_lastRawSteps = rawSteps;
        settleOffset(rawSteps);
    } else {
        if (taken < m_lastReadingMs)
            return; // older than what was counted already (a replayed reading): changes nothing
        const QDate takenDay = QDateTime::fromMSecsSinceEpoch(taken).date();
        if (takenDay > m_day)
            newDay(takenDay);
        // The counter is cumulative; a lower value means it started again from
        // zero (sensor hub reset or firmware reload). Carry on from the count
        // so far instead of computing negative steps. A zero is not trusted as
        // a restart.
        if (rawSteps > 0 && rawSteps < m_lastRawSteps) {
            m_stepsOffset -= m_lastRawSteps;
            saveOffset();
            qWarning() << "step counter started again at" << rawSteps << "(was" << m_lastRawSteps << "), offset now" << m_stepsOffset;
        }
        if (rawSteps > 0)
            m_lastRawSteps = rawSteps;
        if (takenDay < m_day) {
            // Taken before the current day began, delivered after the day
            // changed (a hub batch that came only after the day was changed):
            // these steps belong to that day, and the current day starts
            // after them. Counted to the current day they were written into
            // the earlier day's file as a small count, read as its total.
            m_lastReadingMs = taken;
            if (rawSteps > m_stepsOffset) {
                const int dayTotal = qMax(0, fileGetPrevRecord(sensorPathPrefix,
                                                               QDateTime(takenDay, QTime(23, 59, 59)))[1].toInt());
                fileAddRecord(sensorPathPrefix, QString::number(dayTotal + rawSteps - m_stepsOffset),
                              QDateTime::fromMSecsSinceEpoch(taken));
                m_stepsOffset = rawSteps;
                saveOffset();
            }
            return;
        }
    }
    if (taken < m_lastReadingMs)
        return; // older than what was counted already (a replayed reading)
    m_lastReadingMs = taken;
    const int steps = rawSteps - m_stepsOffset;
    if (rawSteps <= 0 || steps < 0)
        return;
    const qint64 at = taken / interval;
    if (m_open >= 0 && at != m_open)
        writePending();
    m_open = at;
    m_pendingSteps = steps;
    m_pendingMs = taken;
}

void StepCounterPlugin::writePending()
{
    if (m_pendingSteps < 0)
        return;
    const int steps = m_pendingSteps;
    m_pendingSteps = -1;
    if (steps == m_writtenSteps)
        return;
    if (steps < m_writtenSteps) {
        qDebug() << "step count" << steps << "below the last written" << m_writtenSteps << "- not recording";
        return;
    }
    fileAddRecord(sensorPathPrefix, QString::number(steps), QDateTime::fromMSecsSinceEpoch(m_pendingMs));
    m_writtenSteps = steps;
}

void StepCounterPlugin::batchIdle()
{
    // the newest count of the batch, so the day file is current
    writePending();
    if (m_dayChangePending) {
        // no step since midnight: today starts at the last count
        m_dayChangePending = false;
        if (m_offsetReady && QDate::currentDate() > m_day)
            newDay(QDate::currentDate());
    }
}

void StepCounterPlugin::timeUpdate()
{
    writePending();
    if (m_offsetReady && QDate::currentDate() > m_day) {
        // Not at once: the display came on, and the sensor hub hands over
        // what it batched before midnight only now (sensorfwd flushes it on
        // the same mce signal; it arrives after this). A new day started
        // from the last count seen here would count those steps to today and
        // write them into the previous day's file as its total. Once the
        // batch is over (or none came), the day changes at the last count.
        m_dayChangePending = true;
        if (!m_idleTimer->isActive())
            m_idleTimer->start();
    }
}

void StepCounterPlugin::triggerRecording()
{
    writePending();
}
