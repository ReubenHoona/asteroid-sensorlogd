/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef OFFBODY_H
#define OFFBODY_H

#include <QObject>

/*
 * The wrist, from the sensor hub's low latency off-body sensor (M4 WO-03). sensorfwd's hub connection runs it as a
 * wake-up on-change sampling, as stock sysui's LlobDetector registers it, and says the state on the system bus:
 * org.eos.UsfHub GetOffBody() -> i and the signal OffBodyChanged(i), -1 unknown (the provider is off in
 * eos-usf.conf, the hub is not up, no sample yet), 0 on the wrist, 1 off it. This plugin asks at the start and
 * whenever sensorfwd comes back, follows the signal and logs every change, so that what is estimated later from the
 * logs (the night's sleep, the day's resting heart rate) knows when the watch was off the wrist:
 *
 *   offBody/   a record when the state changes: 1 off the wrist, 0 on the wrist, -1 unknown again (after a known
 *              state: the hub went away, the provider was switched off). Dated when the change came: the sensor
 *              wakes the system for it and sensorfwd holds it awake until the signal is out (usfhubqt.cpp), so that
 *              is within about a second of the change. An unknown state with nothing known before it is not logged.
 *
 * The Health monitor takes the state as its "worn" (healthMonitor.h). Nothing here wakes the system or polls; the
 * wakes are the sensor's own, one per change (eos-usf.conf `offbody`). Settings: offBody/enabled (true).
 */
class OffBodyPlugin : public QObject {
    Q_OBJECT
public:
    static const int UNKNOWN = -1, ON_WRIST = 0, OFF_WRIST = 1;

    explicit OffBodyPlugin(QObject *parent = nullptr);

    // -1 unknown, 0 on the wrist, 1 off it
    int state() const { return m_state; }
    // asks sensorfwd's hub connection for the state (at the start, and again when sensorfwd came back)
    void query();

signals:
    // the state changed (atMs: when it came, ms since the epoch)
    void changed(int state, qint64 atMs);

public slots:
    // the hub connection's OffBodyChanged signal
    void offBodyChanged(int state);

private:
    void set(int state, qint64 atMs);

    int m_state = UNKNOWN;
    int m_logged = -2; // the state of the last record (-2: none today)
};

#endif // OFFBODY_H
