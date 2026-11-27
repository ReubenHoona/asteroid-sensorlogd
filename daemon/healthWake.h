/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HEALTHWAKE_H
#define HEALTHWAKE_H

#include <QObject>

/*
 * The Health monitor's own system wake (R5 E-04): the only way it looks at the data at a time of its choosing while
 * the system sleeps. Stock wakes the watch for these too: the reminder to move at minute 50 is an exact RTC wakeup
 * alarm (RtmManager.java:190-201, setExactAndAllowWhileIdle(RTC_WAKEUP)), and the high / low heart rate alerts are
 * events of the sensor hub's own detector (WHS HealthEvent ELEVATED_HR_DETECTED / LOW_HR_DETECTED), so they come
 * when the 10 inactive minutes are over. The watch's hub has no such event over USF (CHRE, WO-15), so the monitor
 * wakes, has the hub hand over its batch and looks.
 *
 *   schedule(at, slack)  wake the system between at and at + slack (ms since the epoch); replaces the request
 *                        before it; at < 0: no wake. While a wake runs, the new time takes effect at release().
 *   release()            the wake's work is done: the system may sleep again
 *   flushHub()           ask the sensor hub to hand over what it batched (sensorfwd's org.eos.UsfHub.Flush);
 *                        flushed(n) follows: n samplings flushed (their readings come as a batch), 0 nothing waits
 *                        (the display is on: everything is live), -1 no answer (no hub, no sensorfwd)
 *   woke()               the time has come; the system is held awake until release()
 *
 * The daemon's is KeepaliveWake (keepaliveWake.h: libkeepalive's BackgroundActivity, an iphb wakeup through dsme,
 * which mce holds awake while it runs); the tests' a fake with their own clock.
 */
class HealthWake : public QObject
{
    Q_OBJECT
public:
    explicit HealthWake(QObject *parent = nullptr) : QObject(parent) {}
    ~HealthWake() override {}

    virtual void schedule(qint64 atMs, int slackMs) = 0;
    virtual void release() = 0;
    virtual void flushHub() = 0;

signals:
    void woke();
    void flushed(int samplings);
};

#endif // HEALTHWAKE_H
