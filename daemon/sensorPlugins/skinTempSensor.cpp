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

#include "../../store/healthDb.h"

#include "sampleTime.h"
#include "skinTempSensor.h"

static const char *HUB_SERVICE = "com.nokia.SensorService";
static const char *HUB_PATH = "/org/eos/usfhub";
static const char *HUB_INTERFACE = "org.eos.UsfHub";
static const qint64 MINUTE_MS = 60000;

SkinTempPlugin::SkinTempPlugin(Health::HealthDb *store, QObject *parent) : QObject(parent), m_store(store)
{
    m_settle = new QTimer(this);
    m_settle->setSingleShot(true);
    connect(m_settle, &QTimer::timeout, this, &SkinTempPlugin::settled);
    // by the well-known name: the connection follows sensorfwd when it restarts
    if (!QDBusConnection::systemBus().connect(QLatin1String(HUB_SERVICE), QLatin1String(HUB_PATH),
                                              QLatin1String(HUB_INTERFACE), QStringLiteral("ActivitySamples"), this,
                                              SLOT(samples(int, QList<qlonglong>, QList<double>))))
        qWarning() << "skin temperature: cannot connect to the sensor hub's ActivitySamples";
}

SkinTempPlugin::~SkinTempPlugin()
{
    if (m_wanted) {
        QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(HUB_SERVICE), QLatin1String(HUB_PATH),
                                                        QLatin1String(HUB_INTERFACE),
                                                        QStringLiteral("UnsubscribeSkinTemp"));
        QDBusConnection::systemBus().send(m);
    }
    m_wanted = false;
    m_stopMs = QDateTime::currentMSecsSinceEpoch();
    settled(); // what came, and the reading in force up to now
}

void SkinTempPlugin::call(const char *method)
{
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(HUB_SERVICE), QLatin1String(HUB_PATH),
                                                    QLatin1String(HUB_INTERFACE), QLatin1String(method));
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m), this);
    const QByteArray name(method);
    connect(w, &QDBusPendingCallWatcher::finished, this, [name](QDBusPendingCallWatcher *pw) {
        QDBusPendingReply<int> r = *pw;
        if (r.isError())
            qDebug() << "skin temperature:" << name << "failed:" << r.error().message();
        else if (name == "SubscribeSkinTemp" && r.value() < 0)
            qDebug() << "skin temperature: off in the sensor hub's configuration";
        pw->deleteLater();
    });
}

void SkinTempPlugin::setWanted(bool on)
{
    if (on == m_wanted)
        return;
    m_wanted = on;
    if (on) {
        m_stopMs = 0;
        call("SubscribeSkinTemp");
    } else {
        // the hub hands over what it holds before it stops: counted once that is in
        m_stopMs = QDateTime::currentMSecsSinceEpoch();
        call("UnsubscribeSkinTemp");
        m_settle->start(STOP_SETTLE_MS);
    }
    qDebug() << "skin temperature:" << (on ? "on" : "off") << "- minutes written so far" << m_written;
}

void SkinTempPlugin::resubscribe()
{
    if (m_wanted)
        call("SubscribeSkinTemp");
}

void SkinTempPlugin::samples(int type, const QList<qlonglong> &timesUs, const QList<double> &values)
{
    if (type != SKIN_TEMP)
        return;
    const int n = std::min(timesUs.size(), values.size());
    for (int i = 0; i < n; i++) {
        const double c = values[i];
        if (!std::isfinite(c) || c < SENSOR_MIN_C || c > SENSOR_MAX_C) {
            m_dropped++;
            continue;
        }
        m_pending.append(qMakePair(readingWallMs((quint64)timesUs[i]), c));
    }
    // after an unsubscribe the flushed batch closes the night: keep the longer wait
    if (!m_settle->isActive() || m_wanted)
        m_settle->start(m_wanted ? SETTLE_MS : STOP_SETTLE_MS);
}

void SkinTempPlugin::settled()
{
    std::stable_sort(m_pending.begin(), m_pending.end(),
                     [](const QPair<qint64, double> &a, const QPair<qint64, double> &b) { return a.first < b.first; });
    for (const QPair<qint64, double> &r : m_pending) {
        if (m_have && r.first < m_readingMs)
            continue; // out of order, or seen already
        if (!m_have && r.first < m_countedMs)
            continue;
        if (m_have)
            endReading(r.first);
        m_have = true;
        m_readingMs = r.first;
        m_readingC = r.second;
        m_countedMs = r.first;
    }
    m_pending.clear();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_wanted && m_stopMs > 0) {
        // the sampling ended: the reading in force up to the end, the minute it was in closed
        if (m_have)
            endReading(m_stopMs);
        closeMinute();
        m_stopMs = 0;
    } else if (m_have && now >= m_readingMs + HOLD_MS) {
        endReading(now); // its hold is over: the minutes up to its end are complete
    }
    writeRows();
}

void SkinTempPlugin::endReading(qint64 toMs)
{
    advance(std::min(toMs, m_readingMs + HOLD_MS));
    if (toMs >= m_readingMs + HOLD_MS || !m_wanted)
        m_have = false;
}

void SkinTempPlugin::advance(qint64 toMs)
{
    while (m_countedMs < toMs) {
        const qint64 minute = m_countedMs / MINUTE_MS;
        if (minute != m_minute) {
            closeMinute();
            m_minute = minute;
        }
        const qint64 end = std::min(toMs, (minute + 1) * MINUTE_MS);
        m_sum += m_readingC * (double)(end - m_countedMs);
        m_cover += end - m_countedMs;
        m_countedMs = end;
        if (end == (minute + 1) * MINUTE_MS)
            closeMinute();
    }
}

void SkinTempPlugin::closeMinute()
{
    if (m_minute >= 0 && m_cover >= MIN_COVER_MS && m_minute > m_lastRowMinute) {
        m_rows.append(qMakePair(m_minute * MINUTE_MS, std::round(m_sum / (double)m_cover * 100.0) / 100.0));
        m_lastRowMinute = m_minute;
    }
    m_minute = -1;
    m_sum = 0;
    m_cover = 0;
}

void SkinTempPlugin::writeRows()
{
    if (m_rows.isEmpty())
        return;
    if (m_store && m_store->addSkinTemp(m_rows))
        m_written += m_rows.size();
    m_rows.clear();
}
