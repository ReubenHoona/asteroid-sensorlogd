/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <QDateTime>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>

#include <keepalive/backgroundactivity.h>

#include "keepaliveWake.h"

KeepaliveWake::KeepaliveWake(QObject *parent) : HealthWake(parent), m_activity(new BackgroundActivity(this))
{
    connect(m_activity, &BackgroundActivity::running, this, &KeepaliveWake::activityRunning);
}

KeepaliveWake::~KeepaliveWake()
{
    m_activity->stop();
}

void KeepaliveWake::schedule(qint64 atMs, int slackMs)
{
    m_atMs = atMs;
    m_slackMs = qMax(0, slackMs);
    // a running wake keeps the system up until release(), which arms the new time
    if (!m_activity->isRunning())
        arm();
}

void KeepaliveWake::release()
{
    if (m_activity->isRunning())
        arm();
}

void KeepaliveWake::arm()
{
    // a new range takes effect on a state change only (libkeepalive): stopped first
    if (!m_activity->isStopped())
        m_activity->stop();
    if (m_atMs < 0)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int from = (int)qBound<qint64>(1, (m_atMs - now + 999) / 1000, 7LL * 24 * 3600);
    const int to = from + qMax(1, m_slackMs / 1000);
    m_activity->wait(from, to);
}

void KeepaliveWake::activityRunning()
{
    qDebug() << "health: woke for a look at the data";
    emit woke();
}

void KeepaliveWake::flushHub()
{
    QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("com.nokia.SensorService"), QStringLiteral("/org/eos/usfhub"),
                                                    QStringLiteral("org.eos.UsfHub"), QStringLiteral("Flush"));
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<int> reply = *w;
        w->deleteLater();
        emit flushed(reply.isError() ? -1 : reply.value());
    });
}
