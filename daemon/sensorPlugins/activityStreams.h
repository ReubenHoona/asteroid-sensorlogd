/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef ACTIVITYSTREAMS_H
#define ACTIVITYSTREAMS_H

#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

class QTimer;

/*
 * The sensor hub's motion detect, stationary detect and step cadence (M4 WO-05), for the still and moving
 * intervals of the sleep estimate (WO-06) and the cadence minutes of exercise detection and auto-pause (WO-07,
 * WO-15). sensorfwd's hub connection runs them while this subscribes (org.eos.UsfHub.SubscribeActivity on the
 * system bus, at the start and whenever sensorfwd comes back) and sends what the hub delivered as the signal
 * ActivitySamples(i type, ax times_us, ad values). The hub keeps them non-wake-up: they come with the heart rate's
 * batch, or live with the display on. Nothing here wakes the system or polls.
 *
 * Day files (local days, "<seconds since the epoch>:<value>" lines, in time order):
 *   motion/       a record when the state changes: 1 movement began, 0 stillness began (the hub's motion and
 *                 stationary detect, both one-shot), 2 the detectors were armed again: what happened between the
 *                 record before and this one was not watched (the hub's one-shot detector had reported while
 *                 the system slept and could only be started again when the system had its event)
 *   stepCadence/  a record a minute with steps: the minute's start and its mean cadence in steps a minute (the
 *                 hub reports on change; a reading holds until the next one, CADENCE_HOLD_MS at most)
 * Settings: activity/enabled (true).
 */
class ActivityStreamsPlugin : public QObject {
    Q_OBJECT
public:
    static const int SETTLE_MS = 300;           // the signals of one delivery are in (one per stream)
    static const qint64 CADENCE_HOLD_MS = 60000; // an on-change cadence reading holds this long at most
    static const qint64 MINUTE_MS = 60000;
    // USF sensor types
    static const int MOTION = 0x20, STATIONARY = 0x21, CADENCE = 0x3a;

    explicit ActivityStreamsPlugin(QObject *parent = nullptr);
    ~ActivityStreamsPlugin() override;

    // asks sensorfwd's hub connection for the streams (again: sensorfwd may have restarted)
    void subscribe();

public slots:
    // the hub's samples of one stream (CLOCK_BOOTTIME microseconds, values[0])
    void samples(int type, const QList<qlonglong> &timesUs, const QList<double> &values);

private slots:
    void settled();

private:
    struct Change {
        qint64 ms;
        int state;
    };
    void cadence(qint64 takenMs, double spm);
    // the cadence reading in force counted up to toMs; the minutes that are over written
    void advanceCadence(qint64 toMs);
    void writeMinute();

    QVector<Change> m_pending;  // motion records of the delivery, written in time order once it is in
    int m_lastState = -1;
    qint64 m_lastMotionMs = 0;
    QTimer *m_settle;
    // cadence: the reading in force and the minute being summed
    bool m_haveCadence = false;
    qint64 m_cadenceMs = 0;     // the reading's time; counted up to m_countedMs
    qint64 m_countedMs = 0;
    double m_cadenceSpm = 0;
    qint64 m_minute = -1;
    double m_minuteSum = 0;     // steps a minute x ms within the minute
    qint64 m_lastCadenceMs = 0;
};

#endif // ACTIVITYSTREAMS_H
