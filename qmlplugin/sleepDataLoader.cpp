/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <QDateTime>

#include "sleepDataLoader.h"
#include "../common.h"
#include "../metrics/healthData.h"

using namespace Health;

SleepDataLoader::SleepDataLoader() : QObject()
{
    connectHealthChanged(this, SLOT(onHealthChanged(qlonglong)));
}

QVariantMap SleepDataLoader::forDate(QDate wakeDate)
{
    SleepNight night;
    const bool stored = loadStoredSleep(wakeDate, &night);
    if (!stored) {
        // the resting heart rate matters only to an estimate made now
        const HealthProfile profile = HealthProfile::load();
        const int estimate = profile.restingHr > 0 ? 0 : estimatedRestingHr(QDateTime::currentMSecsSinceEpoch());
        night = estimateNight(wakeDate, profile, effectiveRestingHr(profile, estimate));
    }
    QVariantMap m = night.toMap();
    m["stored"] = stored;
    m["wakeDate"] = wakeDate;
    return m;
}

QVariantMap SleepDataLoader::lastNight()
{
    const QDate today = QDate::currentDate();
    const QVariantMap now = forDate(today);
    if (now.value("valid").toBool())
        return now;
    const QVariantMap before = forDate(today.addDays(-1));
    return before.value("valid").toBool() ? before : now;
}
