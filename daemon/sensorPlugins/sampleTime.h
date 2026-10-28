/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SAMPLETIME_H
#define SAMPLETIME_H

#include <time.h>
#include <QDateTime>

/*
 * When a reading was taken, as wall clock milliseconds. sensorfw reading
 * time stamps are CLOCK_BOOTTIME microseconds (the hybris adaptors pass the
 * Android event time; the eos sensor hub adaptors use the same base), which
 * keeps counting while the system is suspended: a reading that arrives in
 * a batch, or is only read after another suspend, keeps its own time.
 * A time stamp that cannot be one (none, in the future, over a week old)
 * counts as taken now.
 */
inline qint64 readingWallMs(quint64 timestampUs)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    const qint64 nowUs = (qint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    const qint64 wallMs = QDateTime::currentMSecsSinceEpoch();
    if (timestampUs == 0 || (qint64)timestampUs > nowUs + 1000000)
        return wallMs;
    const qint64 ageMs = (nowUs - (qint64)timestampUs) / 1000;
    if (ageMs > 7LL * 24 * 3600 * 1000)
        return wallMs;
    return wallMs - ageMs;
}

#endif // SAMPLETIME_H
