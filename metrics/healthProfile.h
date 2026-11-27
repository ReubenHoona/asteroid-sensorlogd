/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HEALTHPROFILE_H
#define HEALTHPROFILE_H

#include <QDate>
#include <QStringList>
#include <QVariant>

namespace Health {

/*
 * The Health profile of Settings (stock: Settings > Health profile, health-fitness-12): gender, height, weight
 * and the date of birth that stock takes from the phone. The Settings app writes them as dconf keys, which
 * everything that needs distance, energy or heart rate zones reads here:
 *
 *   /org/asteroidos/health/profile/gender      s  "male" | "female" | "other" (also 1 male, 2 female)
 *   /org/asteroidos/health/profile/height-cm   i  (a double is rounded)
 *   /org/asteroidos/health/profile/weight-kg   d
 *   /org/asteroidos/health/profile/birth-date  s  ISO yyyy-MM-dd; else
 *   /org/asteroidos/health/profile/age         i  years
 *   /org/asteroidos/health/profile/resting-hr  i  bpm; 0 or unset: the daily estimates (restingHr.h)
 *   /org/asteroidos/ux/sleep-goal              i  minutes a night, 60 to 1440 (the sleep score's duration part
 *                                                 and "goal met"); stock sets it in the phone app, here
 *                                                 asteroid-btsyncd writes it from Gadgetbridge (M4 WO-11)
 *
 * Anything unset takes a neutral value (170 cm, 70 kg, 30 years, no gender, 480 minutes of sleep), so that the
 * numbers exist before the profile is filled in. asteroid-btsyncd also writes the four keys of Settings and the
 * birth date from the phone's profile.
 */
struct HealthProfile {
    enum Sex { Unspecified, Male, Female };

    Sex sex = Unspecified;
    double heightCm = 170;
    double weightKg = 70;
    int age = 30;
    int restingHr = 0; // 0: not set
    int sleepGoalMin = 480;

    // the maximum heart rate stock's zones use: 220 minus the age
    int maxHr() const { return 220 - age; }

    // from the dconf keys above (today: the day the age is counted to)
    static HealthProfile load(const QDate &today = QDate::currentDate());
    // the keys load() reads: gender, height, weight, birth date, age, resting heart rate, then the sleep goal (a
    // reader that keeps the profile watches them)
    static QStringList keys();
    // from the values of keys(), in that order (invalid QVariant: unset); load() reads the keys and calls this
    static HealthProfile fromKeyValues(const QVariantList &values, const QDate &today);
    // from the raw values of the keys (invalid QVariant: unset); load() reads the keys and calls this
    static HealthProfile fromValues(const QVariant &gender, const QVariant &heightCm, const QVariant &weightKg,
                                    const QVariant &birthDate, const QVariant &age, const QVariant &restingHr,
                                    const QDate &today);
    // the sleep goal from the key's raw value (invalid QVariant: unset)
    static int sleepGoalFrom(const QVariant &minutes);
};

} // namespace Health

#endif // HEALTHPROFILE_H
