/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef KEEPALIVEWAKE_H
#define KEEPALIVEWAKE_H

#include "healthWake.h"

class BackgroundActivity;

/*
 * The daemon's HealthWake (healthWake.h): one libkeepalive BackgroundActivity. Waiting, it is an iphb wakeup in
 * dsme (an RTC wake from suspend, aligned with other wakes inside the slack: the same path btsyncd's and mce's
 * heartbeat wakes take); running, mce holds the system awake (a cpu-keepalive session) until release(). The hub's
 * batch is asked for on sensorfwd's hub connection (com.nokia.SensorService /org/eos/usfhub org.eos.UsfHub.Flush on
 * the system bus, the method the eos hub plugin has for readers that wake on their own schedule); the answer comes
 * asynchronously, the readings through sensorfw as any batch. Nothing runs between wakes: no timer, no D-Bus traffic.
 */
class KeepaliveWake : public HealthWake
{
    Q_OBJECT
public:
    explicit KeepaliveWake(QObject *parent = nullptr);
    ~KeepaliveWake() override;

    void schedule(qint64 atMs, int slackMs) override;
    void release() override;
    void flushHub() override;

private slots:
    void activityRunning();

private:
    void arm();

    BackgroundActivity *m_activity;
    qint64 m_atMs = -1;
    int m_slackMs = 0;
};

#endif // KEEPALIVEWAKE_H
