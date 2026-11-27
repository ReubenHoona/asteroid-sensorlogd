/*
 * Copyright (C) 2023 Arseniy Movshev <dodoradio@outlook.com>
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef BAROMETERSENSOR_H
#define BAROMETERSENSOR_H

#include <QObject>
#include <QTimer>
#include <QVector>
#include <MGConfItem>

#include <QtSensors/QPressureSensor>

/*
 * Air pressure, measured all the time by the sensor (on eos: the sensor hub
 * samples it at 1 Hz, its slowest rate, and with the display off keeps the
 * readings for the same batch latency as heart rate and steps; non-wake-up,
 * so they come with the heart rate's batch and never wake the system, M4
 * WO-05). The sensor is started once and stays on, so floors are counted in a
 * pocket too (before, it was read every ten minutes while the system happened
 * to be awake).
 *
 * One record an interval (barometer/recordInterval, 60 s), dated by the last
 * reading of the interval: the median of its readings, in Pa, in
 * barometer-raw and, less the dconf calibration offset, in
 * barometer-compensated. An interval is written when a reading of a later one
 * shows it is over (live or in a batch), on triggerRecording() and when the
 * plugin goes (SIGTERM). Readings older than the last one taken are not
 * looked at (sensorfw replays the last reading at a start). Nothing here
 * wakes the system.
 */
class BarometerSensorPlugin : public QObject {
    Q_OBJECT
public:
    explicit BarometerSensorPlugin(QObject* parent = 0, int recordInterval = 60000);
    virtual ~BarometerSensorPlugin();

    // the system is awake anyway (display on): nothing to do, the readings come by themselves
    void timeUpdate();

public slots:
    // the interval collected so far is written now
    void triggerRecording();

private slots:
    void readingReceived();
    void retryStart();

private:
    void makeSensor();
    void startSensor();
    void writeInterval();

    QPressureSensor *barometerSensor = nullptr;
    QTimer *m_retryTimer;
    int m_retryDelay = 30000;
    qint64 m_interval;          // [ms]
    qint64 m_open = -1;         // the interval being collected (takenMs / m_interval)
    QVector<int> m_values;      // its readings [Pa]
    qint64 m_openLastMs = 0;    // the time of its last reading
    qint64 m_lastMs = 0;        // the newest reading taken into account
    MGConfItem *m_barometerOffsetGconf;

    const QString sensorPathPrefix = "barometer-raw";
    const QString sensorPathPrefixCompensated = "barometer-compensated";
};

#endif // BAROMETERSENSOR_H
