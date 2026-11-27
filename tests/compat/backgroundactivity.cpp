/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "keepalive/backgroundactivity.h"

BackgroundActivity *BackgroundActivity::last = nullptr;

BackgroundActivity::BackgroundActivity(QObject *parent) : QObject(parent)
{
    last = this;
}

BackgroundActivity::~BackgroundActivity()
{
    if (last == this)
        last = nullptr;
}

void BackgroundActivity::wait(int minDelay, int maxDelay)
{
    waits.append(qMakePair(minDelay, maxDelay));
    m_state = Waiting;
    emit waiting();
}

void BackgroundActivity::run()
{
    m_state = Running;
    emit running();
}

void BackgroundActivity::stop()
{
    stops++;
    m_state = Stopped;
    emit stopped();
}

void BackgroundActivity::fire()
{
    if (m_state == Waiting)
        run();
}
