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
#include <QStandardPaths>
#include <QFile>
#include <QDebug>
#include <QDir>
#include <QTextStream>
#include "common.h"

QString fileNameForDate(QDate date, QString prefix) {
    QSettings settings("asteroid", "sensorlogd"); // this should be moved out of here at some point TODO
    return settings.value("loggerRootPath", QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.asteroid-sensorlogd/").toString() + prefix + "/" + date.toString("yyyy-MM-dd.log");
}

QString sensorDirPath(QString prefix) {
    QSettings settings("asteroid", "sensorlogd"); // this should be moved out of here at some point TODO
    return settings.value("loggerRootPath", QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.asteroid-sensorlogd/").toString() + prefix + "/";
}

QString heartrateDbPath() {
    QSettings settings("asteroid", "sensorlogd");
    return settings.value("loggerRootPath", QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.asteroid-sensorlogd/").toString() + "heartrate.db";
}

void fileAddRecord(QString sensorPrefix, QString logdata, QDateTime recordTime) { //adds a record to today's log file for the given sensor
    QFile file(fileNameForDate(recordTime.date(), sensorPrefix));
    if (!file.open(QIODevice::ReadWrite | QIODevice::Text)) {
        qWarning() << "cannot write" << file.fileName() << file.errorString();
        return;
    }
    file.seek(file.size());
    QTextStream out(&file);
    out << QString::number(recordTime.toSecsSinceEpoch()) + ":" + logdata + "\n";
    file.close();
}

void setupFilePath(QString sensorPrefix) {
    QSettings settings("asteroid","sensorlogd"); //this should be moved out of here at some point TODO
    QDir::root().mkpath(settings.value("loggerRootPath",QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.asteroid-sensorlogd/").toString() + sensorPrefix);
}

QList<QPointF> readDayRecords(const QString &path) {
    QList<QPointF> records;
    QFile file(path);
    if (!file.exists()) // nothing was logged that day: not an error
        return records;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "cannot read" << path << file.errorString();
        return records;
    }
    QTextStream inStream(&file);
    while (!inStream.atEnd()) {
        const QString line = inStream.readLine();
        const int sep = line.indexOf(':');
        if (sep <= 0) // empty line, or torn by a power loss mid-write
            continue;
        bool timeOk, valueOk;
        const qint64 time = line.left(sep).toLongLong(&timeOk);
        const double value = line.mid(sep + 1).toDouble(&valueOk); // integers and decimals (barometer, weight)
        if (timeOk && valueOk)
            records.append(QPointF(time, value));
    }
    return records;
}
