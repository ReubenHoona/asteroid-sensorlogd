/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MOVEREMINDER_H
#define MOVEREMINDER_H

#include <QList>

#include "healthMetrics.h"

namespace Health {

/*
 * Reminders to move and their celebrations (health-fitness-15). Stock: goal 250 steps an hour, 9:00-18:00,
 * every day, off by default. At minute 50 of an hour under 250 steps the watch vibrates and shows "Reminder to
 * move" / "N steps left this hour" with a ring of steps/250; reaching 250 in an hour that had a reminder shows
 * "Reminder to move" / "Goal complete!"; meeting every hour of the window shows "Hourly Activity" / "N hr"
 * (RtmManager.java:613-614 the 250 cap, :1039-1050 minute 50; HourlyReminderToMoveMessage.java,
 * HourlyReminderToMoveCelebration.java, DailyReminderToMoveCelebration.java).
 *
 * evaluate() is called when data has been delivered (the sensor hub's batch, display on): never from a timer
 * of its own. A reminder is for the hour it is in: at or after minute 50, once per hour, and never for an
 * hour that is over by the time the steps are known.
 */
struct MoveReminderSettings {
    bool enabled = false;
    int startHour = 9;  // the first hour of the window
    int endHour = 18;   // the window ends at the start of this hour
    int days = 0x7f;    // bit 0 Monday ... bit 6 Sunday; all: every day
};

struct MoveEvent {
    enum Kind { Remind, HourComplete, DayComplete };
    Kind kind;
    int steps = 0;     // steps this hour
    int stepsLeft = 0; // Remind: to 250
    int hoursMet = 0;  // DayComplete: hours of the window met
};

class MoveReminder
{
public:
    static const int REMIND_MINUTE = 50;

    // what to show now. `worn`: the watch is on the wrist (a reminder is no use on a desk).
    QList<MoveEvent> evaluate(qint64 nowMs, const MinuteSeries &series, const MoveReminderSettings &settings, bool worn);

    // the time of the next reminder minute of the window after nowMs, or -1 (disabled, no hour left today)
    static qint64 nextReminderMs(qint64 nowMs, const MoveReminderSettings &settings);

    // the state that has to survive a restart: the hour reminded, the hour completed, the day of the day goal
    struct State {
        qint64 remindedHour = -1;
        qint64 completedHour = -1;
        qint64 celebratedDay = -1;
    };
    State state() const { return m_state; }
    void setState(const State &state) { m_state = state; }

private:
    State m_state;
};

} // namespace Health

#endif // MOVEREMINDER_H
