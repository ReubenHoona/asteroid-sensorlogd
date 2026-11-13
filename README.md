# Asteroid-sensorlogd
asteroid-sensorlogd is an extensible sensor logger which is intended for health tracking, which depends on nemo's MCE and Asteroid's QtSensors which was modified to add heartrate capability.
The project comes as two parts: the sensor logging daemon, which runs in the background to trigger sensor recordings, and the qml module which provides easy access to it. There is currently no facility for the data to be accessible to other languages.
## Recording triggers and power management
Sensorlogd will only record data when the CPU is already running. It does not schedule any wakeups, so if your device isn't woken up already, there will be no recordings. This is intended as a crude form of wear detection and as a power-saving measure. Better solutions can probably be implemented.

When sensorlogd receives the `displayOn` signal from MCE, it assumes this means 'the system has come out of suspend, so the system clock has significantly checked since the last time the daemon was aware of the time'. Each sensor plugin will then check if enough time has elapsed between readings.
## File format
For each given sensor, sensorlogd will create log files named `[year]-[month]-[day].log`, eg. `2023-03-12.log`. Each day a new file will be created. This structure is intended to make it fast to retrieve data for a single day with minimal seeking.

Every heart rate reading is also kept, with the time it was taken, in an SQLite database next to the sensor folders, `heartrate.db` (WAL mode, so other programs can read it while sensorlogd writes):

    CREATE TABLE heartrate (time INTEGER PRIMARY KEY, bpm INTEGER NOT NULL, accuracy INTEGER NOT NULL)

`time` is in milliseconds since the epoch (UTC), `accuracy` is the sensor's `QHrmSensor::Status` (1 unreliable to 4 high; readings without contact are not kept). A reading that arrives twice is kept once. Sensorlogd never deletes anything from it. The `heartrateMonitor` day files keep one median record per interval, as before.

## Health features
The Health app's numbers and prompts are computed from what is logged, when a batch of readings has been delivered (the sensor hub's batch, a display on, a live heart rate reading that begins a minute): there is no polling and no timer of its own. The shared code is in `metrics/`.

* **One steps goal**, as stock has: the dconf key `/org/asteroidos/ux/steps-goal` (10000), read by the steps tile, the complications, the Health app and the celebrations. `LoggerSettings.stepGoalTarget` reads and writes it; there is no switch (`stepGoalEnabled` is always true). When today's steps first reach it, the logger emits the D-Bus signal `goalReached("steps", steps, goal)` (once a day).
* **Health profile**, written by Settings > Health profile: `/org/asteroidos/health/profile/` `gender` (male, female, other), `height-cm`, `weight-kg`, `birth-date` (ISO) or `age`, and `resting-hr` (unset: estimated from a week of readings). Unset values take neutral ones (170 cm, 70 kg, 30 years).
* **Metrics** (QML type `HealthMetricsLoader`): distance (stride 0.415 / 0.413 x height), energy burned (Mifflin-St Jeor resting energy prorated by time, plus walking and running steps, plus the heart rate above the Light zone), floors (barometer climbs while stepping, 3.048 m a floor), heart rate zones by heart rate reserve (Light below 40 %, Moderate 40-59 %, Vigorous 60-84 %, Peak 85 % and up; maximum 220 minus age), Active Zone Minutes (1 a Moderate minute, 2 a Vigorous or Peak one, goal 150 a week) and hourly activity (250 steps an hour, 9:00-18:00). `summary(date)` has all of them.
* **Heart rate alerts**: 10 minutes in a row with fewer than 10 steps a minute and a mean heart rate above the high threshold (or below the low one) post the notification "High heart rate" / "Low heart rate" ("N bpm", detail "Your heart rate rose above N bpm while you appeared to be inactive from T1 - T2."). On by default. Settings `hrAlerts/highEnabled`, `highBpm`, `lowEnabled`, `lowBpm` (0: from age and resting heart rate).
* **Reminders to move**: off by default; settings `reminders/enabled`, `startHour` (9), `endHour` (18), `days` (bit 0 Monday). At minute 50 of an hour of the window under 250 steps: "Reminder to move" / "N steps left this hour"; reaching 250 in that hour: "Goal complete!"; every hour met: "Hourly Activity" / "N hr". The celebrations are notifications with the hint `x-asteroid-health` (JSON: type, steps, goal) for the launcher to draw.
* **Sleep**: estimated each morning from the night's step records and heart rate (no new sensor: bedtime, wake time, awake time, stages by heart rate, score, naps) and stored in `sleep/<wake date>.json`; `SleepDataLoader` reads it (or estimates it if the daemon has not yet).
* **Exercise prompts**: 10 minutes of walking or running cadence offers "Start tracking?"; 5 quiet minutes in a workout (`setWorkoutActive`) offers "Finished working out?". Settings `autoExercise/start`, `autoExercise/stop`. The logger emits `exerciseOffered(action, activity, since)` too.

Tests: `tests/` (`tst_health`, `tst_monitor`, `tst_healthloaders` for these features).
