// The daemon's D-Bus methods for health.db (daemon/logger.*: BeginWorkout, AddTrackPoints, AddLap, SetPaused,
// EndWorkout, DeleteWorkout, HealthSeq and the signal HealthChanged), called over the session bus as the Exercise
// app will (WO-02), and read back through the QML plugin's WorkoutLoader. The Logger runs without sensorfwd (it
// waits for it), so only its health database is up. The workouts and fixes are invented.
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QSignalSpy>
#include <QTest>

#include "../common.h"
#include "../daemon/logger.h"
#include "../qmlplugin/workoutLoader.h"
#include "../store/healthDb.h"
#include "testenv.h"

using namespace Health;

namespace {

const QDate SAT(2026, 11, 14);

qint64 ms(const QDate &d, int h, int m = 0, int s = 0) { return QDateTime(d, QTime(h, m, s)).toMSecsSinceEpoch(); }

}

// what a client of HealthChanged sees
class Listener : public QObject
{
    Q_OBJECT
public:
    QList<qlonglong> seqs;
public slots:
    void changed(qlonglong seq) { seqs << seq; }
};

class TestApi : public QObject
{
    Q_OBJECT

    Logger *logger = nullptr;
    QDBusInterface *iface = nullptr;
    Listener listener;

private slots:
    void initTestCase()
    {
        QVERIFY2(QDBusConnection::sessionBus().isConnected(), "run under dbus-run-session");
        testenv::clean();
        logger = new Logger();
        iface = new QDBusInterface("org.asteroid.sensorlogd.logger", "/org/asteroid/sensorlogd/logger",
                                   "org.asteroid.sensorlogd.logger", QDBusConnection::sessionBus());
        QVERIFY(iface->isValid());
        QVERIFY(connectHealthChanged(&listener, SLOT(changed(qlonglong))));
    }

    void cleanupTestCase()
    {
        delete iface;
        delete logger;
    }

    void aWorkoutOverTheBus()
    {
        WorkoutLoader loader;
        QSignalSpy loaderSpy(&loader, &WorkoutLoader::dataChanged);
        QSignalSpy spy(logger, &Logger::HealthChanged);
        QDBusReply<qlonglong> seq0 = iface->call("HealthSeq");
        QVERIFY(seq0.isValid());
        QCOMPARE(seq0.value(), 0LL);

        QDBusReply<qlonglong> id = iface->call("BeginWorkout", (qlonglong)ms(SAT, 9, 0), QString("run"), QString("manual"), true);
        QVERIFY2(id.isValid(), qPrintable(id.error().message()));
        QVERIFY(id.value() > 0);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last().at(0).toLongLong(), 1LL);
        QCOMPARE(logger->HealthSeq(), 1LL);
        // a second one with the same start is refused
        QDBusReply<qlonglong> dup = iface->call("BeginWorkout", (qlonglong)ms(SAT, 9, 0), QString("walk"), QString(""), false);
        QCOMPARE(dup.value(), 0LL);

        // fixes in a batch: six numbers each (one with a latitude that is no position is left out)
        const QList<qlonglong> fixes = {ms(SAT, 9, 0, 1), 515000000, -1200000, 120, 40, 250,
                                        ms(SAT, 9, 0, 2), 515000090, -1200100, 121, 40, 260,
                                        ms(SAT, 9, 0, 3), 999999999, 0, 0, 0, 0};
        QDBusReply<bool> added = iface->call("AddTrackPoints", id.value(), QVariant::fromValue(fixes));
        QVERIFY2(added.isValid(), qPrintable(added.error().message()));
        QVERIFY(added.value());
        QDBusReply<bool> odd = iface->call("AddTrackPoints", id.value(), QVariant::fromValue(QList<qlonglong>{1, 2, 3}));
        QVERIFY(!odd.value());
        QDBusReply<bool> lap = iface->call("AddLap", id.value(), 1, (qlonglong)ms(SAT, 9, 0), (qlonglong)ms(SAT, 9, 5, 30),
                                           1000.0, 148, QString("split-km"));
        QVERIFY(lap.value());
        QDBusReply<bool> paused = iface->call("SetPaused", id.value(), true, (qlonglong)ms(SAT, 9, 20), true);
        QVERIFY(paused.value());
        QDBusReply<bool> resumed = iface->call("SetPaused", id.value(), false, (qlonglong)ms(SAT, 9, 22), true);
        QVERIFY(resumed.value());

        QVariantMap summary{{"activeS", 2580}, {"distanceM", 7020.5}, {"kcal", 488.0}, {"steps", 7900},
                            {"avgBpm", 149}, {"maxBpm", 171}, {"moderateS", 400}, {"vigorousS", 1900},
                            {"peakS", 280}, {"azm", 80}, {"elevGainM", 18.0}};
        QDBusReply<bool> ended = iface->call("EndWorkout", id.value(), (qlonglong)ms(SAT, 9, 45), summary);
        QVERIFY2(ended.isValid(), qPrintable(ended.error().message()));
        QVERIFY(ended.value());
        QDBusReply<bool> again = iface->call("EndWorkout", id.value(), (qlonglong)ms(SAT, 9, 50), summary);
        QVERIFY(!again.value());

        // every write told its counter, in order; the bus clients too
        QCOMPARE(spy.count(), 6);
        for (int i = 1; i < spy.count(); i++)
            QVERIFY(spy[i].at(0).toLongLong() > spy[i - 1].at(0).toLongLong());
        QTRY_COMPARE(listener.seqs.size(), 6);
        QCOMPARE(listener.seqs.last(), spy.last().at(0).toLongLong());
        QTRY_COMPARE(loaderSpy.count(), 6);

        // the QML plugin reads it back
        const QVariantMap w = loader.workout(id.value());
        QCOMPARE(w["type"].toString(), QString("run"));
        QCOMPARE(w["startMs"].toLongLong(), ms(SAT, 9, 0));
        QCOMPARE(w["endMs"].toLongLong(), ms(SAT, 9, 45));
        QCOMPARE(w["activeS"].toInt(), 2580);
        QCOMPARE(w["distanceM"].toDouble(), 7020.5);
        QCOMPARE(w["azm"].toInt(), 80);
        QVERIFY(w["gps"].toBool());
        QVERIFY(w["ended"].toBool());
        QVERIFY(!w["imported"].toBool());
        QCOMPARE(w["laps"].toList().size(), 1);
        QCOMPARE(w["laps"].toList()[0].toMap()["avgBpm"].toInt(), 148);
        QCOMPARE(w["pauses"].toList().size(), 1);
        QVERIFY(w["pauses"].toList()[0].toMap()["auto"].toBool());
        const QVariantList track = loader.track(id.value());
        QCOMPARE(track.size(), 2);
        QCOMPARE(track[0].toMap()["lat"].toDouble(), 51.5);
        QCOMPARE(track[1].toMap()["speed"].toDouble(), 2.6);
        QCOMPARE(loader.workouts(SAT, SAT).size(), 1);
        QCOMPARE(loader.workouts(SAT.addDays(1), SAT.addDays(2)).size(), 0);
        QCOMPARE(loader.last()["id"].toLongLong(), id.value());
        QCOMPARE(loader.recent(5).size(), 1);
        QCOMPARE(loader.seq(), logger->HealthSeq());

        // a workout that is on is not the last activity
        QDBusReply<qlonglong> on = iface->call("BeginWorkout", (qlonglong)ms(SAT, 17, 0), QString("walk"), QString("auto"), false);
        QVERIFY(on.value() > 0);
        QCOMPARE(loader.recent(5).size(), 2);
        QCOMPARE(loader.last()["id"].toLongLong(), id.value());

        // deleted: gone for the readers, a tombstone for a sync
        QDBusReply<bool> del = iface->call("DeleteWorkout", id.value());
        QVERIFY(del.value());
        QVERIFY(loader.workout(id.value()).isEmpty());
        QCOMPARE(loader.track(id.value()).size(), 0);
        QCOMPARE(loader.recent(5).size(), 1);
        QVERIFY(loader.last().isEmpty());
        const ChangePage page = HealthDb::changes(healthDbPath(), "workout", 0);
        QCOMPARE(page.rows.size(), 2);
        QCOMPARE(page.rows.last()["id"].toLongLong(), id.value());
        QCOMPARE(page.rows.last()["deleted"].toLongLong(), 1LL);

        // the counts for a check on the watch: numbers only
        QDBusReply<QVariantMap> counts = iface->call("HealthCounts");
        QVERIFY2(counts.isValid(), qPrintable(counts.error().message()));
        const QVariantMap c = counts.value();
        QCOMPARE(c["schema"].toLongLong(), 1LL);
        QCOMPARE(c["seq"].toLongLong(), logger->HealthSeq());
        QCOMPARE(c["workouts"].toLongLong(), 1LL);
        QCOMPARE(c["workoutsDeleted"].toLongLong(), 1LL);
        QCOMPARE(c["workoutsImported"].toLongLong(), 0LL);
        QCOMPARE(c["trackPoints"].toLongLong(), 0LL); // went with the deleted workout
        QCOMPARE(c["nights"].toLongLong(), 0LL);
        QCOMPARE(c["days"].toLongLong(), 0LL);
        for (auto it = c.constBegin(); it != c.constEnd(); ++it)
            QCOMPARE(it.value().typeId(), (int)QMetaType::LongLong);
    }

    void badCallsAreRefusedAndChangeNothing()
    {
        QSignalSpy spy(logger, &Logger::HealthChanged);
        const qlonglong before = logger->HealthSeq();
        QCOMPARE(QDBusReply<qlonglong>(iface->call("BeginWorkout", 0LL, QString("walk"), QString(), false)).value(), 0LL);
        QCOMPARE(QDBusReply<qlonglong>(iface->call("BeginWorkout", (qlonglong)ms(SAT, 20), QString("../x"), QString(), false)).value(), 0LL);
        QVERIFY(!QDBusReply<bool>(iface->call("AddLap", 12345LL, 1, 1LL, 2LL, 0.0, 0, QString("manual"))).value());
        QVERIFY(!QDBusReply<bool>(iface->call("SetPaused", 12345LL, true, 1LL, false)).value());
        QVERIFY(!QDBusReply<bool>(iface->call("EndWorkout", 12345LL, 2LL, QVariantMap())).value());
        QVERIFY(!QDBusReply<bool>(iface->call("DeleteWorkout", 12345LL)).value());
        QCOMPARE(logger->HealthSeq(), before);
        QCOMPARE(spy.count(), 0);
    }
};

SENSORLOGD_TEST_MAIN(TestApi)
#include "tst_api.moc"
