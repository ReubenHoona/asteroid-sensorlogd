/*
 * Copyright (C) 2023 Arseniy Movshev <dodoradio@outlook.com>
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <QDebug>

#include "loggerSettings.h"

static const char *const STEPS_GOAL_KEY = "/org/asteroidos/ux/steps-goal";
static const int DEFAULT_STEPS_GOAL = 10000;

LoggerSettings::LoggerSettings()
    : QObject()
{
    m_settings = new QSettings("asteroid", "sensorlogd");
    m_iface = new QDBusInterface("org.asteroid.sensorlogd.logger", "/org/asteroid/sensorlogd/logger", "", QDBusConnection::sessionBus(), this);
    if (!m_iface->isValid()) {
        qDebug() << "interface is not valid";
    } else {
        qDebug() << "interface is valid";
    }
    m_goal = new MDConfItem(STEPS_GOAL_KEY, this);
    connect(m_goal, &MDConfItem::valueChanged, this, &LoggerSettings::stepGoalChanged);
}

void LoggerSettings::reInitLogger()
{
    m_iface->call("resetup");
}

bool LoggerSettings::getHeartrateSensorEnabled()
{
    return this->m_settings->value("heartrateSensor/enabled", true).toBool();
}

void LoggerSettings::setHeartrateSensorEnabled(bool value)
{
    this->m_settings->setValue("heartrateSensor/enabled", value);
}

int LoggerSettings::getHeartrateSensorInterval()
{
    return this->m_settings->value("heartrateSensor/interval", 600000).toInt();
}

void LoggerSettings::setHeartrateSensorInterval(int value)
{
    this->m_settings->setValue("heartrateSensor/interval", value);
}

bool LoggerSettings::getStepCounterEnabled()
{
    return this->m_settings->value("stepCounter/enabled", true).toBool();
}

void LoggerSettings::setStepCounterEnabled(bool value)
{
    this->m_settings->setValue("stepCounter/enabled", value);
}

int LoggerSettings::getStepCounterInterval()
{
    return this->m_settings->value("stepCounter/interval", 600000).toInt();
}

void LoggerSettings::setStepCounterInterval(int value)
{
    this->m_settings->setValue("stepCounter/interval", value);
}

// One goal for everything (stock: the main goal, steps by default): the dconf key the tile, the complications
// and the celebrations read. The goal0 keys of the old settings page are not used any more.
bool LoggerSettings::getStepGoalEnabled()
{
    return true;
}

void LoggerSettings::setStepGoalEnabled(bool)
{
}

int LoggerSettings::getStepGoalTarget()
{
    return qMax(1, m_goal->value(DEFAULT_STEPS_GOAL).toInt());
}

void LoggerSettings::setStepGoalTarget(int value)
{
    if (value < 1)
        return;
    m_goal->set(value);
}

void LoggerSettings::healthSet(const char *key, const QVariant &value)
{
    if (m_settings->value(key) == value && m_settings->contains(key))
        return;
    m_settings->setValue(key, value);
    m_settings->sync();
    m_iface->asyncCall("healthSettingsChanged");
    emit healthChanged();
}

bool LoggerSettings::getRemindersEnabled() { return m_settings->value("reminders/enabled", false).toBool(); }
void LoggerSettings::setRemindersEnabled(bool value) { healthSet("reminders/enabled", value); }
int LoggerSettings::getRemindersStartHour() { return m_settings->value("reminders/startHour", 9).toInt(); }
void LoggerSettings::setRemindersStartHour(int value) { healthSet("reminders/startHour", qBound(0, value, 23)); }
int LoggerSettings::getRemindersEndHour() { return m_settings->value("reminders/endHour", 18).toInt(); }
void LoggerSettings::setRemindersEndHour(int value) { healthSet("reminders/endHour", qBound(1, value, 24)); }
int LoggerSettings::getRemindersDays() { return m_settings->value("reminders/days", 0x7f).toInt(); }
void LoggerSettings::setRemindersDays(int value) { healthSet("reminders/days", value & 0x7f); }

bool LoggerSettings::getHrAlertHighEnabled() { return m_settings->value("hrAlerts/highEnabled", true).toBool(); }
void LoggerSettings::setHrAlertHighEnabled(bool value) { healthSet("hrAlerts/highEnabled", value); }
int LoggerSettings::getHrAlertHighBpm() { return m_settings->value("hrAlerts/highBpm", 0).toInt(); }
void LoggerSettings::setHrAlertHighBpm(int value) { healthSet("hrAlerts/highBpm", qMax(0, value)); }
bool LoggerSettings::getHrAlertLowEnabled() { return m_settings->value("hrAlerts/lowEnabled", true).toBool(); }
void LoggerSettings::setHrAlertLowEnabled(bool value) { healthSet("hrAlerts/lowEnabled", value); }
int LoggerSettings::getHrAlertLowBpm() { return m_settings->value("hrAlerts/lowBpm", 0).toInt(); }
void LoggerSettings::setHrAlertLowBpm(int value) { healthSet("hrAlerts/lowBpm", qMax(0, value)); }

bool LoggerSettings::getAutoStartEnabled() { return m_settings->value("autoExercise/start", true).toBool(); }
void LoggerSettings::setAutoStartEnabled(bool value) { healthSet("autoExercise/start", value); }
bool LoggerSettings::getAutoStopEnabled() { return m_settings->value("autoExercise/stop", true).toBool(); }
void LoggerSettings::setAutoStopEnabled(bool value) { healthSet("autoExercise/stop", value); }

void LoggerSettings::setWorkoutActive(bool active)
{
    m_iface->asyncCall("setWorkoutActive", active);
}
