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
#include <QTime>

#include "healthMetrics.h"

namespace Health {

void MinuteSeries::trimBefore(qint64 minute)
{
    m_data.erase(m_data.begin(), m_data.lowerBound(minute));
}

void MinuteSeries::addSteps(qint64 ms, int steps)
{
    if (steps > 0)
        m_data[minuteOf(ms)].steps += steps;
}

void MinuteSeries::addHr(qint64 ms, int bpm)
{
    if (bpm <= 0)
        return;
    Minute &m = m_data[minuteOf(ms)];
    m.hr = (m.hr * m.hrCount + bpm) / (m.hrCount + 1);
    m.hrCount++;
}

void MinuteSeries::addHrMinute(const HrMinute &h)
{
    if (h.bpm <= 0 || h.count <= 0)
        return;
    Minute &m = m_data[h.minute];
    m.hr = (m.hr * m.hrCount + h.bpm * h.count) / (m.hrCount + h.count);
    m.hrCount += h.count;
}

void MinuteSeries::addStepRecords(const QList<QPointF> &records, qint64 intervalMs, int before)
{
    intervalMs = qMax<qint64>(60000, intervalMs);
    int previous = before;
    qint64 previousMs = -1;
    for (const QPointF &r : records) {
        const int count = (int)r.y();
        const qint64 tMs = (qint64)r.x() * 1000;
        const int delta = count - previous;
        if (delta <= 0) {
            previous = count; // a counter that started again: go on from there
            previousMs = tMs;
            continue;
        }
        qint64 start = tMs - tMs % intervalMs;
        if (previousMs >= 0 && previousMs > start)
            start = previousMs;
        const qint64 first = minuteOf(start), last = minuteOf(tMs);
        const int n = (int)qMax<qint64>(1, last - first + 1);
        const int base = delta / n, extra = delta % n;
        for (int i = 0; i < n; i++) {
            const int steps = base + (i >= n - extra ? 1 : 0);
            if (steps > 0)
                m_data[first + i].steps += steps;
        }
        previous = count;
        previousMs = tMs;
    }
}

int MinuteSeries::stepsBetween(qint64 fromMs, qint64 toMs) const
{
    int steps = 0;
    const qint64 last = minuteOf(toMs);
    for (auto it = m_data.lowerBound(minuteOf(fromMs)); it != m_data.end() && it.key() <= last; ++it)
        steps += it->steps;
    return steps;
}

Zone zoneForBpm(double bpm, int maxHr, int restingHr)
{
    const double reserve = qMax(1, maxHr - restingHr);
    const double fraction = (bpm - restingHr) / reserve;
    if (fraction < 0.40)
        return Zone::Light;
    if (fraction < 0.60)
        return Zone::Moderate;
    if (fraction < 0.85)
        return Zone::Vigorous;
    return Zone::Peak;
}

int azmForZone(Zone zone)
{
    switch (zone) {
    case Zone::Moderate: return 1;
    case Zone::Vigorous:
    case Zone::Peak: return 2;
    default: return 0;
    }
}

int zoneStartBpm(Zone zone, int maxHr, int restingHr)
{
    const double reserve = qMax(1, maxHr - restingHr);
    double fraction = 0;
    switch (zone) {
    case Zone::Light: fraction = 0; break;
    case Zone::Moderate: fraction = 0.40; break;
    case Zone::Vigorous: fraction = 0.60; break;
    case Zone::Peak: fraction = 0.85; break;
    }
    return (int)std::ceil(restingHr + fraction * reserve - 1e-9);
}

ZoneMinutes zoneMinutes(const MinuteSeries &series, qint64 fromMinute, qint64 toMinute, const HealthProfile &profile,
                        int restingHr)
{
    ZoneMinutes z;
    const int maxHr = profile.maxHr();
    for (auto it = series.data().lowerBound(fromMinute); it != series.data().end() && it.key() < toMinute; ++it) {
        if (it->hr <= 0)
            continue;
        const Zone zone = zoneForBpm(it->hr, maxHr, restingHr);
        switch (zone) {
        case Zone::Light: z.light++; break;
        case Zone::Moderate: z.moderate++; break;
        case Zone::Vigorous: z.vigorous++; break;
        case Zone::Peak: z.peak++; break;
        }
        z.azm += azmForZone(zone);
    }
    return z;
}

int effectiveRestingHr(const HealthProfile &profile, int estimate)
{
    if (profile.restingHr > 0)
        return profile.restingHr;
    return estimate > 0 ? estimate : 60;
}

double strideM(const HealthProfile &profile)
{
    const double factor = profile.sex == HealthProfile::Male ? 0.415 : profile.sex == HealthProfile::Female ? 0.413 : 0.414;
    return profile.heightCm * factor / 100.0;
}

double distanceM(int steps, const HealthProfile &profile)
{
    return qMax(0, steps) * strideM(profile);
}

double bmrKcalPerDay(const HealthProfile &profile)
{
    const double constant = profile.sex == HealthProfile::Male ? 5 : profile.sex == HealthProfile::Female ? -161 : -78;
    return 10 * profile.weightKg + 6.25 * profile.heightCm - 5 * profile.age + constant;
}

double activeKcalForMinute(const Minute &minute, const HealthProfile &profile, int restingHr)
{
    double kcal = 0;
    if (minute.steps > 0) {
        const double km = distanceM(minute.steps, profile) / 1000.0;
        const double perKgKm = minute.steps >= 140 ? 1.0 : 0.5;
        kcal = perKgKm * profile.weightKg * km;
    }
    if (minute.hr > 0 && zoneForBpm(minute.hr, profile.maxHr(), restingHr) != Zone::Light) {
        const double hr = minute.hr, w = profile.weightKg, a = profile.age;
        const double male = (-55.0969 + 0.6309 * hr + 0.1988 * w + 0.2017 * a) / 4.184;
        const double female = (-20.4022 + 0.4472 * hr - 0.1263 * w + 0.074 * a) / 4.184;
        const double gross = profile.sex == HealthProfile::Male ? male
                             : profile.sex == HealthProfile::Female ? female : (male + female) / 2;
        kcal = qMax(kcal, gross - bmrKcalPerDay(profile) / 1440.0);
    }
    return qMax(0.0, kcal);
}

double energyKcal(const MinuteSeries &series, qint64 fromMs, qint64 toMs, const HealthProfile &profile, int restingHr)
{
    if (toMs <= fromMs)
        return 0;
    double kcal = bmrKcalPerDay(profile) * (double)(toMs - fromMs) / 86400000.0;
    const qint64 to = MinuteSeries::minuteOf(toMs);
    for (auto it = series.data().lowerBound(MinuteSeries::minuteOf(fromMs)); it != series.data().end() && it.key() < to; ++it)
        kcal += activeKcalForMinute(*it, profile, restingHr);
    return kcal;
}

double altitudeDifferenceM(double fromHpa, double toHpa)
{
    auto altitude = [](double hpa) { return 44330.0 * (1.0 - std::pow(hpa / 1013.25, 1.0 / 5.255)); };
    return altitude(toHpa) - altitude(fromHpa);
}

int floorsClimbed(const QList<QPointF> &pressure, const MinuteSeries &steps)
{
    static const double FLOOR_M = 3.048;
    static const double MIN_GAIN_M = 1.5;
    static const qint64 MAX_GAP_S = 40 * 60;
    static const int MIN_STEPS = 10;
    auto hpa = [](double v) { return v > 20000 ? v / 100.0 : v > 2000 ? v / 10.0 : v; };
    double gain = 0;
    bool have = false;
    qint64 lastT = 0;
    double low = 0;   // the altitude a climb is measured from (metres relative to the first record)
    double first = 0;
    for (const QPointF &r : pressure) {
        const qint64 t = (qint64)r.x();
        const double p = hpa(r.y());
        if (p < 300 || p > 1100)
            continue; // not a pressure (a reading of 0)
        if (!have) {
            first = p;
            low = 0;
            lastT = t;
            have = true;
            continue;
        }
        const double h = altitudeDifferenceM(first, p);
        const bool walked = t > lastT && t - lastT <= MAX_GAP_S &&
                            steps.stepsBetween(lastT * 1000, t * 1000) >= MIN_STEPS;
        lastT = t;
        if (!walked || h < low) {
            low = h; // no climb on foot (or down): the next one is measured from here
        } else if (h - low >= MIN_GAIN_M) {
            gain += h - low;
            low = h;
        }
    }
    return (int)std::floor(gain / FLOOR_M);
}

HourlyActivity hourlyActivity(const MinuteSeries &series, const QDate &day, int firstHour, int endHour, int stepsPerHour)
{
    HourlyActivity h;
    for (int hour = 0; hour < 24; hour++) {
        const QDateTime from(day, QTime(hour, 0));
        const QDateTime to = hour == 23 ? QDateTime(day.addDays(1), QTime(0, 0)) : QDateTime(day, QTime(hour + 1, 0));
        h.hourSteps[hour] = series.stepsBetween(from.toMSecsSinceEpoch(), to.toMSecsSinceEpoch() - 1);
    }
    h.hoursTotal = qMax(0, endHour - firstHour);
    for (int hour = qMax(0, firstHour); hour < qMin(24, endHour); hour++)
        if (h.hourSteps[hour] >= stepsPerHour)
            h.hoursMet++;
    return h;
}

} // namespace Health
