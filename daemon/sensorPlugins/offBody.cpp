/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QDebug>

#include "../logger.h"
#include "../../common.h"

#include "offBody.h"

static const char *HUB_SERVICE = "com.nokia.SensorService";
static const char *HUB_PATH = "/org/eos/usfhub";
static const char *HUB_INTERFACE = "org.eos.UsfHub";
static const QString offBodyPrefix = QStringLiteral("offBody");

OffBodyPlugin::OffBodyPlugin(QObject *parent) : QObject(parent)
{
    setupFilePath(offBodyPrefix);
    // what today's file says last is not written again
    const QStringList last = fileGetPrevRecord(offBodyPrefix);
    if (last.value(0).toLongLong() > 0)
        m_logged = last.value(1).toInt();
    // by the well-known name: the connection follows sensorfwd when it restarts
    if (!QDBusConnection::systemBus().connect(QLatin1String(HUB_SERVICE), QLatin1String(HUB_PATH),
                                              QLatin1String(HUB_INTERFACE), QStringLiteral("OffBodyChanged"), this,
                                              SLOT(offBodyChanged(int))))
        qWarning() << "off-body: cannot connect to the sensor hub's OffBodyChanged";
    query();
}

void OffBodyPlugin::query()
{
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(HUB_SERVICE), QLatin1String(HUB_PATH),
                                                    QLatin1String(HUB_INTERFACE), QStringLiteral("GetOffBody"));
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *pw) {
        QDBusPendingReply<int> r = *pw;
        if (r.isError()) {
            // an older sensorfwd without the provider, or none: unknown (the monitor goes by the heart rate then)
            qDebug() << "off-body: the sensor hub has no state for us:" << r.error().message();
            set(UNKNOWN, QDateTime::currentMSecsSinceEpoch());
        } else {
            set(r.value(), QDateTime::currentMSecsSinceEpoch());
        }
        pw->deleteLater();
    });
}

void OffBodyPlugin::offBodyChanged(int state)
{
    set(state, QDateTime::currentMSecsSinceEpoch());
}

void OffBodyPlugin::set(int state, qint64 atMs)
{
    if (state != ON_WRIST && state != OFF_WRIST)
        state = UNKNOWN;
    // a record for a change of what the file says; an unknown state with nothing known before it says nothing
    if (state != m_logged && !(state == UNKNOWN && (m_logged == -2 || m_logged == UNKNOWN))) {
        fileAddRecord(offBodyPrefix, QString::number(state), QDateTime::fromMSecsSinceEpoch(atMs));
        m_logged = state;
    }
    if (state == m_state)
        return;
    m_state = state;
    qDebug() << "off-body:" << (state == OFF_WRIST ? "off the wrist" : state == ON_WRIST ? "on the wrist" : "unknown");
    emit changed(state, atMs);
}
