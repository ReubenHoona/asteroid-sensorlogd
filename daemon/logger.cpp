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

#include "logger.h"
#include "../common.h"

#include "sensorPlugins/stepCounter.h"
#include "sensorPlugins/heartrateSensor.h"
#include "sensorPlugins/barometerSensor.h"

static const QString sensorfwService = QStringLiteral("com.nokia.SensorService");

Logger::Logger(QObject *parent)  :
    QObject(parent){
    if(!QDBusConnection::sessionBus().registerService("org.asteroid.sensorlogd.logger")) qDebug() << "failed to register service";
    if(!QDBusConnection::sessionBus().registerObject("/org/asteroid/sensorlogd/logger", this, QDBusConnection::ExportAllContents)) qDebug() << "failed to register object";

    settings = new QSettings(this);

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

Logger::~Logger()
{
    // the plugins write what they collected; before settings goes
    deletePlugins();
}

void Logger::sensorfwRegistered() {
    if (m_setupDone)
        return;
    qDebug() << sensorfwService << "is up";
    this->setup();
}

void Logger::deletePlugins() {
    delete m_heartrateSensor;
    m_heartrateSensor = nullptr;
    delete m_stepCounter;
    m_stepCounter = nullptr;
    delete m_barometerSensor;
    m_barometerSensor = nullptr;
}

void Logger::setup() {
    m_setupDone = true;
    // resetup(): replace the sensors rather than add a second set next to them
    deletePlugins();
    settings->sync(); // the settings app wrote the file
    daemonFresh = getDaemonFresh();

    heartrateSensorEnabled = this->settings->value("heartrateSensor/enabled",true).toBool();
    stepCounterEnabled = this->settings->value("stepCounter/enabled",true).toBool();
    barometerEnabled = this->settings->value("barometer/enabled",true).toBool();

    // heart rate: one record per interval at rest (Wear OS-like, 5-10 min)
    if (heartrateSensorEnabled) {
        m_heartrateSensor = new HeartrateSensorPlugin(this, settings, settings->value("heartrateSensor/interval",600000).toInt());
    }

    if (stepCounterEnabled) {
        m_stepCounter = new StepCounterPlugin(this,settings->value("stepCounter/interval",600000).toInt(),daemonFresh);
    }

    if (barometerEnabled) {
        m_barometerSensor = new BarometerSensorPlugin(this,settings->value("barometer/interval",600000).toInt());
    }

    qDebug() << "sensorlogd setup complete";
    if (barometerEnabled)
        m_barometerSensor->triggerRecording();
}

void Logger::resetup() {
    if (!m_setupDone) // still waiting for sensorfwd; setup() reads the new settings then
        return;
    this->setup();
}

void Logger::displayOn(QString displayState) {
    if (displayState == "off")
        return;
    // the system is awake anyway: the sensor hub hands over its batch now
    // (the eos hub plugin flushes at display on); write what is due
    qDebug() << "display on detected";

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
    // the last valid record: callers read [1], and a torn or zero-filled
    // last line (power loss mid-write) must not hide the day's real last one
    QStringList record = {"0", "0"};
    while(!inStream.atEnd())
    {
        const QStringList fields = inStream.readLine().split(":");
        bool timeOk = false, valueOk = false;
        if (fields.size() == 2) {
            fields[0].toLongLong(&timeOk);
            fields[1].toInt(&valueOk);
        }
        if (timeOk && valueOk)
            record = fields;
    }
    file.close();
    return record;
}
