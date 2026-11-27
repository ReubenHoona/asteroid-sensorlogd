/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SKINTEMPSENSOR_H
#define SKINTEMPSENSOR_H

#include <QList>
#include <QObject>
#include <QPair>
#include <QVector>

class QTimer;

namespace Health {
class HealthDb;
}

/*
 * The sensor hub's skin temperature (AS6221, USF 0x3d; M4 WO-13), for the night's skin temperature variation
 * (metrics/skinTemp.h). It runs only while the Health monitor asks (setWanted: inside a night's sleep): then this
 * subscribes to it on sensorfwd's hub connection (org.eos.UsfHub SubscribeSkinTemp on the system bus, again when
 * sensorfwd comes back) and unsubscribes when the monitor no longer wants it, or when this ends. The hub keeps it
 * non-wake-up and batched with the screen off, so its readings come with the heart rate's batch (the signal
 * ActivitySamples of type 0x3d, deg C); at the unsubscribe the hub hands over what it holds first. Nothing here
 * wakes the system or polls.
 *
 * The readings are on-change: one holds until the next, HOLD_MS at most. health.db skin_temp gets a row a minute
 * (t_ms: the minute's start, c: the time-weighted mean of the readings in force in it, 0.01 deg C), for each minute
 * with MIN_COVER_MS of them, written when the minute is over (one transaction a delivery; no change of seq, no
 * HealthChanged: a sample table). A reading outside SENSOR_MIN_C..SENSOR_MAX_C is no temperature and is dropped.
 * Settings: skinTemp/enabled (true).
 */
class SkinTempPlugin : public QObject {
    Q_OBJECT
public:
    static const int SKIN_TEMP = 0x3d;              // the USF sensor type
    static const int SETTLE_MS = 300;               // the signals of one delivery are in
    static const int STOP_SETTLE_MS = 1500;         // after the unsubscribe: the hub's last batch is in
    static const qint64 HOLD_MS = 5 * 60000;        // an on-change reading holds this long at most (guess)
    static const qint64 MIN_COVER_MS = 30000;       // a minute with less time covered is not written
    static constexpr double SENSOR_MIN_C = 5.0;
    static constexpr double SENSOR_MAX_C = 50.0;

    explicit SkinTempPlugin(Health::HealthDb *store, QObject *parent = nullptr);
    ~SkinTempPlugin() override;

    bool wanted() const { return m_wanted; }
    // sensorfwd came back: its hub connection knows nothing of us
    void resubscribe();
    // since the start, for the debug output and the tests (no values)
    int minutesWritten() const { return m_written; }
    int readingsDropped() const { return m_dropped; }

public slots:
    // the Health monitor: inside a night's sleep or not
    void setWanted(bool on);
    // the hub's samples of one stream (CLOCK_BOOTTIME microseconds, values[0]); only type 0x3d is taken
    void samples(int type, const QList<qlonglong> &timesUs, const QList<double> &values);

private slots:
    void settled();

private:
    void call(const char *method);
    // the reading in force counted up to toMs; the minutes that are over made rows
    void advance(qint64 toMs);
    void closeMinute();
    // the reading in force ends at toMs (no later reading came): counted to it, at most to its hold
    void endReading(qint64 toMs);
    void writeRows();

    Health::HealthDb *m_store;
    bool m_wanted = false;
    qint64 m_stopMs = 0;     // the unsubscribe, until the last batch is in
    QVector<QPair<qint64, double>> m_pending; // the readings of a delivery (wall ms, deg C)
    bool m_have = false;     // a reading in force
    qint64 m_readingMs = 0;
    double m_readingC = 0;
    qint64 m_countedMs = 0;  // counted up to here
    qint64 m_minute = -1;    // the minute being summed
    double m_sum = 0;        // deg C x ms
    qint64 m_cover = 0;      // ms
    qint64 m_lastRowMinute = -1;
    QVector<QPair<qint64, double>> m_rows;
    QTimer *m_settle;
    int m_written = 0;
    int m_dropped = 0;
};

#endif // SKINTEMPSENSOR_H
