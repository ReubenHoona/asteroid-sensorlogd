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
#include <QTimer>
#include <QVector>

#include <QtSensors/QHrmSensor>

#include "heartrateDb.h"

class QSettings;

/*
 * Heart rate, measured all the time by the sensor (on eos: the sensor hub
 * runs the algorithm and batches the readings while the system sleeps, as
 * on Wear OS). The sensor is started once and stays on; readings arrive
 * live or in batches, each with the time it was taken.
 *
 * Every reading with a heart rate goes to the heart rate database
 * (heartrateDb.h), with its time and accuracy: a batch in one transaction
 * when it is over, live readings once a minute and when the display goes
 * off. Readings that come late, out of order or twice are kept once each,
 * in time order.
 *
 * The day files keep one record per interval (heartrateSensor/interval) for
 * the Health app, dated by its reading: the median usable reading of that
 * interval. When the heart rate moves within an interval (activity: more
 * than heartrateSensor/activeSpread bpm between its lowest and highest
 * reading) a record per minute is written instead.
 *
 * Nothing here wakes the system: everything is written when readings arrive.
 */
class HeartrateSensorPlugin : public QObject
{
    Q_OBJECT
public:
    explicit HeartrateSensorPlugin(QObject *parent, QSettings *settings, int interval);
    virtual ~HeartrateSensorPlugin();

    // the system is awake anyway (display on): close an interval that is over
    void timeUpdate();
    // the display goes off: the live readings so far go to the database
    // before the system may sleep
    void displayOff();

public slots:
    void triggerRecording();

private slots:
    void readingReceived();
    void retryStart();
    void batchIdle();
    void commitReadings();

private:
    struct Sample {
        qint64 takenMs; // wall clock of the reading
        int bpm;
    };
    void makeSensor();
    void startSensor();
    void keepReading(qint64 takenMs, int bpm, int status);
    void closeInterval();
    void writeRecord(const QVector<Sample> &samples);

    QSettings *m_settings;
    QHrmSensor *m_sensor = nullptr;
    QTimer *m_retryTimer;
    int m_retryDelay = 30000;
    QTimer *m_idleTimer;        // after the last reading of a batch
    qint64 m_interval;          // [ms]
    int m_minStatus;            // QHrmSensor::Status, 3 = AccuracyMedium
    int m_activeSpread;         // [bpm]
    qint64 m_lastLoggedMs = 0;  // readings up to here are in the files (sensorfw replays the last one)
    qint64 m_open = -1;         // the interval being collected (takenMs / m_interval)
    QVector<Sample> m_samples;  // its usable readings
    int m_readings = 0;         // all readings since the last record, for the journal
    int m_unusable = 0;

    HeartrateDb m_db;
    QVector<HeartrateDb::Reading> m_pending; // readings not in the database yet
    QTimer *m_commitTimer;

    const QString sensorPathPrefix = "heartrateMonitor";
};

#endif // HEARTRATESENSOR_H
