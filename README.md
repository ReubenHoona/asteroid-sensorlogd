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

## Health database
The records that are made once and may change later live in `health.db` (SQLite, WAL, `synchronous=FULL`), next to `heartrate.db`, with sensorlogd as the only writer (stock keeps the same in the Fitbit app's Room databases with one owner). Everybody else reads it with a connection of its own (`store/healthDb.h` has the C++ readers; `PRAGMA user_version` is the schema version, 1):

| table | rows |
|---|---|
| `meta` | `seq` (the change counter), `import.sleep`, `import.exercise`, `import.daily` (the one-time import, 1 when done) |
| `workout` | `id`, `start_ms` (unique), `end_ms` (NULL while on), `active_s`, `type`, `distance_m`, `kcal`, `steps`, `avg_bpm`, `max_bpm`, `light_s`, `moderate_s`, `vigorous_s`, `peak_s`, `azm`, `elev_gain_m`, `source` (manual, auto), `gps`, `imported_from` (`exercise/<day>.log`), `seq`, `deleted` |
| `workout_track` | `workout_id`, `t_ms`, `lat_e7`, `lon_e7`, `alt_dm`, `hacc_dm`, `speed_cms` (stays on the watch and the phone) |
| `workout_lap` | `workout_id`, `n`, `start_ms`, `end_ms`, `distance_m`, `avg_bpm`, `kind` (split-km, split-mile, manual) |
| `workout_pause` | `workout_id`, `start_ms`, `end_ms`, `auto` |
| `sleep_session` | `id`, `wake_date` (the local morning, ISO), `start_ms`, `end_ms`, `main`, `asleep_min`, `awake_min`, `rem_min`, `light_min`, `deep_min`, `score`, `goal_min`, `avg_hr`, `spo2_*`, `temp_dev_c`, `resp_rate`, `hrv_rmssd`, `algo`, `imported_from`, `seq`, `deleted` |
| `sleep_stage` | `session_id`, `start_ms`, `end_ms`, `stage` (0 awake, 1 light, 2 deep, 3 REM) |
| `daily` | `date` (local, ISO), `resting_hr`, `hrv_rmssd`, `resp_rate`, `temp_dev_c`, `steps`, `kcal`, `distance_m`, `floors`, `azm`, `hours_met`, `cardio_load`, `readiness`, `closed_ms`, `seq` |
| `spo2`, `hrv`, `skin_temp`, `eda_event` | for the hub streams of later work; no writer yet |

**Change sequence**: every insert or update of a `workout`, `sleep_session`, `daily` or `eda_event` row gives it the next value of the counter in its `seq` (triggers, so no write can forget it), and a change of a workout's track, laps or pauses or of a session's stages gives the parent the next one. Nothing is ever deleted: `deleted = 1` is the tombstone (a deleted workout's track, laps and pauses go with it). A sync that has seen everything up to N reads `SELECT * FROM <table> WHERE seq > N ORDER BY seq` (`HealthDb::changes()`); the counter survives restarts and never goes back. Every write is one transaction, so a reader never sees half of one and never blocks it.

**Who writes what, when**: the night, when the morning's sleep estimate is made (the first delivery 30 minutes after the night, 05:00-20:00; it replaces the morning's sessions, the old ones tombstoned); the day's row, closed on the first delivery after its midnight (and the days missed while the daemon was off, up to 31); workouts, through the D-Bus methods below. `sleep/<date>.json` is no longer written.

**One-time import** (`store/healthImport.h`), from the daemon's start, one file or day at a time through the event loop: `sleep/*.json` (each night's sessions), `exercise/*.log` (one workout a line; the log has no end, so `end_ms` = start + active time) and a closed `daily` row for every day with steps or heart rate up to yesterday. Each file or day is one transaction and everything already in is skipped, so a stop anywhere loses at most the unit in hand, and the next start carries on; each phase is marked in `meta` when it is through. The files stay as they are and the readers read them until the import has taken them in. Until the Exercise app writes through sensorlogd, its new log lines are taken in when it says a workout ended (`setWorkoutActive(false)`) and at the daemon's start.

**D-Bus** (session bus, `org.asteroid.sensorlogd.logger` at `/org/asteroid/sensorlogd/logger`): `BeginWorkout(x startMs, s type, s source, b gps) -> x id` (0: refused), `AddTrackPoints(x id, ax points)` (six numbers a fix: t_ms, lat_e7, lon_e7, alt_dm, hacc_dm, speed_cms), `AddLap(x id, i n, x startMs, x endMs, d distanceM, i avgBpm, s kind)`, `SetPaused(x id, b paused, x atMs, b auto)`, `EndWorkout(x id, x endMs, a{sv} summary)` (activeS, distanceM, kcal, steps, avgBpm, maxBpm, lightS, moderateS, vigorousS, peakS, azm, elevGainM), `DeleteWorkout(x id)`, `HealthSeq() -> x`; the signal `HealthChanged(x seq)` after every write that changed something.

**QML** (`org.asteroid.sensorlogd`): `SleepDataLoader` (as before, now from health.db), `WorkoutLoader` (`workouts(from, to)`, `recent(n)`, `last()`, `workout(id)` with laps and pauses, `track(id)`, `seq`), `HealthMetricsLoader.daily(date)` (the day's row), and the energy formula for the Exercise app: `minuteKcal(steps, bpm)` + `restingKcal(durationMs)`. Each reads again on `HealthChanged` (`dataChanged`).

**Battery**: nothing polls and nothing new wakes the system. The writes happen on deliveries that are made anyway (the hub's batch, display on), at the end of a workout and on the apps' calls; the import runs once, while the system is awake.

## Health features
The Health app's numbers and prompts are computed from what is logged, when a batch of readings has been delivered (the sensor hub's batch, a display on, a live heart rate reading that begins a minute): there is no polling and no timer of its own. The shared code is in `metrics/`.

* **One steps goal**, as stock has: the dconf key `/org/asteroidos/ux/steps-goal` (10000), read by the steps tile, the complications, the Health app and the celebrations. `LoggerSettings.stepGoalTarget` reads and writes it; there is no switch (`stepGoalEnabled` is always true). When today's steps first reach it, the logger emits the D-Bus signal `goalReached("steps", steps, goal)` (once a day).
* **Health profile**, written by Settings > Health profile: `/org/asteroidos/health/profile/` `gender` (male, female, other), `height-cm`, `weight-kg`, `birth-date` (ISO) or `age`, and `resting-hr` (unset: the mean of the last 7 days' resting heart rates, below). Unset values take neutral ones (170 cm, 70 kg, 30 years).
* **Metrics** (QML type `HealthMetricsLoader`): distance (stride 0.415 / 0.413 x height), energy burned (Mifflin-St Jeor resting energy prorated by time, plus walking and running steps, plus the heart rate above the Light zone), floors (barometer climbs while stepping, 3.048 m a floor), heart rate zones by heart rate reserve (Light below 40 %, Moderate 40-59 %, Vigorous 60-84 %, Peak 85 % and up; maximum 220 minus age), Active Zone Minutes (1 a Moderate minute, 2 a Vigorous or Peak one, goal 150 a week) and hourly activity (250 steps an hour, 9:00-18:00). `summary(date)` has all of them.
* **Heart rate alerts**: 10 minutes in a row with fewer than 10 steps a minute and a mean heart rate above the high threshold (or below the low one) post the notification "High heart rate" / "Low heart rate" ("N bpm", detail "Your heart rate rose above N bpm while you appeared to be inactive from T1 - T2."). On by default. Settings `hrAlerts/highEnabled`, `highBpm`, `lowEnabled`, `lowBpm` (0: none set; then stock's defaults, high 120 and low 50 bpm: `CLIENT_SIDE_DEFAULT`, D/abgd.java:431,449; a value the user set stays in force).
* **Resting heart rate** (`metrics/restingHr.h`), one definition for the Today card and its page, zones, energy, sleep and the sync. Stock "estimates it once a day" (in the cloud); ours: the median heart rate of the resting minutes of 24 hours (worn, no step in the minute or the 10 before it, no workout on; at least 60 of them), estimated once for each local day and kept in health.db `daily.resting_hr`: in the morning when the night is stored (the 24 hours up to the night's end), else when the day is closed (the day itself). What uses a resting rate takes the profile's, else the mean of the last 7 days', else 60. `HealthMetricsLoader.restingHrOf(date)` reads a day's.
* **Reminders to move**: off by default; settings `reminders/enabled`, `startHour` (9), `endHour` (18), `days` (bit 0 Monday). At minute 50 of an hour of the window under 250 steps: "Reminder to move" / "N steps left this hour"; reaching 250 in that hour: "Goal complete!"; every hour met: "Hourly Activity" / "N hr". The celebrations are notifications with the hint `x-asteroid-health` (JSON: type, steps, goal) for the launcher to draw.
* **Sleep**: estimated each morning from the night's step records and heart rate (no new sensor: bedtime, wake time, awake time, stages by heart rate, score, naps) and stored in health.db (`sleep_session`); `SleepDataLoader` reads it (or estimates it if the daemon has not yet).
* **Exercise prompts**: 10 minutes of walking or running cadence offers "Start tracking?"; 5 quiet minutes in a workout (`setWorkoutActive`) offers "Finished working out?". Settings `autoExercise/start`, `autoExercise/stop`. The logger emits `exerciseOffered(action, activity, since)` too.

Tests: `tests/` (`tst_health`, `tst_monitor`, `tst_healthloaders` for these features; `tst_store` and `tst_api` for health.db, its import and its D-Bus methods).
