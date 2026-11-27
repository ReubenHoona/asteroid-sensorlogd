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

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QTime>

#include "../common.h"
#include "../daemon/sensorPlugins/heartrateDb.h"
#include "../store/healthDb.h"
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

void loadHeartRate(MinuteSeries &series, qint64 fromMs, qint64 toMs, const QList<QPair<qint64, qint64>> &skip)
{
    for (const HeartrateDb::MinuteMean &m : HeartrateDb::minuteMeans(heartrateDbPath(), fromMs, toMs)) {
        const qint64 ms = MinuteSeries::msOf(m.minute);
        bool off = false;
        for (const QPair<qint64, qint64> &r : skip)
            off = off || (ms + 60000 > r.first && ms < r.second);
        if (!off)
            series.addHrMinute({m.minute, m.bpm, m.count});
    }
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

QList<QPointF> loadStateRecords(const QString &prefix, qint64 fromMs, qint64 toMs, int lookBackDays)
{
    QList<QPointF> out;
    const double fromS = fromMs / 1000.0, toS = toMs / 1000.0;
    const QDate first = QDateTime::fromMSecsSinceEpoch(fromMs).date();
    const QDate last = QDateTime::fromMSecsSinceEpoch(qMax(fromMs, toMs - 1)).date();
    // the state at fromMs: the latest record before it, from fromMs's day back
    for (int back = 0; back <= qMax(0, lookBackDays); back++) {
        bool found = false;
        QPointF before;
        for (const QPointF &p : readDayRecords(fileNameForDate(first.addDays(-back), prefix))) {
            if (p.x() < fromS && (!found || p.x() >= before.x())) {
                before = p;
                found = true;
            }
        }
        if (found) {
            out.append(before);
            break;
        }
    }
    for (QDate d = first; d <= last; d = d.addDays(1))
        for (const QPointF &p : readDayRecords(fileNameForDate(d, prefix)))
            if (p.x() >= fromS && p.x() < toS)
                out.append(p);
    std::stable_sort(out.begin(), out.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    return out;
}

QList<QPair<qint64, qint64>> offWristRanges(qint64 fromMs, qint64 toMs)
{
    QList<QPair<qint64, qint64>> out;
    qint64 offSince = -1;
    for (const QPointF &p : loadStateRecords(QStringLiteral("offBody"), fromMs, toMs)) {
        const qint64 t = qMax(fromMs, (qint64)std::llround(p.x()) * 1000);
        if (std::lround(p.y()) == 1) {
            if (offSince < 0)
                offSince = t;
        } else if (offSince >= 0) {
            if (t > offSince)
                out.append({offSince, t});
            offSince = -1;
        }
    }
    if (offSince >= 0 && toMs > offSince)
        out.append({offSince, toMs});
    return out;
}

int estimatedRestingHr(qint64 nowMs, int days)
{
    const QDate today = QDateTime::fromMSecsSinceEpoch(nowMs).date();
    int sum = 0, n = 0;
    for (const DailyRecord &r : HealthDb::readDailyRange(healthDbPath(), today.addDays(1 - qMax(1, days)), today)) {
        if (r.restingHr > 0) {
            sum += r.restingHr;
            n++;
        }
    }
    return n ? (int)std::lround((double)sum / n) : 0;
}

void dayRange(const QDate &day, qint64 nowMs, qint64 *fromMs, qint64 *toMs)
{
    *fromMs = QDateTime(day, QTime(0, 0)).toMSecsSinceEpoch();
    *toMs = day == QDateTime::fromMSecsSinceEpoch(nowMs).date() ? nowMs
                                                                : QDateTime(day.addDays(1), QTime(0, 0)).toMSecsSinceEpoch();
}

DaySummary summarizeDay(const QDate &day, const HealthProfile &profile, int restingHr, qint64 nowMs)
{
    DaySummary d;
    const MinuteSeries series = loadDay(day);
    qint64 from, to;
    dayRange(day, nowMs, &from, &to);
    d.steps = series.stepsBetween(from, to);
    d.distanceM = distanceM(d.steps, profile);
    d.energyKcal = energyKcal(series, from, to, profile, restingHr);
    d.floors = floorsClimbed(loadPressure(day), series);
    d.zones = zoneMinutes(series, MinuteSeries::minuteOf(from), MinuteSeries::minuteOf(to), profile, restingHr);
    d.hourly = hourlyActivity(series, day);
    return d;
}

QList<ZoneMinutes> zoneMinutesOfDays(const QDate &from, const QDate &to, const HealthProfile &profile, int restingHr,
                                     qint64 nowMs)
{
    QList<ZoneMinutes> out;
    if (!from.isValid() || !to.isValid() || to < from)
        return out;
    // the heart rate of every day of them at once (loadDay() reads a day's, and no off-wrist ranges either)
    MinuteSeries series;
    loadHeartRate(series, QDateTime(from, QTime(0, 0)).toMSecsSinceEpoch(),
                  QDateTime(to.addDays(1), QTime(0, 0)).toMSecsSinceEpoch());
    for (QDate day = from; day <= to; day = day.addDays(1)) {
        qint64 dayFrom, dayTo;
        dayRange(day, nowMs, &dayFrom, &dayTo);
        out << zoneMinutes(series, MinuteSeries::minuteOf(dayFrom), MinuteSeries::minuteOf(dayTo), profile, restingHr);
    }
    return out;
}

bool hasDataOn(const QDate &day)
{
    if (QFile::exists(fileNameForDate(day, "stepCounter")))
        return true;
    return HeartrateDb::anyReading(heartrateDbPath(), QDateTime(day, QTime(0, 0)).toMSecsSinceEpoch(),
                                   QDateTime(day.addDays(1), QTime(0, 0)).toMSecsSinceEpoch());
}

QString sleepFilePath(const QDate &wakeDate)
{
    return sensorDirPath("sleep") + wakeDate.toString("yyyy-MM-dd") + ".json";
}

QList<QPointF> loadRecords(const QString &prefix, qint64 fromMs, qint64 toMs)
{
    QList<QPointF> out;
    const double fromS = fromMs / 1000.0, toS = toMs / 1000.0;
    const QDate last = QDateTime::fromMSecsSinceEpoch(qMax(fromMs, toMs - 1)).date();
    for (QDate d = QDateTime::fromMSecsSinceEpoch(fromMs).date(); d <= last; d = d.addDays(1))
        for (const QPointF &p : readDayRecords(fileNameForDate(d, prefix)))
            if (p.x() >= fromS && p.x() < toS)
                out.append(p);
    std::stable_sort(out.begin(), out.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    return out;
}

SleepContext loadSleepContext(const QDate &wakeDate)
{
    const qint64 from = QDateTime(wakeDate.addDays(-1), QTime(18, 0)).toMSecsSinceEpoch();
    const qint64 to = QDateTime(wakeDate, QTime(SLEEP_WINDOW_END_HOUR, 0)).toMSecsSinceEpoch();
    SleepContext c;
    c.motion = loadStateRecords(QStringLiteral("motion"), from, to, 1);
    c.cadence = loadRecords(QStringLiteral("stepCadence"), from, to);
    c.offBody = loadStateRecords(QStringLiteral("offBody"), from, to);
    c.bedtime = loadStateRecords(QStringLiteral("bedtime"), from, to);
    return c;
}

SleepNight estimateNight(const QDate &wakeDate, const HealthProfile &profile, int restingHr)
{
    MinuteSeries series;
    loadSteps(series, wakeDate.addDays(-1));
    loadSteps(series, wakeDate);
    const qint64 from = QDateTime(wakeDate.addDays(-1), QTime(18, 0)).toMSecsSinceEpoch();
    const qint64 to = QDateTime(wakeDate, QTime(SLEEP_WINDOW_END_HOUR, 0)).toMSecsSinceEpoch();
    // no heart rate off the wrist (M4 WO-03): a reading from then is not the wearer's
    loadHeartRate(series, from, to, offWristRanges(from, to));
    return estimateSleep(series, wakeDate, profile, restingHr, profile.sleepGoalMin, loadSleepContext(wakeDate));
}

bool loadStoredSleep(const QDate &wakeDate, SleepNight *night)
{
    const QString db = healthDbPath();
    if (HealthDb::readNight(db, wakeDate, night))
        return true;
    // the file only until the import took the files in (then health.db alone says what is stored)
    if (HealthDb::readMeta(db, QStringLiteral("import.sleep")).toLongLong() == 1)
        return false;
    QFile file(sleepFilePath(wakeDate));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            if (night)
                *night = SleepNight::fromJson(doc.object());
            return true;
        }
    }
    return false;
}

SleepNight loadSleep(const QDate &wakeDate, const HealthProfile &profile, int restingHr, bool *stored)
{
    SleepNight night;
    const bool found = loadStoredSleep(wakeDate, &night);
    if (stored)
        *stored = found;
    return found ? night : estimateNight(wakeDate, profile, restingHr);
}

} // namespace Health
