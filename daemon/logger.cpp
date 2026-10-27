/*
 * Copyright (C) 2023 Arseniy Movshev <dodoradio@outlook.com>
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */


#include <QSettings>
#include <QDBusInterface>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QDebug>
#include <QStandardPaths>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

#include <backgroundactivity.h>

#include "logger.h"
#include "../common.h"

#include "sensorPlugins/stepCounter.h"
#include "sensorPlugins/heartrateSensor.h"
#include "sensorPlugins/barometerSensor.h"

static const QString sensorfwService = QStringLiteral("com.nokia.SensorService");

// the eos sensor hub plugin (libusfhub-qt6 in sensorfwd): hands over the
// samples the hub batched while the display was off
static const QString usfHubPath = QStringLiteral("/org/eos/usfhub");
static const QString usfHubInterface = QStringLiteral("org.eos.UsfHub");

static const int WAKE_FLUSH_SETTLE_MS = 1500; // FLUSH -> the batch is with sensorfwd's clients
static const int WAKE_LIMIT_MS = 15000;       // longest a wake keeps the system up
static const int WAKE_POLL_MS = 250;
static const int WAKE_HR_SECOND_S = 60;       // interval heart rate: read the batch this much later

Logger::Logger(QObject *parent)  :
    QObject(parent){
    if(!QDBusConnection::sessionBus().registerService("org.asteroid.sensorlogd.logger")) qDebug() << "failed to register service";
    if(!QDBusConnection::sessionBus().registerObject("/org/asteroid/sensorlogd/logger", this, QDBusConnection::ExportAllContents)) qDebug() << "failed to register object";

    settings = new QSettings(this);

    m_wake = new BackgroundActivity(this);
    connect(m_wake, &BackgroundActivity::running, this, &Logger::wakeRunning);
    m_wakeSettle = new QTimer(this);
    m_wakeSettle->setSingleShot(true);
    connect(m_wakeSettle, &QTimer::timeout, this, &Logger::wakeWork);
    m_wakePoll = new QTimer(this);
    m_wakePoll->setSingleShot(true);
    connect(m_wakePoll, &QTimer::timeout, this, &Logger::wakeCheckDone);
    m_wakeLimit = new QTimer(this);
    m_wakeLimit->setSingleShot(true);
    connect(m_wakeLimit, &QTimer::timeout, this, &Logger::wakeDone);
    // mce's display state; connected by name, so it works whether or not mce is up yet
    if (!QDBusConnection::systemBus().connect("com.nokia.mce", "/com/nokia/mce/signal", "com.nokia.mce.signal",
                                              "display_status_ind", this, SLOT(displayOn(QString))))
        qDebug() << "failed to connect to mce's display_status_ind";

    // QtSensors' sensorfw backend gives up for good on a sensor that is made
    // before sensorfwd's D-Bus service exists ("There is no sensor manager
    // yet"), so the sensors are set up only once the service is there. A later
    // sensorfwd restart is handled by the backend itself.
    m_sensorfwWatcher = new QDBusServiceWatcher(sensorfwService, QDBusConnection::systemBus(),
                                                QDBusServiceWatcher::WatchForRegistration, this);
    connect(m_sensorfwWatcher, &QDBusServiceWatcher::serviceRegistered, this, &Logger::sensorfwRegistered);
    if (QDBusConnection::systemBus().interface()->isServiceRegistered(sensorfwService).value())
        this->setup();
    else
        qDebug() << "waiting for" << sensorfwService;
}

void Logger::sensorfwRegistered() {
    if (m_setupDone)
        return;
    qDebug() << sensorfwService << "is up";
    this->setup();
}

void Logger::setup() {
    m_setupDone = true;
    // resetup(): replace the sensors rather than add a second set next to them
    delete m_heartrateSensor;
    m_heartrateSensor = nullptr;
    delete m_stepCounter;
    m_stepCounter = nullptr;
    delete m_barometerSensor;
    m_barometerSensor = nullptr;
    settings->sync(); // the settings app wrote the file
    daemonFresh = getDaemonFresh();

    heartrateSensorEnabled = this->settings->value("heartrateSensor/enabled",true).toBool();
    stepCounterEnabled = this->settings->value("stepCounter/enabled",true).toBool();
    barometerEnabled = this->settings->value("barometer/enabled",true).toBool();

//intialise HRM
    if (heartrateSensorEnabled) {
        m_heartrateSensor = new HeartrateSensorPlugin(this,settings->value("heartrateSensor/interval",600000).toInt(),
                                                      settings->value("heartrateSensor/continuous",false).toBool());
    }

//initialise step counter
    if (stepCounterEnabled) {
        m_stepCounter = new StepCounterPlugin(this,settings->value("stepCounter/interval",600000).toInt(),daemonFresh);
    }

//initialise barometer
    if (barometerEnabled) {
        m_barometerSensor = new BarometerSensorPlugin(this,settings->value("barometer/interval",600000).toInt());
    }

    // one system wake for every sensor: the shortest interval, at the
    // earliest; iphb may align it with other wakes up to half an interval later
    int wakeMs = 0;
    if (heartrateSensorEnabled && !settings->value("heartrateSensor/continuous",false).toBool())
        wakeMs = settings->value("heartrateSensor/interval",600000).toInt();
    if (heartrateSensorEnabled && settings->value("heartrateSensor/continuous",false).toBool())
        wakeMs = 600000; // continuous: collect the batch every 10 min
    if (stepCounterEnabled) {
        int ms = settings->value("stepCounter/interval",600000).toInt();
        wakeMs = wakeMs ? qMin(wakeMs, ms) : ms;
    }
    if (barometerEnabled) {
        int ms = settings->value("barometer/interval",600000).toInt();
        wakeMs = wakeMs ? qMin(wakeMs, ms) : ms;
    }
    m_wakeInterval = qMax(60, wakeMs / 1000);

    qDebug() << "sensorlogd setup complete";
    this->triggerRecording();
    if (heartrateSensorEnabled && m_heartrateSensor->measuring())
        scheduleWake(WAKE_HR_SECOND_S);
    else if (heartrateSensorEnabled || stepCounterEnabled || barometerEnabled)
        scheduleWake(m_wakeInterval);
    else
        m_wake->stop();
}

void Logger::scheduleWake(int seconds) {
    // also ends a running wake: the keepalive stops once the next wake is armed
    m_wakeSettle->stop();
    m_wakePoll->stop();
    m_wakeLimit->stop();
    // a new range only takes effect on a state change (libkeepalive)
    if (m_wake->isWaiting())
        m_wake->stop();
    m_wake->wait(seconds, seconds + seconds / 2);
}

void Logger::flushHub() {
    QDBusMessage m = QDBusMessage::createMethodCall(sensorfwService, usfHubPath, usfHubInterface,
                                                    QStringLiteral("Flush"));
    QDBusPendingCallWatcher *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m, 3000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *pw) {
        QDBusPendingReply<int> r = *pw;
        if (r.isError() && !m_flushWarned) {
            // an older sensor hub plugin: the hub hands its batch over on its own latency
            qWarning() << "sensor hub flush not available:" << r.error().message();
            m_flushWarned = true;
        }
        pw->deleteLater();
    });
}

void Logger::wakeRunning() {
    // The system woke for us (mce cpu keepalive held): get the hub's batch
    // first, then record what is due
    m_wakeLimit->start(WAKE_LIMIT_MS);
    flushHub();
    m_wakeSettle->start(WAKE_FLUSH_SETTLE_MS);
}

void Logger::wakeWork() {
    if (heartrateSensorEnabled)
        m_heartrateSensor->wakeUpdate();
    if (stepCounterEnabled)
        m_stepCounter->timeUpdate();
    if (barometerEnabled)
        m_barometerSensor->timeUpdate();
    wakeCheckDone();
}

void Logger::wakeCheckDone() {
    // the barometer gives a reading within a second or two: wait for it
    // (bounded by m_wakeLimit); heart rate is read from the batch at a second wake
    if (barometerEnabled && m_barometerSensor->measuring()) {
        m_wakePoll->start(WAKE_POLL_MS);
        return;
    }
    wakeDone();
}

void Logger::wakeDone() {
    if (!m_wake->isRunning())
        return;
    // the limit came first: do not leave the barometer on until the next wake
    if (barometerEnabled && m_barometerSensor->measuring())
        m_barometerSensor->abortRecording();
    if (heartrateSensorEnabled && m_heartrateSensor->measuring())
        scheduleWake(WAKE_HR_SECOND_S);
    else
        scheduleWake(m_wakeInterval);
}

void Logger::resetup() {
    if (!m_setupDone) // still waiting for sensorfwd; setup() reads the new settings then
        return;
    this->setup();
}

void Logger::displayOn(QString displayState) {
    if (displayState == "off")
        return;
    qDebug() << "display on detected";
    uint currTime = QDateTime::currentMSecsSinceEpoch();

    if (heartrateSensorEnabled) {
        m_heartrateSensor->timeUpdate();
    }

    if (stepCounterEnabled) {
        m_stepCounter->timeUpdate();
    }

    if (barometerEnabled) {
        m_barometerSensor->timeUpdate();
    }
}

void Logger::triggerRecording() {
    if (heartrateSensorEnabled) {
        m_heartrateSensor->triggerRecording();
    }

    if (stepCounterEnabled) {
        m_stepCounter->triggerRecording();
    }

    if (barometerEnabled) {
        m_barometerSensor->triggerRecording();
    }
}

bool Logger::getDaemonFresh() {
    QFile file("/proc/sys/kernel/random/boot_id");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "failed to open bootId";
        return 0;
    }
    QTextStream inStream(&file);
    QString currBootId = inStream.readAll().trimmed();
    file.close();
    QSettings settings("asteroid","sensorlogd");
    if(currBootId == settings.value("lastBootId","")) {
        qDebug() << "daemon is not starting for first time this boot";
        return false;
    } else {
        settings.setValue("lastBootId",currBootId);
        qDebug() << "daemon is starting for first time this boot";
        return true;
    }
}

bool dayFileExists(QString sensorPrefix, QDateTime dateTime) {
    return QFile::exists(fileNameForDate(dateTime.date(), sensorPrefix));
}

QStringList fileGetPrevRecord(QString sensorPrefix, QDateTime recordTime) {
    qDebug() << fileNameForDate(recordTime.date(), sensorPrefix);
    QFile file(fileNameForDate(recordTime.date(), sensorPrefix));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "failed to open file";
        return {"0", "0"};
    }
    QTextStream inStream(&file);
    QString line;
    while(!inStream.atEnd())
    {
        QString next = inStream.readLine();
        if (!next.isEmpty())
            line = next;
    }
    file.close();
    QStringList record = line.split(":");
    if (record.size() < 2) // empty file or a torn last line: callers read [1]
        return {"0", "0"};
    return record;
}



