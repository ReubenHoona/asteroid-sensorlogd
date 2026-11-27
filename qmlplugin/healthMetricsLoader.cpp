/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <cmath>

#include <QDateTime>
#include <QTime>

#include <MDConfItem>

#include "healthMetricsLoader.h"
#include "../common.h"
#include "../metrics/healthData.h"
#include "../metrics/healthMetrics.h"
#include "../store/healthDb.h"

using namespace Health;

namespace {

const int DEFAULT_STEPS_GOAL = 10000;

// the profile and the resting heart rate in force, and the time they were taken at
struct Context {
    HealthProfile profile;
    int resting;
    qint64 nowMs;
    Context()
    {
        nowMs = QDateTime::currentMSecsSinceEpoch();
        profile = HealthProfile::load();
        resting = effectiveRestingHr(profile, profile.restingHr > 0 ? 0 : estimatedRestingHr(nowMs));
    }
};

} // namespace

HealthMetricsLoader::HealthMetricsLoader() : QObject()
{
    m_fileWatcher = new QFileSystemWatcher(this);
    m_fileWatcher->addPath(sensorDirPath("stepCounter"));
    connect(m_fileWatcher, &QFileSystemWatcher::directoryChanged, this, &HealthMetricsLoader::dataChanged);
    connect(m_fileWatcher, &QFileSystemWatcher::fileChanged, this, &HealthMetricsLoader::dataChanged);
    connectHealthChanged(this, SLOT(onHealthChanged(qlonglong)));
}

int HealthMetricsLoader::stepGoal() const
{
    return qMax(1, MDConfItem("/org/asteroidos/ux/steps-goal").value(DEFAULT_STEPS_GOAL).toInt());
}

int HealthMetricsLoader::azmWeeklyGoal() const { return AZM_WEEKLY_GOAL; }
int HealthMetricsLoader::hourlyStepGoal() const { return HOURLY_STEPS; }

QVariantMap HealthMetricsLoader::summary(QDate date)
{
    const Context c;
    const DaySummary d = summarizeDay(date, c.profile, c.resting, c.nowMs);
    const ZoneMinutes &z = d.zones;
    const HourlyActivity &h = d.hourly;
    QVariantList hourSteps;
    for (int s : h.hourSteps)
        hourSteps << s;

    QVariantMap m;
    m["steps"] = d.steps;
    m["distanceM"] = d.distanceM;
    m["energyKcal"] = d.energyKcal;
    m["floors"] = d.floors;
    m["zoneLight"] = z.light;
    m["zoneModerate"] = z.moderate;
    m["zoneVigorous"] = z.vigorous;
    m["zonePeak"] = z.peak;
    m["azm"] = z.azm;
    m["hoursMet"] = h.hoursMet;
    m["hoursTotal"] = h.hoursTotal;
    m["hourSteps"] = hourSteps;
    m["restingHr"] = c.resting;
    m["maxHr"] = c.profile.maxHr();
    m["strideM"] = strideM(c.profile);
    return m;
}

double HealthMetricsLoader::distanceM(QDate date) { return summary(date).value("distanceM").toDouble(); }
int HealthMetricsLoader::energyKcal(QDate date) { return (int)std::lround(summary(date).value("energyKcal").toDouble()); }
int HealthMetricsLoader::floors(QDate date) { return summary(date).value("floors").toInt(); }
int HealthMetricsLoader::azm(QDate date) { return summary(date).value("azm").toInt(); }

int HealthMetricsLoader::azmWeek(QDate date)
{
    int total = 0;
    for (QDate d = date.addDays(1 - date.dayOfWeek()); d <= date; d = d.addDays(1))
        total += azm(d);
    return total;
}

QVariantMap HealthMetricsLoader::hourly(QDate date)
{
    const HourlyActivity h = hourlyActivity(loadDay(date), date);
    QVariantList hourSteps;
    for (int s : h.hourSteps)
        hourSteps << s;
    return QVariantMap{{"hoursMet", h.hoursMet}, {"hoursTotal", h.hoursTotal}, {"hourSteps", hourSteps},
                       {"stepsPerHour", HOURLY_STEPS}};
}

QVariantList HealthMetricsLoader::zones()
{
    static const char *const names[] = {"Light", "Moderate", "Vigorous", "Peak"};
    static const Zone order[] = {Zone::Light, Zone::Moderate, Zone::Vigorous, Zone::Peak};
    const Context c;
    const int max = c.profile.maxHr();
    QVariantList list;
    for (int i = 0; i < 4; i++) {
        const int start = zoneStartBpm(order[i], max, c.resting);
        const int end = i < 3 ? zoneStartBpm(order[i + 1], max, c.resting) - 1 : max;
        list << QVariantMap{{"name", QString::fromLatin1(names[i])}, {"startBpm", start}, {"endBpm", end}};
    }
    return list;
}

int HealthMetricsLoader::zoneForBpm(int bpm)
{
    const Context c;
    return (int)Health::zoneForBpm(bpm, c.profile.maxHr(), c.resting);
}

int HealthMetricsLoader::restingHr() { return Context().resting; }
int HealthMetricsLoader::maxHr() { return HealthProfile::load().maxHr(); }

int HealthMetricsLoader::defaultHighBpm()
{
    const Context c;
    return Health::defaultHighBpm(c.profile, c.resting);
}

int HealthMetricsLoader::defaultLowBpm()
{
    const Context c;
    return Health::defaultLowBpm(c.profile, c.resting);
}

QVariantMap HealthMetricsLoader::daily(QDate date)
{
    DailyRecord r;
    return HealthDb::readDaily(healthDbPath(), date, &r) ? r.toMap() : QVariantMap();
}

double HealthMetricsLoader::minuteKcal(int steps, double bpm)
{
    const Context c;
    Minute m;
    m.steps = qMax(0, steps);
    m.hr = bpm > 0 ? bpm : 0;
    m.hrCount = bpm > 0 ? 1 : 0;
    return activeKcalForMinute(m, c.profile, c.resting);
}

double HealthMetricsLoader::restingKcal(double durationMs)
{
    return durationMs > 0 ? bmrKcalPerDay(HealthProfile::load()) * durationMs / 86400000.0 : 0;
}
