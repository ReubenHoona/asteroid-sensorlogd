/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <QDateTime>
#include <QTime>

#include "moveReminder.h"

namespace Health {

static bool windowValid(const MoveReminderSettings &s)
{
    return s.startHour >= 0 && s.endHour <= 24 && s.startHour < s.endHour;
}

static bool dayOn(const MoveReminderSettings &s, const QDate &day)
{
    return (s.days >> (day.dayOfWeek() - 1)) & 1; // Qt: 1 Monday ... 7 Sunday
}

QList<MoveEvent> MoveReminder::evaluate(qint64 nowMs, const MinuteSeries &series, const MoveReminderSettings &settings,
                                        bool worn)
{
    QList<MoveEvent> events;
    if (!settings.enabled || !windowValid(settings))
        return events;
    const QDateTime now = QDateTime::fromMSecsSinceEpoch(nowMs);
    const QDate day = now.date();
    const int hour = now.time().hour();
    if (!dayOn(settings, day) || hour < settings.startHour)
        return events;

    int steps = 0;
    if (hour < settings.endHour) {
        const qint64 hourKey = day.toJulianDay() * 24 + hour;
        const QDateTime hourStart(day, QTime(hour, 0));
        steps = series.stepsBetween(hourStart.toMSecsSinceEpoch(), nowMs);

        if (steps >= HOURLY_STEPS) {
            if (m_state.remindedHour == hourKey && m_state.completedHour != hourKey) {
                m_state.completedHour = hourKey;
                events.append({MoveEvent::HourComplete, steps, 0, 0});
            }
        } else if (now.time().minute() >= REMIND_MINUTE && worn && m_state.remindedHour != hourKey) {
            m_state.remindedHour = hourKey;
            events.append({MoveEvent::Remind, steps, HOURLY_STEPS - steps, 0});
        }
    }

    // every hour of the window met: until the day is over (a batch can come after the window ended)
    if (m_state.celebratedDay != day.toJulianDay()) {
        const HourlyActivity activity = hourlyActivity(series, day, settings.startHour, settings.endHour);
        if (activity.hoursMet == activity.hoursTotal) {
            m_state.celebratedDay = day.toJulianDay();
            events.append({MoveEvent::DayComplete, steps, 0, activity.hoursMet});
        }
    }
    return events;
}

qint64 MoveReminder::nextReminderMs(qint64 nowMs, const MoveReminderSettings &settings)
{
    if (!settings.enabled || !windowValid(settings))
        return -1;
    const QDateTime now = QDateTime::fromMSecsSinceEpoch(nowMs);
    for (int d = 0; d < 8; d++) {
        const QDate day = now.date().addDays(d);
        if (!dayOn(settings, day))
            continue;
        for (int hour = settings.startHour; hour < settings.endHour; hour++) {
            const qint64 at = QDateTime(day, QTime(hour, REMIND_MINUTE)).toMSecsSinceEpoch();
            if (at > nowMs)
                return at;
        }
    }
    return -1;
}

} // namespace Health
