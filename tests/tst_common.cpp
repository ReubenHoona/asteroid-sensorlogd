// The day-file format shared by the daemon and the QML plugin (common.cpp) and the daemon's day-file helpers
// (logger.cpp: fileGetPrevRecord, dayFileExists): paths from the loggerRootPath setting, appending
// "<seconds>:<value>" records to the file of the record's own date, reading them back quietly and safely (missing
// day, unreadable file, empty or torn lines, decimals), and the last record of a day.
#include <QRegularExpression>
#include <QTest>

#include "../common.h"
#include "../daemon/logger.h"
#include "testenv.h"

class TestCommon : public QObject
{
    Q_OBJECT
private slots:
    void init() { testenv::clean(); }

    void pathsDefaultToHome()
    {
        QSettings settings("asteroid", "sensorlogd");
        settings.remove("loggerRootPath");
        settings.sync();
        const QString home = qEnvironmentVariable("HOME");
        QCOMPARE(fileNameForDate(QDate(2026, 1, 2), "stepCounter"),
                 home + "/.asteroid-sensorlogd/stepCounter/2026-01-02.log");
        QCOMPARE(sensorDirPath("heartrateMonitor"), home + "/.asteroid-sensorlogd/heartrateMonitor/");
    }

    void pathsFollowTheRootSetting()
    {
        QCOMPARE(fileNameForDate(QDate(2026, 10, 28), "barometer-raw"), testenv::root() + "barometer-raw/2026-10-28.log");
        QCOMPARE(sensorDirPath("weight"), testenv::root() + "weight/");
    }

    void setupFilePathMakesTheFolder()
    {
        QVERIFY(!QDir(testenv::root() + "stepCounter").exists());
        setupFilePath("stepCounter");
        QVERIFY(QDir(testenv::root() + "stepCounter").exists());
        setupFilePath("stepCounter"); // again: no harm
        QVERIFY(QDir(testenv::root() + "stepCounter").exists());
    }

    void addRecordAppends()
    {
        setupFilePath("stepCounter");
        const QDateTime t1(QDate::currentDate(), QTime(8, 0, 0));
        const QDateTime t2(QDate::currentDate(), QTime(8, 10, 0));
        fileAddRecord("stepCounter", "120", t1);
        fileAddRecord("stepCounter", "480", t2);
        QCOMPARE(testenv::dayLines("stepCounter"),
                 QStringList({QString::number(t1.toSecsSinceEpoch()) + ":120",
                              QString::number(t2.toSecsSinceEpoch()) + ":480"}));
    }

    void addRecordDefaultsToNow()
    {
        setupFilePath("weight");
        const qint64 before = QDateTime::currentSecsSinceEpoch();
        fileAddRecord("weight", "72.5");
        const qint64 after = QDateTime::currentSecsSinceEpoch();
        const QList<qint64> times = testenv::dayTimes("weight");
        QCOMPARE(times.size(), 1);
        QVERIFY(times[0] >= before && times[0] <= after);
        QCOMPARE(testenv::dayValues("weight"), QStringList({"72.5"}));
    }

    void addRecordGoesToTheRecordsOwnDay()
    {
        setupFilePath("stepCounter");
        const QDate yesterday = QDate::currentDate().addDays(-1);
        fileAddRecord("stepCounter", "9000", QDateTime(yesterday, QTime(23, 59, 59)));
        QCOMPARE(testenv::dayValues("stepCounter", yesterday), QStringList({"9000"}));
        QVERIFY(!QFile::exists(testenv::dayFile("stepCounter")));
    }

    void addRecordWithoutFolderWarns()
    {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^cannot write .*nosuchsensor"));
        fileAddRecord("nosuchsensor", "1");
        QVERIFY(!QFile::exists(testenv::dayFile("nosuchsensor")));
    }

    void readMissingDayIsQuiet()
    {
        QTest::failOnWarning(QRegularExpression(".*"));
        QVERIFY(readDayRecords(testenv::dayFile("stepCounter")).isEmpty());
    }

    void readUnreadableDayWarns()
    {
        // a folder where the day file should be: it exists but cannot be opened as a file
        QDir().mkpath(testenv::dayFile("stepCounter"));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^cannot read "));
        QVERIFY(readDayRecords(testenv::dayFile("stepCounter")).isEmpty());
    }

    void readRecords_data()
    {
        QTest::addColumn<QByteArray>("content");
        QTest::addColumn<QList<QPointF>>("expected");
        QTest::newRow("empty file") << QByteArray() << QList<QPointF>();
        QTest::newRow("integers") << QByteArray("100:5\n200:7\n") << QList<QPointF>({{100, 5}, {200, 7}});
        QTest::newRow("decimals and negatives")
            << QByteArray("100:72.5\n200:-3\n") << QList<QPointF>({{100, 72.5}, {200, -3}});
        QTest::newRow("no final newline") << QByteArray("100:5\n200:7") << QList<QPointF>({{100, 5}, {200, 7}});
        QTest::newRow("empty lines") << QByteArray("\n100:5\n\n\n200:7\n\n") << QList<QPointF>({{100, 5}, {200, 7}});
        QTest::newRow("torn line without separator")
            << QByteArray("100:5\n17000\n200:7\n") << QList<QPointF>({{100, 5}, {200, 7}});
        QTest::newRow("no time") << QByteArray(":5\n200:7\n") << QList<QPointF>({{200, 7}});
        QTest::newRow("bad time") << QByteArray("abc:5\n200:7\n") << QList<QPointF>({{200, 7}});
        QTest::newRow("bad value") << QByteArray("100:x\n200:7\n") << QList<QPointF>({{200, 7}});
        QTest::newRow("zero-filled tail after a power loss")
            << QByteArray("100:5\n", 6) + QByteArray(16, '\0') << QList<QPointF>({{100, 5}});
        QTest::newRow("record written after a zero-filled tail")
            << QByteArray("100:5\n") + QByteArray(8, '\0') + QByteArray("200:7\n") << QList<QPointF>({{100, 5}});
    }

    void readRecords()
    {
        QFETCH(QByteArray, content);
        QFETCH(QList<QPointF>, expected);
        testenv::writeDay("stepCounter", QDate::currentDate(), content);
        QCOMPARE(readDayRecords(testenv::dayFile("stepCounter")), expected);
    }

    void dayFileExistsFollowsTheDate()
    {
        QVERIFY(!dayFileExists("stepCounter"));
        testenv::writeDay("stepCounter", QDate::currentDate(), "100:5\n");
        QVERIFY(dayFileExists("stepCounter"));
        QVERIFY(!dayFileExists("stepCounter", QDateTime::currentDateTime().addDays(-1)));
        QVERIFY(!dayFileExists("heartrateMonitor"));
    }

    void prevRecordOfMissingDay()
    {
        QCOMPARE(fileGetPrevRecord("stepCounter"), QStringList({"0", "0"}));
    }

    void prevRecordIsTheLastLine()
    {
        testenv::writeDay("stepCounter", QDate::currentDate(), "100:5\n200:70\n300:900\n\n\n");
        QCOMPARE(fileGetPrevRecord("stepCounter"), QStringList({"300", "900"}));
    }

    void prevRecordOfAnotherDay()
    {
        const QDate yesterday = QDate::currentDate().addDays(-1);
        testenv::writeDay("stepCounter", yesterday, "100:4321\n");
        QCOMPARE(fileGetPrevRecord("stepCounter", QDateTime(yesterday, QTime(12, 0))), QStringList({"100", "4321"}));
        QCOMPARE(fileGetPrevRecord("stepCounter"), QStringList({"0", "0"}));
    }

    void prevRecordOfEmptyDay()
    {
        testenv::writeDay("stepCounter", QDate::currentDate(), "");
        QCOMPARE(fileGetPrevRecord("stepCounter"), QStringList({"0", "0"}));
    }

    void prevRecordAfterTornLine()
    {
        // callers index [1]: a torn last line must not give a one-element list (it crashed the step counter)
        testenv::writeDay("stepCounter", QDate::currentDate(), "100:5\n1700");
        const QStringList record = fileGetPrevRecord("stepCounter");
        QVERIFY(record.size() >= 2);
    }

    void prevRecordSkipsAZeroFilledTail()
    {
        // ext4 can leave a zero-filled tail after a power loss mid-write. readDayRecords() skips it, but
        // fileGetPrevRecord() takes the last non-empty line, gives {"0", "0"} and the step counter's first reading
        // after the reboot then counts the day from zero again (see tst_stepcounter freshBootAfterPowerLoss).
        testenv::writeDay("stepCounter", QDate::currentDate(), QByteArray("100:5\n200:4800\n") + QByteArray(16, '\0'));
        QEXPECT_FAIL("", "BUG fileGetPrevRecord: a zero-filled / torn last line hides the day's last record "
                         "(patches/sensorlogd/0001-fileGetPrevRecord-last-valid-record.patch)", Continue);
        QCOMPARE(fileGetPrevRecord("stepCounter"), QStringList({"200", "4800"}));
    }
};

SENSORLOGD_TEST_MAIN(TestCommon)
#include "tst_common.moc"
