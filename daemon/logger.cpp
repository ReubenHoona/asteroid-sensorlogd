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

#include <climits>

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

    // the health database: open from the start (the workouts' methods work before the sensors are up)
    m_store = new Health::HealthDb(healthDbPath());
    m_store->setChangeHandler([this](qint64 seq) { emit HealthChanged(seq); });

    // mce's display state; connected by name, so it works whether or not mce is up yet
    if (!QDBusConnection::systemBus().connect("com.nokia.mce", "/com/nokia/mce/signal", "com.nokia.mce.signal",
                                              "display_status_ind", this, SLOT(displayOn(QString))))
        qDebug() << "failed to connect to mce's display_status_ind";

    // QtSensors' sensorfw backend gives up for good on a sensor that is made
    // before sensorfwd's D-Bus service exists ("There is no sensor manager
    // yet"), so the sensors are set up only once the service is there. A later
    // sensorfwd restart is handled by the backend itself.
    // the Exercise app's connection: its end (exit, crash, kill) ends the hub's workout mode
    m_clientWatcher = new QDBusServiceWatcher(QString(), QDBusConnection::sessionBus(),
                                              QDBusServiceWatcher::WatchForUnregistration, this);
    connect(m_clientWatcher, &QDBusServiceWatcher::serviceUnregistered, this, &Logger::workoutClientGone);

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
    delete m_store;
}

void Logger::sensorfwRegistered() {
    if (m_setupDone) {
        // a new sensorfwd: its hub connection knows nothing of us yet
        qDebug() << sensorfwService << "is back";
        if (m_activity)
            m_activity->subscribe();
        if (m_offBody)
            m_offBody->query();
        if (m_skinTemp)
            m_skinTemp->resubscribe();
        if (m_hubWorkout)
            sendHubWorkout();
        return;
    }
    qDebug() << sensorfwService << "is up";
    this->setup();
}

void Logger::deletePlugins() {
    delete m_health; // before the plugins it listens to
    m_health = nullptr;
    delete m_heartrateSensor;
    m_heartrateSensor = nullptr;
    delete m_stepCounter;
    m_stepCounter = nullptr;
    delete m_barometerSensor;
    m_barometerSensor = nullptr;
    delete m_activity;
    m_activity = nullptr;
    delete m_offBody;
    m_offBody = nullptr;
    delete m_skinTemp;
    m_skinTemp = nullptr;
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
    const bool activityEnabled = this->settings->value("activity/enabled", true).toBool();
    const bool offBodyEnabled = this->settings->value("offBody/enabled", true).toBool();
    const bool skinTempEnabled = this->settings->value("skinTemp/enabled", true).toBool();

    // heart rate: one record per interval at rest (Wear OS-like, 5-10 min)
    if (heartrateSensorEnabled) {
        m_heartrateSensor = new HeartrateSensorPlugin(this, settings, settings->value("heartrateSensor/interval",600000).toInt());
    }

    if (stepCounterEnabled) {
        m_stepCounter = new StepCounterPlugin(this,settings->value("stepCounter/interval",600000).toInt(),daemonFresh);
    }

    // pressure all the time, one record a minute (barometer/interval was the old reading period)
    if (barometerEnabled) {
        m_barometerSensor = new BarometerSensorPlugin(this, settings->value("barometer/recordInterval", 60000).toInt());
    }

    if (activityEnabled)
        m_activity = new ActivityStreamsPlugin(this);

    // the wrist (M4 WO-03): the hub's off-body sensor
    if (offBodyEnabled)
        m_offBody = new OffBodyPlugin(this);

    // the skin temperature (M4 WO-13): sampled inside a night's sleep, when the Health monitor says
    if (skinTempEnabled)
        m_skinTemp = new SkinTempPlugin(m_store, this);

    m_health = new HealthMonitor(settings, &m_poster, this);
    m_health->setStore(m_store);
    m_health->attach(m_stepCounter, m_heartrateSensor);
    connect(m_health, &HealthMonitor::goalReached, this, &Logger::goalReached);
    connect(m_health, &HealthMonitor::exerciseOffered, this, &Logger::ExerciseOffered);
    if (m_offBody) {
        m_health->setOffBody(m_offBody->state());
        connect(m_offBody, &OffBodyPlugin::changed, m_health, &HealthMonitor::setOffBody);
    }
    if (m_skinTemp) {
        m_health->setSkinTempAvailable(true);
        connect(m_health, &HealthMonitor::skinTempSamplingChanged, m_skinTemp, &SkinTempPlugin::setWanted);
    }
    m_health->start();

    qDebug() << "sensorlogd setup complete";
    updateHubWorkout();
}

void Logger::resetup() {
    if (!m_setupDone) // still waiting for sensorfwd; setup() reads the new settings then
        return;
    this->setup();
}

void Logger::displayOn(QString displayState) {
    if (displayState == "off") {
        // the system may sleep now: the live heart rate readings go to the database first
        if (heartrateSensorEnabled && m_heartrateSensor)
            m_heartrateSensor->displayOff();
        return;
    }
    // the system is awake anyway: the sensor hub hands over its batch now
    // (the eos hub plugin flushes at display on); write what is due
    qDebug() << "display on detected";

    if (m_health)
        m_health->displayOn();

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

void Logger::healthSettingsChanged() {
    settings->sync();
    if (m_health)
        m_health->reloadSettings();
}

void Logger::setWorkoutActive(bool active) {
    if (active)
        noteWorkoutClient();
    m_workoutOn = active;
    if (!active && !m_workoutClient.isEmpty()) {
        m_clientWatcher->removeWatchedService(m_workoutClient);
        m_workoutClient.clear();
    }
    if (m_health)
        m_health->setWorkoutActive(active);
    updateHubWorkout();
}

// the caller of a workout method is the app that runs it (a call from inside this process is no one's)
void Logger::noteWorkoutClient() {
    if (!calledFromDBus())
        return;
    const QString name = message().service();
    if (name.isEmpty() || name == m_workoutClient || name == QDBusConnection::sessionBus().baseService())
        return;
    if (!m_workoutClient.isEmpty())
        m_clientWatcher->removeWatchedService(m_workoutClient);
    m_workoutClient = name;
    m_clientWatcher->addWatchedService(name);
    qDebug() << "workout: run by" << name;
    // it may have left before the watch was there
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    if (bus && !bus->isServiceRegistered(name).value())
        QMetaObject::invokeMethod(this, "workoutClientGone", Qt::QueuedConnection, Q_ARG(QString, name));
    updateHubWorkout();
}

void Logger::workoutClientGone(const QString &name) {
    m_clientWatcher->removeWatchedService(name);
    if (name != m_workoutClient)
        return;
    // the app is gone (it ended, crashed or was killed): the workout stays in health.db for its recovery, but
    // nobody shows zones now; the batch latency again until the app is back and calls
    qDebug() << "workout: its app left the bus";
    m_workoutClient.clear();
    updateHubWorkout();
}

void Logger::updateHubWorkout() {
    const bool want = m_workoutOn && !m_workoutClient.isEmpty() && settings->value("workoutHr/fast", true).toBool();
    if (want == m_hubWorkout)
        return;
    m_hubWorkout = want;
    sendHubWorkout();
}

void Logger::sendHubWorkout() {
    qDebug() << "workout: the sensor hub's workout latency" << (m_hubWorkout ? "on" : "off");
    QDBusMessage m = QDBusMessage::createMethodCall(sensorfwService, QStringLiteral("/org/eos/usfhub"),
                                                    QStringLiteral("org.eos.UsfHub"), QStringLiteral("SetWorkoutMode"));
    m << m_hubWorkout;
    // no reply waited for: sensorfwd not there yet is fine (sensorfwRegistered sends it again)
    QDBusConnection::systemBus().send(m);
}

qlonglong Logger::nextReminderAt() {
    return m_health ? m_health->nextReminderMs() : -1;
}

QVariantMap Logger::ExerciseOffer() {
    return m_health ? m_health->exerciseOffer() : QVariantMap();
}

qlonglong Logger::BeginWorkout(qlonglong startMs, const QString &type, const QString &source, bool gps) {
    const qint64 id = m_store->beginWorkout(startMs, type, source, gps);
    if (id > 0)
        setWorkoutActive(true);
    return id;
}

bool Logger::AddTrackPoints(qlonglong id, const QList<qlonglong> &points) {
    if (points.size() % 6 != 0)
        return false;
    if (m_workoutOn)
        noteWorkoutClient();
    auto clamp = [](qlonglong v, qlonglong lo, qlonglong hi) { return v < lo ? lo : v > hi ? hi : v; };
    QVector<Health::TrackPoint> fixes;
    fixes.reserve(points.size() / 6);
    for (int i = 0; i + 5 < points.size(); i += 6) {
        Health::TrackPoint p;
        p.tMs = points[i];
        p.latE7 = (qint32)clamp(points[i + 1], -2000000000LL, 2000000000LL);
        p.lonE7 = (qint32)clamp(points[i + 2], -2000000000LL, 2000000000LL);
        p.altDm = (int)clamp(points[i + 3], INT_MIN, INT_MAX);
        p.haccDm = (int)clamp(points[i + 4], 0, INT_MAX);
        p.speedCms = (int)clamp(points[i + 5], 0, INT_MAX);
        fixes.append(p);
    }
    return m_store->addTrackPoints(id, fixes);
}

bool Logger::AddLap(qlonglong id, int n, qlonglong startMs, qlonglong endMs, double distanceM, int avgBpm,
                    const QString &kind) {
    Health::LapRecord lap;
    lap.n = n;
    lap.startMs = startMs;
    lap.endMs = endMs;
    lap.distanceM = distanceM;
    lap.avgBpm = avgBpm;
    lap.kind = kind;
    if (m_workoutOn)
        noteWorkoutClient();
    return m_store->addLap(id, lap);
}

bool Logger::SetPaused(qlonglong id, bool paused, qlonglong atMs, bool automatic) {
    const bool ok = m_store->setPaused(id, atMs, paused, automatic);
    if (ok)
        setWorkoutActive(true);
    return ok;
}

bool Logger::EndWorkout(qlonglong id, qlonglong endMs, const QVariantMap &summary) {
    Health::WorkoutRecord s;
    s.activeS = summary.value("activeS").toInt();
    s.distanceM = summary.value("distanceM").toDouble();
    s.kcal = summary.value("kcal").toDouble();
    s.steps = summary.value("steps").toInt();
    s.avgBpm = summary.value("avgBpm").toInt();
    s.maxBpm = summary.value("maxBpm").toInt();
    s.lightS = summary.value("lightS").toInt();
    s.moderateS = summary.value("moderateS").toInt();
    s.vigorousS = summary.value("vigorousS").toInt();
    s.peakS = summary.value("peakS").toInt();
    s.azm = summary.value("azm").toInt();
    s.elevGainM = summary.value("elevGainM").toDouble();
    const bool ok = m_store->endWorkout(id, endMs, s);
    if (ok)
        setWorkoutActive(m_store->openWorkoutCount() > 0);
    return ok;
}

bool Logger::DeleteWorkout(qlonglong id) {
    const bool ok = m_store->deleteWorkout(id);
    if (ok)
        setWorkoutActive(m_store->openWorkoutCount() > 0);
    return ok;
}

bool Logger::SaveWorkoutState(qlonglong id, qlonglong atMs, const QString &state) {
    const bool ok = m_store->saveWorkoutState(id, atMs, state);
    if (ok)
        setWorkoutActive(true);
    return ok;
}

QVariantMap Logger::OpenWorkout(qlonglong beforeMs) {
    Health::WorkoutRecord w;
    qint64 savedMs = 0;
    QString state;
    bool paused = false;
    if (!Health::HealthDb::readOpenWorkout(m_store->path(), beforeMs, &w, &savedMs, &state, &paused))
        return QVariantMap();
    QVariantMap m = w.toMap();
    m.insert("savedMs", (qlonglong)savedMs);
    m.insert("state", state);
    m.insert("paused", paused);
    return m;
}

qlonglong Logger::HealthSeq() {
    return m_store->seq();
}

QVariantMap Logger::HealthCounts() {
    return Health::HealthDb::readCounts(m_store->path());
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
