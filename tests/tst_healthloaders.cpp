// The QML plugin's Health classes: HealthMetricsLoader (distance, energy, floors, zones, Active Zone Minutes and
// hourly activity of a day from the step day files, the heart rate database and the barometer, with the Health
// profile; the one steps goal), SleepDataLoader (the stored night, or one estimated from the logs) and the
// LoggerSettings of the Health features (the single steps goal in dconf, reminders, alerts, auto exercise, each
// change told to the daemon over D-Bus), WorkoutLoader, the day rows of health.db, the energy formula for the
// Exercise app, and the loaders reading again when the daemon says health.db changed. The days are the day before
// today (a whole day; today is cut at now).
#include <QDBusConnection>
#include <QDBusMessage>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>

#include "../common.h"
#include "../metrics/healthData.h"
#include "../qmlplugin/healthMetricsLoader.h"
#include "../qmlplugin/loggerSettings.h"
#include "../qmlplugin/sleepDataLoader.h"
#include "../qmlplugin/workoutLoader.h"
#include "../store/healthDb.h"
#include "../store/healthImport.h"
#include "peek.h"
#include "testenv.h"

using namespace Health;

class FakeLogger : public QObject
{
    Q_OBJECT
public:
    int healthCalls = 0;
    QList<bool> workouts;
public slots:
    void healthSettingsChanged() { healthCalls++; }
    void setWorkoutActive(bool active) { workouts << active; }
    void resetup() {}
};

namespace {

qint64 ms(const QDate &d, int h, int m = 0, int s = 0) { return QDateTime(d, QTime(h, m, s)).toMSecsSinceEpoch(); }

QByteArray rec(qint64 timeMs, int value) { return QByteArray::number(timeMs / 1000) + ":" + QByteArray::number(value) + "\n"; }

void profileMale175()
{
    MGConfItem::setTestValue("/org/asteroidos/health/profile/gender", "male");
    MGConfItem::setTestValue("/org/asteroidos/health/profile/height-cm", 175);
    MGConfItem::setTestValue("/org/asteroidos/health/profile/weight-kg", 70);
    MGConfItem::setTestValue("/org/asteroidos/health/profile/age", 30);
    MGConfItem::setTestValue("/org/asteroidos/health/profile/resting-hr", 60);
}

}

class TestHealthLoaders : public QObject
{
    Q_OBJECT
    FakeLogger daemon;
    QDate day() const { return QDate::currentDate().addDays(-1); }

    void addHeart(const QDate &d, int h, int m, int minutes, int bpm, int accuracy = 4)
    {
        HeartrateDb db(heartrateDbPath());
        QVector<HeartrateDb::Reading> rows;
        for (int i = 0; i < minutes; i++)
            rows.append({ms(d, h, m) + (qint64)i * 60000 + 30000, bpm, accuracy});
        QVERIFY(db.add(rows));
    }

    // a day with: steps 300 by 9:09 and 600 by 9:19 (hour 9: 600), 1100 by 14:10 (hour 14: 500); heart rate 10
    // moderate minutes at 7:00 and 5 vigorous at 7:10; the barometer climbing 10 m at 9:00-9:10
    void makeDay(const QDate &d)
    {
        testenv::writeDay("stepCounter", d, rec(ms(d, 9, 9), 300) + rec(ms(d, 9, 19), 600) + rec(ms(d, 14, 10), 1100));
        addHeart(d, 7, 0, 10, 120);
        addHeart(d, 7, 10, 5, 150);
        testenv::writeDay("barometer-raw", d, rec(ms(d, 9, 0), 100000) + rec(ms(d, 9, 10), 99880));
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QDBusConnection::sessionBus().isConnected(), "run under dbus-run-session");
        QVERIFY(QDBusConnection::sessionBus().registerObject("/org/asteroid/sensorlogd/logger", &daemon,
                                                             QDBusConnection::ExportAllSlots));
        QVERIFY(QDBusConnection::sessionBus().registerService("org.asteroid.sensorlogd.logger"));
    }

    void init()
    {
        testenv::clean();
        MGConfItem::clearTestValues();
        for (const char *p : {"stepCounter", "heartrateMonitor", "barometer-raw", "sleep"})
            setupFilePath(p);
        daemon.healthCalls = 0;
        daemon.workouts.clear();
    }

    // ---- the metrics of a day ----
    void aDaysMetrics()
    {
        profileMale175();
        makeDay(day());
        HealthMetricsLoader loader;
        const QVariantMap s = loader.summary(day());
        QCOMPARE(s["steps"].toInt(), 1100);
        QVERIFY(qAbs(s["distanceM"].toDouble() - 1100 * 0.72625) < 1e-6);
        QVERIFY(qAbs(s["strideM"].toDouble() - 0.72625) < 1e-9);
        QCOMPARE(s["floors"].toInt(), 3);
        QCOMPARE(s["zoneModerate"].toInt(), 10);
        QCOMPARE(s["zoneVigorous"].toInt(), 5);
        QCOMPARE(s["zoneLight"].toInt(), 0);
        QCOMPARE(s["azm"].toInt(), 10 + 2 * 5);
        QCOMPARE(s["hoursTotal"].toInt(), 9);
        QCOMPARE(s["hoursMet"].toInt(), 2); // hour 9 (600) and hour 14 (500)
        QCOMPARE(s["hourSteps"].toList()[9].toInt(), 600);
        QCOMPARE(s["hourSteps"].toList()[14].toInt(), 500);
        QCOMPARE(s["restingHr"].toInt(), 60);
        QCOMPARE(s["maxHr"].toInt(), 190);
        // energy: the whole day at rest, plus the active minutes
        const double energy = s["energyKcal"].toDouble();
        QVERIFY(energy > 1648.75);
        QVERIFY(energy < 1648.75 + 400);
        // the convenience getters are the same numbers
        QCOMPARE(loader.floors(day()), 3);
        QCOMPARE(loader.azm(day()), 20);
        QVERIFY(qAbs(loader.distanceM(day()) - 1100 * 0.72625) < 1e-6);
        QCOMPARE(loader.energyKcal(day()), (int)qRound(energy));
        const QVariantMap h = loader.hourly(day());
        QCOMPARE(h["hoursMet"].toInt(), 2);
        QCOMPARE(h["stepsPerHour"].toInt(), 250);
    }

    void anEmptyDayIsRestingEnergyOnly()
    {
        profileMale175();
        HealthMetricsLoader loader;
        const QVariantMap s = loader.summary(day());
        QCOMPARE(s["steps"].toInt(), 0);
        QCOMPARE(s["floors"].toInt(), 0);
        QCOMPARE(s["azm"].toInt(), 0);
        QCOMPARE(s["hoursMet"].toInt(), 0);
        QVERIFY(qAbs(s["energyKcal"].toDouble() - 1648.75) < 1e-6);
    }

    void todayIsCutAtNow()
    {
        profileMale175();
        HealthMetricsLoader loader;
        const QVariantMap s = loader.summary(QDate::currentDate());
        const qint64 sinceMidnight = QDateTime::currentMSecsSinceEpoch() - QDateTime(QDate::currentDate(), QTime(0, 0)).toMSecsSinceEpoch();
        QVERIFY(qAbs(s["energyKcal"].toDouble() - 1648.75 * sinceMidnight / 86400000.0) < 2.0);
    }

    void aWeeksZoneMinutesRunFromMonday()
    {
        profileMale175();
        // the day before today and the day before that, when they are in the same week as the first
        const QDate d = day();
        makeDay(d);
        HealthMetricsLoader loader;
        QCOMPARE(loader.azm(d), 20);
        QCOMPARE(loader.azmWeek(d), 20 + (d.dayOfWeek() > 1 ? loader.azm(d.addDays(-1)) : 0));
        QCOMPARE(loader.azmWeeklyGoal(), 150);
        QCOMPARE(loader.hourlyStepGoal(), 250);
        if (d.dayOfWeek() > 1) {
            const QDate before = d.addDays(-1);
            makeDay(before);
            QCOMPARE(loader.azmWeek(d), 40);
        }
    }

    void readingsBelowTheAccuracyAreNotCounted()
    {
        profileMale175();
        addHeart(day(), 7, 0, 10, 150, 2); // AccuracyLow: not logged for the metrics
        HealthMetricsLoader loader;
        QCOMPARE(loader.azm(day()), 0);
    }

    void theZonesOfTheProfile()
    {
        profileMale175();
        HealthMetricsLoader loader;
        const QVariantList z = loader.zones();
        QCOMPARE(z.size(), 4);
        const QList<int> starts = {60, 112, 138, 171}, ends = {111, 137, 170, 190};
        const QStringList names = {"Light", "Moderate", "Vigorous", "Peak"};
        for (int i = 0; i < 4; i++) {
            QCOMPARE(z[i].toMap()["name"].toString(), names[i]);
            QCOMPARE(z[i].toMap()["startBpm"].toInt(), starts[i]);
            QCOMPARE(z[i].toMap()["endBpm"].toInt(), ends[i]);
        }
        QCOMPARE(loader.zoneForBpm(100), 0);
        QCOMPARE(loader.zoneForBpm(130), 1);
        QCOMPARE(loader.zoneForBpm(150), 2);
        QCOMPARE(loader.zoneForBpm(180), 3);
        QCOMPARE(loader.restingHr(), 60);
        QCOMPARE(loader.maxHr(), 190);
        QCOMPARE(loader.defaultHighBpm(), 120); // stock's, whatever the profile
        QCOMPARE(loader.defaultLowBpm(), 50);
    }

    void theRestingRateIsEstimatedWhenTheProfileHasNone()
    {
        MGConfItem::setTestValue("/org/asteroidos/health/profile/age", 40);
        HealthMetricsLoader loader;
        QCOMPARE(loader.restingHr(), 60); // no daily estimate: the default
        const QDate today = QDate::currentDate();
        addHeart(today.addDays(-1), 2, 0, 100, 52);
        QCOMPARE(loader.restingHr(), 60); // readings are not a resting rate: the daily estimate is
        {
            HealthDb db(healthDbPath());
            QVERIFY(db.setDailyRestingHr(today.addDays(-1), 52));
            QVERIFY(db.setDailyRestingHr(today.addDays(-3), 57));
            QVERIFY(db.setDailyRestingHr(today.addDays(-7), 40)); // more than a week ago
        }
        QCOMPARE(loader.restingHr(), 55); // the mean of the last 7 days', 54.5
        QCOMPARE(loader.restingHrOf(today.addDays(-1)), 52);
        QCOMPARE(loader.restingHrOf(today.addDays(-2)), -1);
        QCOMPARE(loader.maxHr(), 180);
        MGConfItem::setTestValue("/org/asteroidos/health/profile/resting-hr", 47);
        QCOMPARE(loader.restingHr(), 47);
    }

    void oneStepsGoal()
    {
        HealthMetricsLoader loader;
        QCOMPARE(loader.stepGoal(), 10000);
        MGConfItem::setTestValue("/org/asteroidos/ux/steps-goal", 7500);
        QCOMPARE(loader.stepGoal(), 7500);
    }

    // ---- sleep ----
    void theSleepLoaderEstimatesThenReadsTheStoredNight()
    {
        const QDate wake = QDate::currentDate();
        const QDate eve = wake.addDays(-1);
        addHeart(eve, 23, 0, 420, 56);
        SleepDataLoader loader;
        QVariantMap n = loader.forDate(wake);
        QVERIFY(n["valid"].toBool());
        QVERIFY(!n["stored"].toBool());
        const QVariantMap main = n["main"].toMap();
        QCOMPARE(main["startMs"].toLongLong(), ms(eve, 23, 0));
        QCOMPARE(main["asleepMin"].toInt(), 420);
        QCOMPARE(main["durationMin"].toInt(), 420);
        QCOMPARE(n["goalMin"].toInt(), 480);
        QVERIFY(!n["goalMet"].toBool());
        QVERIFY(!n["scoreLabel"].toString().isEmpty());

        // the daemon stores it in health.db: the same night, read from there
        SleepNight night = estimateNight(wake, HealthProfile(), 60);
        {
            HealthDb db(healthDbPath());
            QVERIFY(db.storeNight(night, HealthImport::SLEEP_ALGO));
            // an invalid night is not stored
            QVERIFY(!db.storeNight(SleepNight(), HealthImport::SLEEP_ALGO));
        }
        QVERIFY(!QFile::exists(sleepFilePath(wake))); // no file any more
        n = loader.forDate(wake);
        QVERIFY(n["stored"].toBool());
        QCOMPARE(n["main"].toMap()["asleepMin"].toInt(), 420);
        QCOMPARE(n["score"].toInt(), night.score);
        QVariantMap expected = night.toMap();
        expected["stored"] = true;
        expected["wakeDate"] = wake;
        QCOMPARE(n, expected);
    }

    void aNightOfTheOldFilesIsReadUntilItIsImported()
    {
        // a sensorlogd from before health.db stored the night in sleep/<date>.json
        const QDate wake = QDate::currentDate().addDays(-2);
        SleepNight old;
        old.valid = true;
        old.wakeDate = wake;
        old.main.startMs = ms(wake.addDays(-1), 23, 30);
        old.main.endMs = ms(wake, 6, 30);
        old.main.asleepMin = 400;
        old.main.awakeMin = 20;
        old.main.deepMin = 80;
        old.main.remMin = 80;
        old.main.lightMin = 240;
        old.main.avgHr = 54.5;
        old.score = 82;
        old.scoreLabel = sleepScoreLabel(82);
        QFile f(sleepFilePath(wake));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(old.toJson()).toJson(QJsonDocument::Compact));
        f.close();
        SleepDataLoader loader;
        const QVariantMap fromFile = loader.forDate(wake);
        QVERIFY(fromFile["stored"].toBool());
        QCOMPARE(fromFile["main"].toMap()["asleepMin"].toInt(), 400);
        // imported: the same answer, from health.db
        HealthDb db(healthDbPath());
        QCOMPARE(HealthImport::importSleepFile(&db, loggerRootPath(), wake.toString("yyyy-MM-dd") + ".json"), 1);
        QVERIFY(QFile::remove(sleepFilePath(wake))); // (only to see where it is read from)
        QCOMPARE(loader.forDate(wake), fromFile);
        // once the import is through, the files are not read: health.db alone says what is stored
        const QDate other = wake.addDays(-1);
        old.wakeDate = other;
        QFile g(sleepFilePath(other));
        QVERIFY(g.open(QIODevice::WriteOnly));
        g.write(QJsonDocument(old.toJson()).toJson(QJsonDocument::Compact));
        g.close();
        QVERIFY(loader.forDate(other)["stored"].toBool());
        QVERIFY(db.setMeta("import.sleep", 1));
        QVERIFY(!loader.forDate(other)["stored"].toBool());
    }

    // ---- workouts and days of health.db ----
    void theWorkoutLoaderReadsHealthDb()
    {
        WorkoutLoader loader;
        QVERIFY(loader.recent(3).isEmpty()); // no database yet
        QVERIFY(loader.last().isEmpty());
        QVERIFY(!QFile::exists(healthDbPath())); // and a reader does not make one
        const QDate d = day();
        HealthDb db(healthDbPath());
        WorkoutRecord w;
        w.startMs = ms(d, 7, 0);
        w.endMs = ms(d, 7, 40);
        w.activeS = 2300;
        w.type = "walk";
        w.distanceM = 3100;
        w.kcal = 190;
        w.importedFrom = "exercise/x.log";
        QVERIFY(db.importWorkout(w));
        const qint64 on = db.beginWorkout(ms(d, 18, 0), "bike", "auto", false);
        QVERIFY(on > 0);
        const QVariantList list = loader.workouts(d, d);
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].toMap()["id"].toLongLong(), on); // newest first
        QVERIFY(!list[0].toMap()["ended"].toBool());
        QCOMPARE(list[0].toMap()["source"].toString(), QString("auto"));
        const QVariantMap walk = list[1].toMap();
        QCOMPARE(walk["type"].toString(), QString("walk"));
        QCOMPARE(walk["activeS"].toInt(), 2300);
        QCOMPARE(walk["distanceM"].toDouble(), 3100.0);
        QVERIFY(walk["imported"].toBool());
        QCOMPARE(loader.last()["type"].toString(), QString("walk")); // the newest finished one
        QCOMPARE(loader.workouts(d.addDays(1), d.addDays(1)).size(), 0);
        QCOMPARE(loader.workouts(d, d.addDays(-1)).size(), 0);
        QVERIFY(loader.workout(999).isEmpty());
        QCOMPARE(loader.seq(), (qlonglong)db.seq());
    }

    void aDaysRowIsReadBack()
    {
        HealthMetricsLoader loader;
        QVERIFY(loader.daily(day()).isEmpty());
        HealthDb db(healthDbPath());
        DailyTotals t;
        t.steps = 9100;
        t.kcal = 2300.5;
        t.distanceM = 6600;
        t.floors = 6;
        t.azm = 31;
        t.hoursMet = 8;
        QVERIFY(db.setDailyTotals(day(), t, ms(QDate::currentDate(), 0, 20)));
        const QVariantMap m = loader.daily(day());
        QCOMPARE(m["date"].toDate(), day());
        QVERIFY(m["closed"].toBool());
        QCOMPARE(m["steps"].toInt(), 9100);
        QCOMPARE(m["kcal"].toDouble(), 2300.5);
        QCOMPARE(m["floors"].toInt(), 6);
        QCOMPARE(m["azm"].toInt(), 31);
        QCOMPARE(m["hoursMet"].toInt(), 8);
        QCOMPARE(m["closedMs"].toLongLong(), ms(QDate::currentDate(), 0, 20));
    }

    void theEnergyFormulaIsTheOneOfTheHealthNumbers()
    {
        profileMale175();
        HealthMetricsLoader loader;
        const HealthProfile p = HealthProfile::load();
        Minute walking;
        walking.steps = 110;
        Minute hard;
        hard.steps = 150;
        hard.hr = 160;
        hard.hrCount = 1;
        QCOMPARE(loader.minuteKcal(110, 0), activeKcalForMinute(walking, p, 60));
        QCOMPARE(loader.minuteKcal(150, 160), activeKcalForMinute(hard, p, 60));
        QCOMPARE(loader.minuteKcal(0, 0), 0.0);
        QCOMPARE(loader.restingKcal(3600000), bmrKcalPerDay(p) * 3600000 / 86400000.0);
        QCOMPARE(loader.restingKcal(-5), 0.0);
        // a workout's energy, minute by minute, is energyKcal() of the same minutes
        MinuteSeries series;
        const qint64 t0 = ms(day(), 10, 0);
        for (int i = 0; i < 30; i++) {
            series.addSteps(t0 + i * 60000LL + 1000, 150);
            series.addHr(t0 + i * 60000LL + 2000, 160);
        }
        double sum = loader.restingKcal(30 * 60000.0);
        for (int i = 0; i < 30; i++)
            sum += loader.minuteKcal(150, 160);
        QVERIFY(qAbs(sum - energyKcal(series, t0, t0 + 30 * 60000LL, p, 60)) < 1e-9);
    }

    void theLoadersReadAgainWhenTheDaemonWrites()
    {
        SleepDataLoader sleep;
        HealthMetricsLoader metrics;
        WorkoutLoader workouts;
        QSignalSpy s1(&sleep, &SleepDataLoader::dataChanged);
        QSignalSpy s2(&metrics, &HealthMetricsLoader::dataChanged);
        QSignalSpy s3(&workouts, &WorkoutLoader::dataChanged);
        QDBusMessage signal = QDBusMessage::createSignal("/org/asteroid/sensorlogd/logger",
                                                         "org.asteroid.sensorlogd.logger", "HealthChanged");
        signal << qlonglong(7);
        QVERIFY(QDBusConnection::sessionBus().send(signal));
        QTRY_COMPARE(s1.count(), 1);
        QTRY_COMPARE(s2.count(), 1);
        QTRY_COMPARE(s3.count(), 1);
    }

    void lastNightIsTodaysOrYesterdays()
    {
        SleepDataLoader loader;
        QVERIFY(!loader.lastNight()["valid"].toBool()); // "No recent data. Wear your watch to sleep."
        const QDate yesterday = QDate::currentDate().addDays(-1);
        addHeart(yesterday.addDays(-1), 23, 0, 400, 56);
        const QVariantMap n = loader.lastNight();
        QVERIFY(n["valid"].toBool());
        QCOMPARE(n["wakeDate"].toDate(), yesterday);
        addHeart(QDate::currentDate().addDays(-1), 23, 0, 300, 56);
        QCOMPARE(loader.lastNight()["wakeDate"].toDate(), QDate::currentDate()); // today's night first
    }

    // ---- the settings ----
    void theStepGoalIsTheOneDconfKey()
    {
        LoggerSettings s;
        QSignalSpy changed(&s, &LoggerSettings::stepGoalChanged);
        QCOMPARE(s.getStepGoalTarget(), 10000);
        QVERIFY(s.getStepGoalEnabled()); // there is no switch: always on
        s.setStepGoalEnabled(false);
        QVERIFY(s.getStepGoalEnabled());
        s.setStepGoalTarget(12000);
        QCOMPARE(MGConfItem::testValue("/org/asteroidos/ux/steps-goal").toInt(), 12000);
        QCOMPARE(s.getStepGoalTarget(), 12000);
        QVERIFY(changed.count() >= 1);
        s.setStepGoalTarget(0); // not a goal
        QCOMPARE(s.getStepGoalTarget(), 12000);
        // the old goal0 keys are not read any more
        testenv::setSetting("stepCounter/goal0/target", 5000);
        QCOMPARE(s.getStepGoalTarget(), 12000);
        // the same value as the tile and the complications read
        HealthMetricsLoader loader;
        QCOMPARE(loader.stepGoal(), 12000);
    }

    void theHealthSettingsHaveStocksDefaults()
    {
        LoggerSettings s;
        QVERIFY(!s.getRemindersEnabled()); // off by default, as stock
        QCOMPARE(s.getRemindersStartHour(), 9);
        QCOMPARE(s.getRemindersEndHour(), 18);
        QCOMPARE(s.getRemindersDays(), 127);
        QVERIFY(s.getHrAlertHighEnabled()); // on by default
        QVERIFY(s.getHrAlertLowEnabled());
        QCOMPARE(s.getHrAlertHighBpm(), 0); // automatic
        QCOMPARE(s.getHrAlertLowBpm(), 0);
        QVERIFY(s.getAutoStartEnabled());
        QVERIFY(s.getAutoStopEnabled());
    }

    void aHealthSettingIsWrittenAndTheDaemonToldOnce()
    {
        LoggerSettings s;
        QSignalSpy changed(&s, &LoggerSettings::healthChanged);
        s.setRemindersEnabled(true);
        QTRY_COMPARE(daemon.healthCalls, 1);
        QCOMPARE(testenv::setting("reminders/enabled").toBool(), true);
        QCOMPARE(changed.count(), 1);
        s.setRemindersEnabled(true); // no change: nobody is told
        QTest::qWait(100);
        QCOMPARE(daemon.healthCalls, 1);
        s.setRemindersStartHour(30);
        s.setRemindersEndHour(0);
        s.setRemindersDays(0xff);
        s.setHrAlertHighEnabled(false);
        s.setHrAlertHighBpm(130);
        s.setHrAlertLowEnabled(false);
        s.setHrAlertLowBpm(45);
        s.setAutoStartEnabled(false);
        s.setAutoStopEnabled(false);
        QTRY_COMPARE(daemon.healthCalls, 10);
        QCOMPARE(testenv::setting("reminders/startHour").toInt(), 23); // clamped
        QCOMPARE(testenv::setting("reminders/endHour").toInt(), 1);
        QCOMPARE(testenv::setting("reminders/days").toInt(), 127);
        QCOMPARE(testenv::setting("hrAlerts/highEnabled").toBool(), false);
        QCOMPARE(testenv::setting("hrAlerts/highBpm").toInt(), 130);
        QCOMPARE(testenv::setting("hrAlerts/lowEnabled").toBool(), false);
        QCOMPARE(testenv::setting("hrAlerts/lowBpm").toInt(), 45);
        QCOMPARE(testenv::setting("autoExercise/start").toBool(), false);
        QCOMPARE(testenv::setting("autoExercise/stop").toBool(), false);
        QCOMPARE(s.getHrAlertHighBpm(), 130);
        QVERIFY(!s.getAutoStartEnabled());
    }

    void theExerciseAppTellsTheDaemonAboutAWorkout()
    {
        LoggerSettings s;
        s.setWorkoutActive(true);
        s.setWorkoutActive(false);
        QTRY_COMPARE(daemon.workouts.size(), 2);
        QCOMPARE(daemon.workouts, QList<bool>({true, false}));
    }
};

SENSORLOGD_TEST_MAIN(TestHealthLoaders)
#include "tst_healthloaders.moc"
