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
#include "../metrics/goals.h"
#include "../metrics/hrAlerts.h"
#include "../metrics/skinTemp.h"
#include "../store/healthDb.h"

using namespace Health;

namespace {

const int DEFAULT_STEPS_GOAL = 10000;

// the profile and the resting heart rate in force, and the time they were taken at
struct Context {
    HealthProfile profile;
    int resting;
    qint64 nowMs;
    explicit Context(const HealthProfile &p)
    {
        nowMs = QDateTime::currentMSecsSinceEpoch();
        profile = p;
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

const HealthProfile &HealthMetricsLoader::profile()
{
    if (m_profileKeys.isEmpty()) {
        for (const QString &key : HealthProfile::keys()) {
            MDConfItem *item = new MDConfItem(key, this);
            connect(item, &MDConfItem::valueChanged, this, &HealthMetricsLoader::onProfileKeyChanged);
            m_profileKeys << item;
        }
    }
    const QDate today = QDate::currentDate();
    if (m_profileDay != today) {
        QVariantList values;
        for (MDConfItem *item : m_profileKeys)
            values << item->value();
        m_profile = HealthProfile::fromKeyValues(values, today);
        m_profileDay = today;
    }
    return m_profile;
}

void HealthMetricsLoader::onProfileKeyChanged()
{
    // (the energy formula keeps the copy it took for its minute, as before)
    m_profileDay = QDate();
    emit profileChanged();
    emit dataChanged();
}

int HealthMetricsLoader::stepGoal() const
{
    return qMax(1, MDConfItem("/org/asteroidos/ux/steps-goal").value(DEFAULT_STEPS_GOAL).toInt());
}

int HealthMetricsLoader::sleepGoal()
{
    return profile().sleepGoalMin;
}

QVariantMap HealthMetricsLoader::mainGoal()
{
    if (m_goalKeys.isEmpty()) {
        for (const QString &key : mainGoalKeys()) {
            MDConfItem *item = new MDConfItem(key, this);
            // the steps goal's change is told by LoggerSettings.stepGoalChanged already (the readers bind to it): a
            // second dataChanged for it would only make them read every day again
            if (key != QLatin1String(goalKey(GoalType::Steps)))
                connect(item, &MDConfItem::valueChanged, this, &HealthMetricsLoader::dataChanged);
            m_goalKeys << item;
        }
    }
    QVariantList values;
    for (MDConfItem *item : m_goalKeys)
        values << item->value();
    const MainGoal goal = mainGoalFrom(values);
    return QVariantMap{{"type", goal.metric()}, {"target", goal.target}};
}

// A day's cardio load: today's from its minutes up to now, a day before it from its closed row in health.db (its
// minutes if it has none yet: a day not closed). The target from the closed rows of the 28 days before it.
QVariantMap HealthMetricsLoader::cardioLoad(QDate date)
{
    const QDate today = QDate::currentDate();
    QMap<QDate, double> loads;
    for (const DailyRecord &r : HealthDb::readDailyRange(healthDbPath(), date.addDays(-CARDIO_HISTORY_DAYS), date))
        if (r.totals.cardioLoad >= 0)
            loads.insert(r.date, r.totals.cardioLoad);
    auto loadOf = [&](const QDate &d) {
        if (d > today)
            return -1.0;
        if (d != today && loads.contains(d))
            return loads.value(d);
        if (d != today && d < today.addDays(-1))
            return -1.0; // closed with no load: a day before the cardio load
        return summary(d).value("cardioLoad").toDouble();
    };
    QList<double> history;
    for (QDate d = date.addDays(-CARDIO_HISTORY_DAYS); d < date; d = d.addDays(1))
        if (loads.contains(d))
            history << loads.value(d);
    const CardioTarget target = cardioTarget(history);
    const double load = loadOf(date);
    QVariantList week;
    for (int i = 6; i >= 0; i--) {
        const QDate d = date.addDays(-i);
        const double l = i == 0 ? load : loadOf(d);
        week << QVariantMap{{"date", QDateTime(d, QTime(0, 0))}, {"load", l >= 0 ? (int)std::lround(l) : -1}};
    }
    const int rounded = load >= 0 ? (int)std::lround(load) : 0;
    QString state;
    if (target.known)
        state = rounded < target.low ? QStringLiteral("below") : rounded > target.high ? QStringLiteral("above")
                                                                                      : QStringLiteral("in");
    return QVariantMap{{"load", rounded}, {"hasTarget", target.known}, {"low", target.low}, {"high", target.high},
                       {"daysLeft", target.daysLeft}, {"state", state}, {"week", week}};
}

int HealthMetricsLoader::azmWeeklyGoal() const { return AZM_WEEKLY_GOAL; }
int HealthMetricsLoader::hourlyStepGoal() const { return HOURLY_STEPS; }

QVariantMap HealthMetricsLoader::summary(QDate date)
{
    const Context c(profile());
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
    m["cardioLoad"] = d.cardioLoad;
    return m;
}

double HealthMetricsLoader::distanceM(QDate date) { return summary(date).value("distanceM").toDouble(); }
int HealthMetricsLoader::energyKcal(QDate date) { return (int)std::lround(summary(date).value("energyKcal").toDouble()); }
int HealthMetricsLoader::floors(QDate date) { return summary(date).value("floors").toInt(); }
int HealthMetricsLoader::azm(QDate date) { return summary(date).value("azm").toInt(); }

int HealthMetricsLoader::azmWeek(QDate date)
{
    int total = 0;
    for (const QVariant &d : zoneDays(date.addDays(1 - date.dayOfWeek()), date))
        total += d.toMap().value("azm").toInt();
    return total;
}

QVariantList HealthMetricsLoader::zoneDays(QDate from, QDate to)
{
    QVariantList out;
    if (!from.isValid() || !to.isValid() || to < from || from.daysTo(to) > 30)
        return out;
    const Context c(profile());
    const QList<ZoneMinutes> days = zoneMinutesOfDays(from, to, c.profile, c.resting, c.nowMs);
    for (int i = 0; i < days.size(); i++) {
        const ZoneMinutes &z = days[i];
        // the day's local midnight: a QDate reaches QML as a Date at midnight UTC, the day before west of it
        out << QVariantMap{{"date", QDateTime(from.addDays(i), QTime(0, 0))}, {"zoneLight", z.light},
                           {"zoneModerate", z.moderate}, {"zoneVigorous", z.vigorous}, {"zonePeak", z.peak},
                           {"azm", z.azm}};
    }
    return out;
}

QVariantMap HealthMetricsLoader::hourly(QDate date)
{
    const MinuteSeries series = loadDay(date);
    const HourlyActivity h = hourlyActivity(series, date);
    QVariantList hourSteps;
    for (int s : h.hourSteps)
        hourSteps << s;
    // the day's sleep (the night that ended on it, its naps, a night that began on its evening) is not inactive time
    QList<QPair<qint64, qint64>> asleep;
    for (const QDate &wake : {date, date.addDays(1)}) {
        SleepNight n;
        HealthDb::readNight(healthDbPath(), wake, &n);
        if (n.valid)
            asleep << qMakePair(n.main.startMs, n.main.endMs);
        for (const SleepSession &nap : n.naps)
            asleep << qMakePair(nap.startMs, nap.endMs);
    }
    qint64 from, to;
    dayRange(date, QDateTime::currentMSecsSinceEpoch(), &from, &to);
    const InactiveSpan inactive = longestInactive(series, from, to, asleep);
    return QVariantMap{{"hoursMet", h.hoursMet}, {"hoursTotal", h.hoursTotal}, {"hourSteps", hourSteps},
                       {"stepsPerHour", HOURLY_STEPS}, {"inactiveStartMs", (double)inactive.startMs},
                       {"inactiveEndMs", (double)inactive.endMs}, {"inactiveMin", inactive.minutes}};
}

QVariantList HealthMetricsLoader::zones()
{
    static const char *const names[] = {"Light", "Moderate", "Vigorous", "Peak"};
    static const Zone order[] = {Zone::Light, Zone::Moderate, Zone::Vigorous, Zone::Peak};
    const Context c(profile());
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
    const Context c(profile());
    return (int)Health::zoneForBpm(bpm, c.profile.maxHr(), c.resting);
}

int HealthMetricsLoader::restingHr() { return Context(profile()).resting; }
int HealthMetricsLoader::maxHr() { return profile().maxHr(); }

int HealthMetricsLoader::restingHrOf(QDate date)
{
    DailyRecord r;
    return HealthDb::readDaily(healthDbPath(), date, &r) && r.restingHr > 0 ? r.restingHr : -1;
}

int HealthMetricsLoader::defaultHighBpm()
{
    return DEFAULT_HIGH_BPM;
}

int HealthMetricsLoader::defaultLowBpm()
{
    return DEFAULT_LOW_BPM;
}

QVariantMap HealthMetricsLoader::daily(QDate date)
{
    DailyRecord r;
    return HealthDb::readDaily(healthDbPath(), date, &r) ? r.toMap() : QVariantMap();
}

QVariantMap HealthMetricsLoader::skinTemp(QDate date)
{
    const QString db = healthDbPath();
    const QList<SkinTempNight> nights = HealthDb::readSkinTempNights(db, date.addDays(-SKIN_BASELINE_DAYS), date);
    const qint64 on = HealthDb::readMeta(db, QStringLiteral("skin_temp.on_ms")).toLongLong();
    const qint64 off = HealthDb::readMeta(db, QStringLiteral("skin_temp.off_ms")).toLongLong();
    return skinTempView(date, nights, on, off, QDateTime::currentMSecsSinceEpoch()).toMap();
}

QVariantList HealthMetricsLoader::skinTempWeek(QDate date)
{
    const QList<SkinTempNight> nights = HealthDb::readSkinTempNights(healthDbPath(), date.addDays(-6), date);
    QVariantList out;
    for (int i = 6; i >= 0; i--) {
        const QDate d = date.addDays(-i);
        QVariantMap m{{"date", d}, {"hasVariation", false}, {"devC", 0.0}, {"loC", 0.0}, {"hiC", 0.0},
                      {"state", QString()}};
        for (const SkinTempNight &n : nights) {
            if (n.wakeDate != d || !n.hasVariation)
                continue;
            m["hasVariation"] = true;
            m["devC"] = n.devC;
            m["loC"] = n.loC;
            m["hiC"] = n.hiC;
            m["state"] = skinTempStateName(n.devC > n.hiC ? SkinAboveRange : n.devC < n.loC ? SkinBelowRange : SkinInRange);
        }
        out << m;
    }
    return out;
}

void HealthMetricsLoader::takeEnergyContext()
{
    static const qint64 KEEP_MS = 60000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_energyAt > 0 && now >= m_energyAt && now - m_energyAt < KEEP_MS)
        return;
    const Context c(profile());
    m_energyProfile = c.profile;
    m_energyResting = c.resting;
    m_energyAt = now;
}

double HealthMetricsLoader::minuteKcal(int steps, double bpm)
{
    takeEnergyContext();
    Minute m;
    m.steps = qMax(0, steps);
    m.hr = bpm > 0 ? bpm : 0;
    m.hrCount = bpm > 0 ? 1 : 0;
    return activeKcalForMinute(m, m_energyProfile, m_energyResting);
}

double HealthMetricsLoader::restingKcal(double durationMs)
{
    if (!(durationMs > 0))
        return 0;
    takeEnergyContext();
    return bmrKcalPerDay(m_energyProfile) * durationMs / 86400000.0;
}
