/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cmath>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QDebug>
#include <QTimer>

#include "../logger.h"
#include "../../common.h"

#include "activityStreams.h"
#include "sampleTime.h"

static const char *HUB_SERVICE = "com.nokia.SensorService";
static const char *HUB_PATH = "/org/eos/usfhub";
static const char *HUB_INTERFACE = "org.eos.UsfHub";
static const QString motionPrefix = QStringLiteral("motion");
static const QString cadencePrefix = QStringLiteral("stepCadence");

ActivityStreamsPlugin::ActivityStreamsPlugin(QObject *parent) : QObject(parent)
{
    setupFilePath(motionPrefix);
    setupFilePath(cadencePrefix);
    // what is in today's files already is not written again
    m_lastMotionMs = fileGetPrevRecord(motionPrefix).value(0).toLongLong() * 1000;
    m_lastCadenceMs = fileGetPrevRecord(cadencePrefix).value(0).toLongLong() * 1000;
    m_settle = new QTimer(this);
    m_settle->setSingleShot(true);
    m_settle->setInterval(SETTLE_MS);
    connect(m_settle, &QTimer::timeout, this, &ActivityStreamsPlugin::settled);
    // by the well-known name: the connection follows sensorfwd when it restarts
    if (!QDBusConnection::systemBus().connect(QLatin1String(HUB_SERVICE), QLatin1String(HUB_PATH),
                                              QLatin1String(HUB_INTERFACE), QStringLiteral("ActivitySamples"), this,
                                              SLOT(samples(int, QList<qlonglong>, QList<double>))))
        qWarning() << "activity: cannot connect to the sensor hub's ActivitySamples";
    subscribe();
}

ActivityStreamsPlugin::~ActivityStreamsPlugin()
{
    settled();
    if (m_haveCadence)
        advanceCadence(std::min(QDateTime::currentMSecsSinceEpoch(), m_cadenceMs + CADENCE_HOLD_MS));
    writeMinute(); // the minute that is open, as far as it went
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(HUB_SERVICE), QLatin1String(HUB_PATH),
                                                    QLatin1String(HUB_INTERFACE), QStringLiteral("UnsubscribeActivity"));
    QDBusConnection::systemBus().send(m);
}

void ActivityStreamsPlugin::subscribe()
{
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(HUB_SERVICE), QLatin1String(HUB_PATH),
                                                    QLatin1String(HUB_INTERFACE), QStringLiteral("SubscribeActivity"));
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [](QDBusPendingCallWatcher *pw) {
        QDBusPendingReply<int> r = *pw;
        if (r.isError())
            qDebug() << "activity: the sensor hub's streams are not there:" << r.error().message();
        else
            qDebug() << "activity: the sensor hub runs" << r.value() << "streams for us";
        pw->deleteLater();
    });
}

void ActivityStreamsPlugin::samples(int type, const QList<qlonglong> &timesUs, const QList<double> &values)
{
    const int n = std::min(timesUs.size(), values.size());
    for (int i = 0; i < n; i++) {
        const qint64 taken = readingWallMs((quint64)timesUs[i]);
        const double v = values[i];
        if (type == CADENCE) {
            cadence(taken, v);
        } else if (type == MOTION || type == STATIONARY) {
            int state = v < 0 ? 2 : type == MOTION ? 1 : 0;
            m_pending.append({taken, state});
        }
    }
    m_settle->start();
}

void ActivityStreamsPlugin::settled()
{
    if (!m_pending.isEmpty()) {
        std::stable_sort(m_pending.begin(), m_pending.end(),
                         [](const Change &a, const Change &b) { return a.ms < b.ms; });
        for (const Change &c : m_pending) {
            if (c.ms <= m_lastMotionMs)
                continue; // written already (a restart) or out of order
            if (c.state == m_lastState)
                continue; // the same state again (an arming after an arming, a second report)
            fileAddRecord(motionPrefix, QString::number(c.state), QDateTime::fromMSecsSinceEpoch(c.ms));
            m_lastState = c.state;
            m_lastMotionMs = c.ms;
        }
        m_pending.clear();
    }
    // a cadence reading whose hold is over is counted to its end (the minutes up to it are complete)
    if (m_haveCadence && QDateTime::currentMSecsSinceEpoch() >= m_cadenceMs + CADENCE_HOLD_MS)
        advanceCadence(m_cadenceMs + CADENCE_HOLD_MS);
}

void ActivityStreamsPlugin::cadence(qint64 takenMs, double spm)
{
    if (!std::isfinite(spm) || spm < 0)
        spm = 0;
    if (m_haveCadence && takenMs < m_cadenceMs)
        return; // out of order
    if (m_haveCadence)
        advanceCadence(std::min(takenMs, m_cadenceMs + CADENCE_HOLD_MS));
    m_haveCadence = true;
    m_cadenceMs = takenMs;
    m_countedMs = takenMs;
    m_cadenceSpm = spm;
    if (m_minute < 0 || takenMs / MINUTE_MS != m_minute) {
        writeMinute();
        m_minute = takenMs / MINUTE_MS;
    }
}

void ActivityStreamsPlugin::advanceCadence(qint64 toMs)
{
    while (m_countedMs < toMs) {
        const qint64 minute = m_countedMs / MINUTE_MS;
        if (minute != m_minute) {
            writeMinute();
            m_minute = minute;
        }
        const qint64 end = std::min(toMs, (minute + 1) * MINUTE_MS);
        m_minuteSum += m_cadenceSpm * (double)(end - m_countedMs);
        m_countedMs = end;
        if (end == (minute + 1) * MINUTE_MS) {
            writeMinute();
            m_minute = minute + 1;
        }
    }
}

void ActivityStreamsPlugin::writeMinute()
{
    if (m_minute >= 0) {
        const qint64 startMs = m_minute * MINUTE_MS;
        const int spm = (int)std::lround(m_minuteSum / (double)MINUTE_MS);
        if (spm >= 1 && startMs > m_lastCadenceMs) {
            fileAddRecord(cadencePrefix, QString::number(spm), QDateTime::fromMSecsSinceEpoch(startMs));
            m_lastCadenceMs = startMs;
        }
    }
    m_minuteSum = 0;
}
