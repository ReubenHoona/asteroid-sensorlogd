/*
 * Copyright (C) 2023 Arseniy Movshev <dodoradio@outlook.com>
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef STEPCOUNTER_H
#define STEPCOUNTER_H

#include <QObject>
#include <QDate>
#include <QDateTime>
#include <QTimer>
#include <QSettings>

#include <QtSensors/QStepCounterSensor>

/*
 * Steps, counted all the time by the sensor (on eos: the sensor hub, which
 * batches the counter readings while the system sleeps, as on Wear OS). The
 * sensor is started once and stays on. The day's count (counter minus the
 * day's offset) is written dated by the reading it comes from: the last
 * reading of each interval (stepCounter/interval) and the newest one at the
 * end of every batch, when it changed. At a new day the previous day's
 * total is written at 23:59:59 and the count starts again. Nothing here
 * wakes the system.
 */
class StepCounterPlugin : public QObject {
    Q_OBJECT
public:
    explicit StepCounterPlugin(QObject* parent = 0, int initInterval = 600000, bool daemonFresh = false);
    virtual ~StepCounterPlugin();

    // the system is awake anyway (display on): a new day, a pending count
    void timeUpdate();

public slots:
    void triggerRecording();

private slots:
    void readingReceived();
    void retryStart();
    void batchIdle();

private:
    void startSensor();
    void saveOffset();
    void settleOffset(int rawSteps);
    void newDay(const QDate &day);
    void writePending();

    bool m_daemonFresh;
    bool m_offsetReady = false; // m_stepsOffset is known (needs a first reading)
    bool m_storedOffset = false; // m_stepsOffset was read from the settings (restart within the same boot)
    int m_lastRawSteps = 0; // the counter's last non-zero value, to notice it starting again
    int m_retryDelay;
    QTimer *m_retryTimer;
    QTimer *m_idleTimer;
    qint64 interval;
    QStepCounterSensor *stepcounterSensor;
    int m_stepsOffset = 0;
    QDate m_day;                // the day m_stepsOffset counts from
    qint64 m_open = -1;         // interval of the pending count
    int m_pendingSteps = -1;    // newest count not written yet (-1: none)
    qint64 m_pendingMs = 0;
    int m_writtenSteps = -1;    // the count last written
    bool m_dayChangePending = false; // display on in a new day: change it once the batch is in
    qint64 m_lastReadingMs = 0; // newest reading taken into account
    QSettings *m_settings;

    const QString sensorPathPrefix = "stepCounter";
};

#endif // STEPCOUNTER_H
