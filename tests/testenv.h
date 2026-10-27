// Shared set-up for the sensorlogd tests: every test program runs with HOME and XDG_CONFIG_HOME in a temporary
// folder (QSettings("asteroid", "sensorlogd") and the day files never touch the real ones), loggerRootPath pointing
// at <tmp>/data/, the fake QtSensors backend installed, and helpers to write and read day files
// ("<seconds since the epoch>:<value>" lines in <root>/<sensor>/<yyyy-MM-dd>.log).
#ifndef SENSORLOGD_TESTENV_H
#define SENSORLOGD_TESTENV_H

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QPair>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTest>

#include "fakes/fakesensorbackend.h"

namespace testenv {

inline QTemporaryDir *&tmpDir()
{
    static QTemporaryDir *dir = nullptr;
    return dir;
}

// before QCoreApplication: the temporary home for the whole program
inline void prepareProcess()
{
    tmpDir() = new QTemporaryDir();
    const QByteArray home = tmpDir()->path().toLocal8Bit();
    qputenv("HOME", home);
    qputenv("XDG_CONFIG_HOME", home + "/config");
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_SENSORS_LOAD_PLUGINS", "0"); // only the fake backend, never a QtSensors plugin of the machine
}

inline QString root() { return tmpDir()->path() + "/data/"; }

// no settings, no day files; loggerRootPath set again (call it in init())
inline void clean()
{
    QSettings settings("asteroid", "sensorlogd");
    settings.clear();
    settings.setValue("loggerRootPath", root());
    settings.sync();
    QDir(root()).removeRecursively();
    QDir().mkpath(root());
    FakeSensorBackend::reset();
}

inline QString dayFile(const QString &prefix, QDate date = QDate::currentDate())
{
    return root() + prefix + "/" + date.toString("yyyy-MM-dd.log");
}

inline void writeDay(const QString &prefix, QDate date, const QByteArray &content)
{
    QDir().mkpath(root() + prefix);
    QFile f(dayFile(prefix, date));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        qFatal("cannot write the test day file");
    f.write(content);
}

inline QStringList dayLines(const QString &prefix, QDate date = QDate::currentDate())
{
    QFile f(dayFile(prefix, date));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QStringList lines;
    QTextStream in(&f);
    while (!in.atEnd())
        lines << in.readLine();
    return lines;
}

// the values of a day file's records, in order
inline QStringList dayValues(const QString &prefix, QDate date = QDate::currentDate())
{
    QStringList values;
    for (const QString &line : dayLines(prefix, date))
        values << line.section(':', 1);
    return values;
}

// the times (seconds since the epoch) of a day file's records, in order
inline QList<qint64> dayTimes(const QString &prefix, QDate date = QDate::currentDate())
{
    QList<qint64> times;
    for (const QString &line : dayLines(prefix, date))
        times << line.section(':', 0, 0).toLongLong();
    return times;
}

inline QVariant setting(const QString &key)
{
    QSettings settings("asteroid", "sensorlogd");
    settings.sync();
    return settings.value(key);
}

inline void setSetting(const QString &key, const QVariant &value)
{
    QSettings settings("asteroid", "sensorlogd");
    settings.setValue(key, value);
    settings.sync();
}

} // namespace testenv

// QTEST_GUILESS_MAIN with the temporary home and the organisation / application names sensorlogd sets
#define SENSORLOGD_TEST_MAIN(TestClass)                                                                            \
    int main(int argc, char **argv)                                                                                \
    {                                                                                                              \
        testenv::prepareProcess();                                                                                 \
        QCoreApplication app(argc, argv);                                                                          \
        QCoreApplication::setOrganizationName("asteroid");                                                         \
        QCoreApplication::setOrganizationDomain("asteroidos.org");                                                 \
        QCoreApplication::setApplicationName("sensorlogd");                                                        \
        FakeSensorBackend::install();                                                                              \
        TestClass tc;                                                                                              \
        const int rc = QTest::qExec(&tc, argc, argv);                                                              \
        delete testenv::tmpDir();                                                                                  \
        return rc;                                                                                                 \
    }

#endif
