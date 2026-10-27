// The QML plugin's data loaders (qmlplugin/*DataLoader.cpp), which the health app reads the day files through:
// a day's records (missing day: none), ranges given either way round across missing days, the step total of a
// day (its last record, never negative) and of today (live counter minus the daemon's offset, never below what was
// logged), weights written from the app, the file watcher's dataChanged, and the recording request to the daemon
// over D-Bus (a stand-in logger object on the session bus of dbus-run-session).
#include <QDBusConnection>
#include <QSignalSpy>
#include <QTest>

#include <QtSensors/QStepCounterSensor>

#include "../common.h"
#include "../qmlplugin/barometerDataLoader.h"
#include "../qmlplugin/hrDataLoader.h"
#include "../qmlplugin/stepsDataLoader.h"
#include "../qmlplugin/weightDataLoader.h"
#include "testenv.h"

class FakeDaemon : public QObject
{
    Q_OBJECT
public:
    int recordings = 0;
public slots:
    void triggerRecording() { recordings++; }
};

namespace {
QList<QPointF> points(const QVariant &v) { return v.value<QList<QPointF>>(); }

qint64 at(QDate day, int h, int m = 0) { return QDateTime(day, QTime(h, m)).toSecsSinceEpoch(); }

QByteArray line(qint64 t, const char *value) { return QByteArray::number(t) + ":" + value + "\n"; }

// the same checks for the three loaders that read one sensor's day files
template <class Loader> void checkReader(const QString &prefix, FakeDaemon &daemon)
{
    const QDate today = QDate::currentDate();
    const QDate d1 = today.addDays(-3), d3 = today.addDays(-1);
    testenv::writeDay(prefix, d1, line(at(d1, 9), "61") + line(at(d1, 10), "62.5"));
    testenv::writeDay(prefix, d3, line(at(d3, 9), "63"));
    testenv::writeDay(prefix, today, line(at(today, 0, 5), "64"));

    Loader loader;
    QCOMPARE(points(loader.getDataForDate(d1)), QList<QPointF>({{double(at(d1, 9)), 61}, {double(at(d1, 10)), 62.5}}));
    QCOMPARE(points(loader.getTodayData()), QList<QPointF>({{double(at(today, 0, 5)), 64}}));
    QVERIFY(points(loader.getDataForDate(today.addDays(-2))).isEmpty());
    const QList<QPointF> all = {{double(at(d1, 9)), 61}, {double(at(d1, 10)), 62.5}, {double(at(d3, 9)), 63},
                                {double(at(today, 0, 5)), 64}};
    QCOMPARE(points(loader.getDataFromTo(d1, today)), all);
    QCOMPARE(points(loader.getDataFromTo(today, d1)), all); // either way round
    QCOMPARE(points(loader.getDataFromTo(today.addDays(-2), today.addDays(-2))), QList<QPointF>());

    // the files read are watched: a new record is a dataChanged
    QSignalSpy changed(&loader, SIGNAL(dataChanged()));
    fileAddRecord(prefix, "65");
    QTRY_VERIFY(changed.count() >= 1);

    const int before = daemon.recordings;
    loader.triggerDaemonRecording();
    QCOMPARE(daemon.recordings, before + 1);
}
}

class TestDataLoaders : public QObject
{
    Q_OBJECT
    FakeDaemon daemon;

private slots:
    void initTestCase()
    {
        QVERIFY2(QDBusConnection::sessionBus().isConnected(), "run the data loader test under dbus-run-session");
        testenv::clean();
        {
            // no daemon on the bus: the loaders still work, a recording request goes nowhere
            HrDataLoader loader;
            loader.triggerDaemonRecording();
            QVERIFY(points(loader.getTodayData()).isEmpty());
        }
        QVERIFY(QDBusConnection::sessionBus().registerObject("/org/asteroid/sensorlogd/logger", &daemon,
                                                             QDBusConnection::ExportAllSlots));
        QVERIFY(QDBusConnection::sessionBus().registerService("org.asteroid.sensorlogd.logger"));
    }

    void init()
    {
        testenv::clean();
        for (const char *prefix : {"stepCounter", "heartrateMonitor", "barometer-compensated", "weight"})
            setupFilePath(prefix);
    }

    void heartRate() { checkReader<HrDataLoader>("heartrateMonitor", daemon); }
    void barometer() { checkReader<BarometerDataLoader>("barometer-compensated", daemon); }
    void steps() { checkReader<StepsDataLoader>("stepCounter", daemon); }

    void newDayFileIsADataChange()
    {
        HrDataLoader loader;
        QSignalSpy changed(&loader, SIGNAL(dataChanged()));
        fileAddRecord("heartrateMonitor", "70"); // creates today's file in the watched folder
        QTRY_VERIFY(changed.count() >= 1);
    }

    void stepTotalOfADay()
    {
        const QDate day = QDate::currentDate().addDays(-1);
        StepsDataLoader loader;
        QCOMPARE(loader.getTotalForDate(day), 0); // nothing logged
        testenv::writeDay("stepCounter", day, line(at(day, 9), "100") + line(at(day, 23, 59), "4200"));
        QCOMPARE(loader.getTotalForDate(day), 4200);
        testenv::writeDay("stepCounter", day, line(at(day, 9), "-30")); // a bad offset once logged
        QCOMPARE(loader.getTotalForDate(day), 0);
    }

    void todayTotal()
    {
        const QDate today = QDate::currentDate();
        testenv::writeDay("stepCounter", today, line(at(today, 0, 10), "800"));
        testenv::setSetting("StepCounterPrivate/stepsOffset", 500);
        StepsDataLoader loader;
        QSignalSpy totalChanged(&loader, SIGNAL(todayTotalChanged()));
        // no reading from the counter yet: what was logged
        QCOMPARE(loader.getTodayTotal(), 800);
        FakeSensorBackend *counter = FakeSensorBackend::last(QStepCounterSensor::sensorType);
        QVERIFY(counter && counter->running());
        counter->pushSteps(1500);
        QCOMPARE(totalChanged.count(), 1);
        QCOMPARE(loader.getTodayTotal(), 1000); // live: 1500 - 500
        // a stale offset (the counter started again) must not show less than was logged
        testenv::writeDay("stepCounter", today, line(at(today, 0, 10), "1200"));
        QCOMPARE(loader.getTodayTotal(), 1200);
    }

    void weight()
    {
        QDir(testenv::root() + "weight").removeRecursively();
        WeightDataLoader loader;
        QVERIFY(QDir(testenv::root() + "weight").exists()); // made by the loader (no sensor plugin for weight)
        loader.addDataPoint(72.5);
        const QList<QPointF> today = points(loader.getTodayData());
        QCOMPARE(today.size(), 1);
        QCOMPARE(today[0].y(), 72.5);
        QVERIFY(qAbs(today[0].x() - QDateTime::currentSecsSinceEpoch()) < 5);
        const QDate yesterday = QDate::currentDate().addDays(-1);
        testenv::writeDay("weight", yesterday, line(at(yesterday, 8), "73"));
        QCOMPARE(points(loader.getDataForDate(yesterday)), QList<QPointF>({{double(at(yesterday, 8)), 73}}));
        QCOMPARE(points(loader.getDataFromTo(QDate::currentDate(), yesterday)).size(), 2);
        const int before = daemon.recordings;
        loader.triggerDaemonRecording();
        QCOMPARE(daemon.recordings, before + 1);
    }
};

SENSORLOGD_TEST_MAIN(TestDataLoaders)
#include "tst_dataloaders.moc"
