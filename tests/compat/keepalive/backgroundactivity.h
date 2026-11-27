/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef COMPAT_BACKGROUNDACTIVITY_H
#define COMPAT_BACKGROUNDACTIVITY_H

#include <QList>
#include <QObject>

/*
 * A stand-in for libkeepalive's BackgroundActivity (nemo-keepalive, keepalive/backgroundactivity.h) for the host
 * tests: the part of its API the daemon uses, the same states and signals, no iphb and no mce. A test reads what was
 * asked (waits: the ranges in seconds, in order) and makes the time come (fire()).
 */
class BackgroundActivity : public QObject
{
    Q_OBJECT
public:
    enum State { Stopped, Waiting, Running };

    explicit BackgroundActivity(QObject *parent = nullptr);
    ~BackgroundActivity() override;

    bool isWaiting() const { return m_state == Waiting; }
    bool isRunning() const { return m_state == Running; }
    bool isStopped() const { return m_state == Stopped; }
    State state() const { return m_state; }
    void wait(int minDelay, int maxDelay = -1);

    // test side: the last one made, the ranges asked for, the time has come
    static BackgroundActivity *last;
    QList<QPair<int, int>> waits;
    int stops = 0;
    void fire();

public Q_SLOTS:
    void run();
    void stop();

Q_SIGNALS:
    void waiting();
    void running();
    void stopped();

private:
    State m_state = Stopped;
};

#endif // COMPAT_BACKGROUNDACTIVITY_H
