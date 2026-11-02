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
 *
 * The same time stamp gives the same time whenever it is read (a reading
 * that comes twice must be found twice in the heart rate database): the
 * wall clock's offset to CLOCK_BOOTTIME is kept until it moves by more than
 * WALL_OFFSET_SLACK_US (the clock was set), instead of being taken anew,
 * with its rounding, for every reading.
 */
static const qint64 WALL_OFFSET_SLACK_US = 50000;

inline qint64 readingWallMs(quint64 timestampUs)
{
    struct timespec boot, wall;
    clock_gettime(CLOCK_BOOTTIME, &boot);
    clock_gettime(CLOCK_REALTIME, &wall);
    const qint64 nowUs = (qint64)boot.tv_sec * 1000000 + boot.tv_nsec / 1000;
    const qint64 offsetUs = (qint64)wall.tv_sec * 1000000 + wall.tv_nsec / 1000 - nowUs;
    static qint64 keptOffsetUs = offsetUs;
    if (qAbs(offsetUs - keptOffsetUs) > WALL_OFFSET_SLACK_US)
        keptOffsetUs = offsetUs;
    if (timestampUs == 0 || (qint64)timestampUs > nowUs + 1000000)
        return (nowUs + offsetUs) / 1000;
    if (nowUs - (qint64)timestampUs > 7LL * 24 * 3600 * 1000000)
        return (nowUs + offsetUs) / 1000;
    return ((qint64)timestampUs + keptOffsetUs) / 1000;
}

#endif // SAMPLETIME_H
