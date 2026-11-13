/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QTime>

#include "../common.h"
#include "../daemon/sensorPlugins/heartrateDb.h"
#include "healthData.h"

namespace Health {

qint64 stepIntervalMs()
{
    QSettings settings("asteroid", "sensorlogd");
    return qMax(60000, settings.value("stepCounter/interval", 600000).toInt());
}

void loadSteps(MinuteSeries &series, const QDate &day)
{
    series.addStepRecords(readDayRecords(fileNameForDate(day, "stepCounter")), stepIntervalMs());
}

void loadHeartRate(MinuteSeries &series, qint64 fromMs, qint64 toMs)
{
    for (const HeartrateDb::MinuteMean &m : HeartrateDb::minuteMeans(heartrateDbPath(), fromMs, toMs))
        series.addHrMinute({m.minute, m.bpm, m.count});
}

MinuteSeries loadDay(const QDate &day)
{
    MinuteSeries series;
    loadSteps(series, day);
    loadHeartRate(series, QDateTime(day, QTime(0, 0)).toMSecsSinceEpoch(),
                  QDateTime(day.addDays(1), QTime(0, 0)).toMSecsSinceEpoch());
    return series;
}

QList<QPointF> loadPressure(const QDate &day)
{
    return readDayRecords(fileNameForDate(day, "barometer-raw"));
}

int estimatedRestingHr(qint64 nowMs, int days)
{
    QVector<double> means;
    for (const HeartrateDb::MinuteMean &m : HeartrateDb::minuteMeans(heartrateDbPath(), nowMs - (qint64)days * 86400000LL, nowMs))
        means.append(m.bpm);
    return estimateRestingHr(means);
}

QString sleepFilePath(const QDate &wakeDate)
{
    return sensorDirPath("sleep") + wakeDate.toString("yyyy-MM-dd") + ".json";
}

SleepNight estimateNight(const QDate &wakeDate, const HealthProfile &profile, int restingHr)
{
    MinuteSeries series;
    loadSteps(series, wakeDate.addDays(-1));
    loadSteps(series, wakeDate);
    loadHeartRate(series, QDateTime(wakeDate.addDays(-1), QTime(18, 0)).toMSecsSinceEpoch(),
                  QDateTime(wakeDate, QTime(18, 0)).toMSecsSinceEpoch());
    return estimateSleep(series, wakeDate, profile, restingHr);
}

SleepNight loadSleep(const QDate &wakeDate, const HealthProfile &profile, int restingHr, bool *stored)
{
    QFile file(sleepFilePath(wakeDate));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            if (stored)
                *stored = true;
            return SleepNight::fromJson(doc.object());
        }
    }
    if (stored)
        *stored = false;
    return estimateNight(wakeDate, profile, restingHr);
}

bool storeSleep(const SleepNight &night)
{
    if (!night.valid)
        return false;
    setupFilePath("sleep");
    QSaveFile file(sleepFilePath(night.wakeDate));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(night.toJson()).toJson(QJsonDocument::Compact));
    return file.commit();
}

} // namespace Health
