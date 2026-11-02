// The heart rate plugin (daemon/sensorPlugins/heartrateSensor.*) and its database (heartrateDb.*): every reading
// with a heart rate goes to <root>/heartrate.db, a batch in one transaction once it is over, live readings once a
// minute and at display off; readings that come late, across midnight, out of order or twice are kept once each;
// a reader reads while a batch is written (WAL); a damaged file is kept aside; readings wait in memory while the
// database cannot be written. The day files keep their one median record per interval for the Health app.
// The readings are synthetic. Their time stamps are CLOCK_BOOTTIME microseconds, as sensorfw's are.
#include <time.h>

#include <sqlite3.h>

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include "../common.h"
#include "../daemon/sensorPlugins/sampleTime.h"
#include "peek.h"
#include "testenv.h"

namespace {

quint64 bootUsAgo(qint64 ageMs)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (quint64)((qint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 - ageMs * 1000);
}

// the database's rows as "time:bpm:accuracy", in table (time) order; optionally only time >= from, < to
QStringList rows(qint64 from = 0, qint64 to = LLONG_MAX)
{
    QStringList out;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(QFile::encodeName(heartrateDbPath()).constData(), &db, SQLITE_OPEN_READWRITE, nullptr)
            != SQLITE_OK) {
        sqlite3_close(db);
        return out;
    }
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT time, bpm, accuracy FROM heartrate WHERE time >= ? AND time < ?", -1, &st,
                           nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, from);
        sqlite3_bind_int64(st, 2, to);
        while (sqlite3_step(st) == SQLITE_ROW)
            out << QString("%1:%2:%3").arg(sqlite3_column_int64(st, 0)).arg(sqlite3_column_int(st, 1))
                       .arg(sqlite3_column_int(st, 2));
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

QString row(qint64 time, int bpm, int accuracy) { return QString("%1:%2:%3").arg(time).arg(bpm).arg(accuracy); }

QString pragma(const char *name)
{
    sqlite3 *db = nullptr;
    QString value;
    if (sqlite3_open_v2(QFile::encodeName(heartrateDbPath()).constData(), &db, SQLITE_OPEN_READWRITE, nullptr)
            == SQLITE_OK) {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(db, (QByteArray("PRAGMA ") + name).constData(), -1, &st, nullptr) == SQLITE_OK
                && sqlite3_step(st) == SQLITE_ROW)
            value = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(st, 0)));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return value;
}

} // namespace

class TestHeartrate : public QObject
{
    Q_OBJECT

    QScopedPointer<QSettings> settings;
    QScopedPointer<HeartrateSensorPlugin> plugin;

    HeartrateSensorPlugin *make(int interval = 600000)
    {
        plugin.reset();
        settings.reset(new QSettings("asteroid", "sensorlogd"));
        plugin.reset(new HeartrateSensorPlugin(nullptr, settings.data(), interval));
        return plugin.data();
    }

    FakeSensorBackend *backend() { return FakeSensorBackend::last(QHrmSensor::sensorType); }

    // a reading taken ageMs ago, as the hub hands it over; its wall clock time
    qint64 push(int bpm, qint64 ageMs, int status = QHrmSensor::AccuracyHigh)
    {
        const quint64 ts = bootUsAgo(ageMs);
        backend()->pushHeartRate(bpm, status, ts);
        return readingWallMs(ts);
    }

private slots:
    void init() { testenv::clean(); }
    void cleanup()
    {
        plugin.reset();
        settings.reset();
    }

    void theDatabaseIsMadeInWalMode()
    {
        make();
        QVERIFY(QFile::exists(testenv::root() + "heartrate.db"));
        QCOMPARE(pragma("journal_mode"), QString("wal"));
        QCOMPARE(pragma("user_version"), QString("1"));
        QVERIFY(rows().isEmpty());
    }

    void aBatchIsOneTransactionOnceItIsOver()
    {
        make();
        QSignalSpy commits(plugin->m_commitTimer, &QTimer::timeout);
        // half an hour batched by the hub, one reading a second, handed over at once
        QList<qint64> times;
        for (int i = 1800; i > 0; i--)
            times << push(60 + i % 7, i * 1000LL);
        QVERIFY(rows().isEmpty()); // not before the batch is over
        QVERIFY(plugin->m_commitTimer->isActive());
        QVERIFY(plugin->m_commitTimer->interval() <= 1000); // within the hub's wake hold after a batch
        QTRY_COMPARE(rows().size(), 1800);
        QCOMPARE(commits.size(), 1);
        QCOMPARE(plugin->m_db.lastAdded(), 1800);
        QCOMPARE(rows().first(), row(times.first(), 60 + 1800 % 7, QHrmSensor::AccuracyHigh));
        QCOMPARE(rows().last(), row(times.last(), 60 + 1 % 7, QHrmSensor::AccuracyHigh));
        QVERIFY(plugin->m_pending.isEmpty());
    }

    void liveReadingsGoOnceAMinuteAndAtDisplayOff()
    {
        make();
        // live: a reading a second, each one when it is taken
        const qint64 t0 = push(70, 2000);
        push(71, 1000);
        QVERIFY(plugin->m_commitTimer->isActive());
        QCOMPARE(plugin->m_commitTimer->interval(), 60000);
        const int left = plugin->m_commitTimer->remainingTime();
        push(72, 0); // a live reading does not put the minute off
        QVERIFY(plugin->m_commitTimer->remainingTime() <= left);
        QVERIFY(rows().isEmpty());
        plugin->displayOff();
        QCOMPARE(rows().size(), 3);
        QCOMPARE(rows().first(), row(t0, 70, QHrmSensor::AccuracyHigh));
        QVERIFY(!plugin->m_commitTimer->isActive());
    }

    void theMinuteTimerWritesLiveReadings()
    {
        make();
        push(70, 0);
        plugin->m_commitTimer->setInterval(50); // instead of waiting a minute
        plugin->m_commitTimer->start();
        QTRY_COMPARE(rows().size(), 1);
    }

    void aRecordingAskedOverDBusAndARestartWriteToo()
    {
        make();
        push(64, 1000);
        plugin->triggerRecording();
        QCOMPARE(rows().size(), 1);
        push(65, 0);
        plugin.reset(); // a restart: nothing collected is lost
        QCOMPARE(rows().size(), 2);
    }

    void aFlushOverlapIsKeptOnce()
    {
        make();
        // the screen-on flush hands over what the hub batched; live readings follow and the flush's last
        // readings come again (and sensorfw replays the last reading when a sensor starts)
        QList<quint64> ts;
        for (int i = 0; i < 100; i++)
            ts << bootUsAgo((100 - i) * 1000LL);
        for (int i = 0; i < 80; i++)
            backend()->pushHeartRate(60 + i % 5, QHrmSensor::AccuracyMedium, ts[i]);
        plugin->displayOff();
        QCOMPARE(rows().size(), 80);
        for (int i = 50; i < 100; i++)
            backend()->pushHeartRate(60 + i % 5, QHrmSensor::AccuracyMedium, ts[i]);
        backend()->pushHeartRate(60 + 99 % 5, QHrmSensor::AccuracyMedium, ts[99]);
        plugin->displayOff();
        QCOMPARE(plugin->m_db.lastAdded(), 20);
        const QStringList all = rows();
        QCOMPARE(all.size(), 100);
        for (int i = 0; i < 100; i++)
            QCOMPARE(all[i], row(readingWallMs(ts[i]), 60 + i % 5, QHrmSensor::AccuracyMedium));
    }

    void aLateBatchAcrossMidnightIsKeptInTimeOrder()
    {
        make();
        const qint64 midnight = QDate::currentDate().startOfDay().toMSecsSinceEpoch();
        // today's live readings are written first; the batch from around midnight (screen off) comes later
        // (the plugin's way in for readings, keepReading(), takes their wall clock times directly here:
        // midnight can be longer ago than the test machine's CLOCK_BOOTTIME)
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (int i = 0; i < 5; i++)
            plugin->keepReading(now - 5000 + i * 1000, 80, QHrmSensor::AccuracyHigh);
        plugin->commitReadings();
        for (qint64 t = midnight - 30000; t < midnight + 30000; t += 1000)
            plugin->keepReading(t, 55 + (t / 1000) % 3, QHrmSensor::AccuracyMedium);
        plugin->commitReadings();
        QCOMPARE(rows().size(), 65);
        QCOMPARE(rows(0, midnight).size(), 30);            // yesterday's
        QCOMPARE(rows(midnight, midnight + 30000).size(), 30); // today's first half minute
        QCOMPARE(rows(0, midnight).first(), row(midnight - 30000, 55 + ((midnight - 30000) / 1000) % 3, 3));
        QCOMPARE(rows(0, midnight).last(), row(midnight - 1000, 55 + ((midnight - 1000) / 1000) % 3, 3));
        // in time order, whatever order they came in
        const QStringList all = rows();
        for (int i = 1; i < all.size(); i++)
            QVERIFY(all[i - 1].section(':', 0, 0).toLongLong() < all[i].section(':', 0, 0).toLongLong());
    }

    void readingsWithoutContactAreNotKept()
    {
        make();
        push(0, 3000, QHrmSensor::NoContact);
        push(75, 2000, QHrmSensor::NoContact);
        const qint64 unreliable = push(150, 1000, QHrmSensor::Unreliable);
        const qint64 low = push(90, 500, QHrmSensor::AccuracyLow);
        plugin->displayOff();
        // every reading with a heart rate, with its accuracy (the records use medium and up)
        QCOMPARE(rows(), QStringList({row(unreliable, 150, 1), row(low, 90, 2)}));
    }

    void aReaderReadsWhileABatchIsWritten()
    {
        make();
        push(70, 2000);
        plugin->displayOff();
        // a reader (btsyncd's health sync) in the middle of a read transaction
        sqlite3 *reader = nullptr;
        QCOMPARE(sqlite3_open_v2(QFile::encodeName(heartrateDbPath()).constData(), &reader, SQLITE_OPEN_READWRITE,
                                 nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_exec(reader, "BEGIN", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_stmt *count = nullptr;
        QCOMPARE(sqlite3_prepare_v2(reader, "SELECT count(*) FROM heartrate", -1, &count, nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_step(count), SQLITE_ROW);
        QCOMPARE(sqlite3_column_int(count, 0), 1);
        sqlite3_reset(count);
        for (int i = 600; i > 0; i--)
            push(70, i * 1000LL + 2000);
        QElapsedTimer took;
        took.start();
        plugin->displayOff(); // not held up by the reader
        QVERIFY(took.elapsed() < 2000);
        QVERIFY(plugin->m_pending.isEmpty());
        QCOMPARE(sqlite3_step(count), SQLITE_ROW);
        QCOMPARE(sqlite3_column_int(count, 0), 1); // the reader's snapshot
        sqlite3_reset(count);
        QCOMPARE(sqlite3_exec(reader, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_step(count), SQLITE_ROW);
        QCOMPARE(sqlite3_column_int(count, 0), 601);
        sqlite3_finalize(count);
        sqlite3_close(reader);
    }

    void readingsWaitWhileTheDatabaseCannotBeWritten()
    {
        // the data folder cannot be written: no database yet
        QFile::setPermissions(testenv::root(), QFile::ReadOwner | QFile::ExeOwner);
        make();
        push(70, 3000);
        push(71, 2000);
        plugin->displayOff();
        QCOMPARE(plugin->m_pending.size(), 2);
        QFile::setPermissions(testenv::root(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        push(72, 1000);
        plugin->displayOff();
        QVERIFY(plugin->m_pending.isEmpty());
        QCOMPARE(rows().size(), 3);
    }

    void aDamagedDatabaseIsKeptAside()
    {
        QDir().mkpath(testenv::root());
        QFile junk(testenv::root() + "heartrate.db");
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write(QByteArray(8192, 'x'));
        junk.close();
        make();
        push(66, 1000);
        plugin->displayOff();
        QCOMPARE(rows().size(), 1);
        const QStringList aside = QDir(testenv::root()).entryList({"heartrate.db.broken-*"}, QDir::Files);
        QCOMPARE(aside.size(), 1);
        QCOMPARE(QFileInfo(testenv::root() + aside.first()).size(), 8192);
    }

    void theIntervalRecordsStayAsTheyWere()
    {
        // the Health app's day files: one median record per interval at rest, dated by its reading
        make(600000);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 start = (now / 600000 - 2) * 600000; // an interval that is over
        const qint64 base = now - start;
        QList<qint64> times;
        const int bpms[] = {62, 60, 61, 64, 63};
        for (int i = 0; i < 5; i++)
            times << push(bpms[i], base - i * 60000 - 1000);
        push(58, base - 5 * 60000, QHrmSensor::AccuracyLow); // below the accuracy the records use
        plugin->batchIdle();
        // the median of 60 61 62 63 64, dated when it was taken
        QCOMPARE(testenv::dayValues("heartrateMonitor", QDateTime::fromMSecsSinceEpoch(times[0]).date()),
                 QStringList({"62"}));
        QCOMPARE(testenv::dayTimes("heartrateMonitor", QDateTime::fromMSecsSinceEpoch(times[0]).date()),
                 QList<qint64>({times[0] / 1000}));
        // and every reading in the database, the low one too
        QCOMPARE(rows().size(), 6);
    }

    void aSecondReadingOfTheSameTimeStampHasTheSameTime()
    {
        const quint64 ts = bootUsAgo(1234);
        const qint64 first = readingWallMs(ts);
        for (int i = 0; i < 1000; i++)
            QCOMPARE(readingWallMs(ts), first);
    }
};

SENSORLOGD_TEST_MAIN(TestHeartrate)
#include "tst_heartrate.moc"
