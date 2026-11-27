/*
 * Copyright (C) 2023 Arseniy Movshev <dodoradio@outlook.com>
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef LOGGERSETTINGS_H
#define LOGGERSETTINGS_H

#include <QDBusInterface>
#include <QObject>
#include <QSettings>

#include <MDConfItem>

/*
 * The settings of the daemon and of the Health features, for the Health app's settings page. The logger's own
 * settings (heartrateSensor/*, stepCounter/*) need reInitLogger() to take effect. The Health features
 * (reminders to move, heart rate alerts, auto exercise) are read again by the daemon as soon as they are set
 * (the logger's healthSettingsChanged()).
 *
 * One steps goal, as stock has: the dconf key /org/asteroidos/ux/steps-goal (default 10000), which the
 * steps tile, the complications, the Health app and the celebrations all use (health-fitness-10). There is no
 * switch for it: stepGoalEnabled is always true (it stays so that a page that still reads it works).
 */
class LoggerSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool heartrateSensorEnabled READ getHeartrateSensorEnabled WRITE setHeartrateSensorEnabled)
    Q_PROPERTY(int heartrateSensorInterval READ getHeartrateSensorInterval WRITE setHeartrateSensorInterval)

    Q_PROPERTY(bool stepCounterEnabled READ getStepCounterEnabled WRITE setStepCounterEnabled)
    Q_PROPERTY(int stepCounterInterval READ getStepCounterInterval WRITE setStepCounterInterval)

    Q_PROPERTY(bool stepGoalEnabled READ getStepGoalEnabled WRITE setStepGoalEnabled NOTIFY stepGoalChanged)
    Q_PROPERTY(int stepGoalTarget READ getStepGoalTarget WRITE setStepGoalTarget NOTIFY stepGoalChanged)

    // Reminders to move (stock: off by default; 250 steps an hour, 9:00-18:00, every day)
    Q_PROPERTY(bool remindersEnabled READ getRemindersEnabled WRITE setRemindersEnabled NOTIFY healthChanged)
    Q_PROPERTY(int remindersStartHour READ getRemindersStartHour WRITE setRemindersStartHour NOTIFY healthChanged)
    Q_PROPERTY(int remindersEndHour READ getRemindersEndHour WRITE setRemindersEndHour NOTIFY healthChanged)
    // bit 0 Monday ... bit 6 Sunday; 127: every day
    Q_PROPERTY(int remindersDays READ getRemindersDays WRITE setRemindersDays NOTIFY healthChanged)

    // High and low heart rate notifications (stock: on; a threshold of 0 is none set: stock's 120 and 50 then)
    Q_PROPERTY(bool hrAlertHighEnabled READ getHrAlertHighEnabled WRITE setHrAlertHighEnabled NOTIFY healthChanged)
    Q_PROPERTY(int hrAlertHighBpm READ getHrAlertHighBpm WRITE setHrAlertHighBpm NOTIFY healthChanged)
    Q_PROPERTY(bool hrAlertLowEnabled READ getHrAlertLowEnabled WRITE setHrAlertLowEnabled NOTIFY healthChanged)
    Q_PROPERTY(int hrAlertLowBpm READ getHrAlertLowBpm WRITE setHrAlertLowBpm NOTIFY healthChanged)

    // Auto-Start / Auto-Stop of the exercise prompts (stock: "Start workout reminders", "End workout reminders");
    // the start prompt is this daemon's, the end one the Exercise app's (it reads autoExercise/stop)
    Q_PROPERTY(bool autoStartEnabled READ getAutoStartEnabled WRITE setAutoStartEnabled NOTIFY healthChanged)
    Q_PROPERTY(bool autoStopEnabled READ getAutoStopEnabled WRITE setAutoStopEnabled NOTIFY healthChanged)

public:
    explicit LoggerSettings();

public slots:
    Q_INVOKABLE void reInitLogger();
    bool getHeartrateSensorEnabled();
    void setHeartrateSensorEnabled(bool value);
    int getHeartrateSensorInterval();
    void setHeartrateSensorInterval(int value); // This setting won't change anything live - service needs to be restarted. TODO

    bool getStepCounterEnabled();
    void setStepCounterEnabled(bool value);
    int getStepCounterInterval();
    void setStepCounterInterval(int value); // This setting won't change anything live - service needs to be restarted. TODO

    bool getStepGoalEnabled();
    void setStepGoalEnabled(bool value); // no effect: the goal is always on
    int getStepGoalTarget();
    void setStepGoalTarget(int value);

    bool getRemindersEnabled();
    void setRemindersEnabled(bool value);
    int getRemindersStartHour();
    void setRemindersStartHour(int value);
    int getRemindersEndHour();
    void setRemindersEndHour(int value);
    int getRemindersDays();
    void setRemindersDays(int value);

    bool getHrAlertHighEnabled();
    void setHrAlertHighEnabled(bool value);
    int getHrAlertHighBpm();
    void setHrAlertHighBpm(int value);
    bool getHrAlertLowEnabled();
    void setHrAlertLowEnabled(bool value);
    int getHrAlertLowBpm();
    void setHrAlertLowBpm(int value);

    bool getAutoStartEnabled();
    void setAutoStartEnabled(bool value);
    bool getAutoStopEnabled();
    void setAutoStopEnabled(bool value);

    // the Exercise app has a workout going (no "Start tracking?" offer during it)
    Q_INVOKABLE void setWorkoutActive(bool active);

signals:
    void stepGoalChanged();
    void healthChanged();

private:
    // a Health setting written: sync the file and tell the daemon
    void healthSet(const char *key, const QVariant &value);

    QSettings* m_settings;
    QDBusInterface* m_iface;
    MDConfItem* m_goal;
};
#endif // LOGGERSETTINGS_H
