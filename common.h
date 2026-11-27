 /*
 * Copyright (C) 2023 Arseniy Movshev <dodoradio@outlook.com>
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SENSORLOGD_COMMON_H
#define SENSORLOGD_COMMON_H

#include <QDate>
#include <QObject>
#include <QString>
#include <QList>
#include <QPointF>

QString fileNameForDate(QDate date, QString prefix);
QString sensorDirPath(QString prefix);
QString heartrateDbPath(); // every heart rate reading (daemon/sensorPlugins/heartrateDb.h), next to the day files
QString loggerRootPath(); // the folder of the day files and the databases, with the closing slash
QString healthDbPath(); // workouts, sleep, one row a day (store/healthDb.h), next to heartrate.db
// connects the daemon's D-Bus signal HealthChanged(x seq), sent after every write to health.db, to the receiver's
// slot (SLOT(name(qlonglong))); false if the session bus is not there
bool connectHealthChanged(QObject *receiver, const char *slot);
void fileAddRecord(QString sensorPrefix, QString logdata, QDateTime recordTime = QDateTime::currentDateTime()); //adds a record to today's log file for the given sensor
QList<QPointF> readDayRecords(const QString &path); // time:value records of a day file; none (quietly) if nothing was logged that day
void setupFilePath(QString sensorPrefix); //sets up the paths for sensors to write into

#endif //SENSORLOGD_COMMON_H
