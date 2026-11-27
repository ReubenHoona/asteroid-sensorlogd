/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <cmath>

#include <MDConfItem>

#include "healthProfile.h"

namespace Health {

static const char *const PROFILE = "/org/asteroidos/health/profile/";

HealthProfile HealthProfile::fromValues(const QVariant &gender, const QVariant &heightCm, const QVariant &weightKg,
                                        const QVariant &birthDate, const QVariant &age, const QVariant &restingHr,
                                        const QDate &today)
{
    HealthProfile p;
    if (gender.isValid()) {
        const QString g = gender.toString().trimmed().toLower();
        if (g == "male" || g == "m" || g == "1")
            p.sex = Male;
        else if (g == "female" || g == "f" || g == "2")
            p.sex = Female;
    }
    bool ok = false;
    double v = heightCm.toDouble(&ok);
    if (ok && v >= 50 && v <= 260)
        p.heightCm = std::round(v);
    v = weightKg.toDouble(&ok);
    if (ok && v >= 20 && v <= 400)
        p.weightKg = v;
    const QDate born = QDate::fromString(birthDate.toString(), Qt::ISODate);
    if (born.isValid() && born < today) {
        int years = today.year() - born.year();
        if (today.month() < born.month() || (today.month() == born.month() && today.day() < born.day()))
            years--;
        if (years >= 5 && years <= 120)
            p.age = years;
    } else {
        const int a = age.toInt(&ok);
        if (ok && a >= 5 && a <= 120)
            p.age = a;
    }
    const int r = restingHr.toInt(&ok);
    if (ok && r >= 30 && r <= 120)
        p.restingHr = r;
    return p;
}

int HealthProfile::sleepGoalFrom(const QVariant &minutes)
{
    bool ok = false;
    const int m = minutes.toInt(&ok);
    return ok && m >= 60 && m <= 1440 ? m : 480;
}

HealthProfile HealthProfile::load(const QDate &today)
{
    auto get = [](const char *key) { return MDConfItem(QString(PROFILE) + key).value(); };
    HealthProfile p = fromValues(get("gender"), get("height-cm"), get("weight-kg"), get("birth-date"), get("age"),
                                 get("resting-hr"), today);
    p.sleepGoalMin = sleepGoalFrom(MDConfItem(QStringLiteral("/org/asteroidos/ux/sleep-goal")).value());
    return p;
}

} // namespace Health
