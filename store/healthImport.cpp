/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTextStream>

#include "../daemon/sensorPlugins/heartrateDb.h"
#include "healthImport.h"

namespace Health {

const char *const HealthImport::SLEEP_ALGO = "eos-steps-hr-1";

// the files of a folder named <yyyy-MM-dd><suffix>, in date order
static QStringList datedFiles(const QString &dir, const QString &suffix)
{
    QStringList out;
    for (const QString &f : QDir(dir).entryList({"*" + suffix}, QDir::Files, QDir::Name)) {
        if (f.size() == 10 + suffix.size() && QDate::fromString(f.left(10), Qt::ISODate).isValid())
            out << f;
    }
    return out;
}

HealthImport::HealthImport(HealthDb *db, const QString &root, DayTotalsFn dayTotals, const QDate &today)
    : m_db(db), m_root(root.endsWith('/') ? root : root + '/'), m_dayTotals(dayTotals), m_today(today)
{
    m_phase = Sleep;
    while (m_phase != Done && m_db && m_db->meta(phaseKey(m_phase)).toLongLong() == 1)
        m_phase = Phase(m_phase + 1);
    if (!m_db)
        m_phase = Done;
}

QString HealthImport::phaseKey(Phase phase) const
{
    switch (phase) {
    case Sleep: return QStringLiteral("import.sleep");
    case Exercise: return QStringLiteral("import.exercise");
    case Daily: return QStringLiteral("import.daily");
    default: return QString();
    }
}

bool HealthImport::done()
{
    return m_phase == Done;
}

void HealthImport::enterPhase(Phase phase)
{
    m_listed = true;
    m_index = 0;
    m_files.clear();
    if (phase == Sleep)
        m_files = datedFiles(m_root + "sleep", ".json");
    else if (phase == Exercise)
        m_files = datedFiles(m_root + "exercise", ".log");
    else if (phase == Daily) {
        // the first day with data: the oldest step day file or heart rate reading
        const QStringList steps = datedFiles(m_root + "stepCounter", ".log");
        QDate first = steps.isEmpty() ? QDate() : QDate::fromString(steps.first().left(10), Qt::ISODate);
        const qint64 firstHr = HeartrateDb::firstTime(m_root + "heartrate.db");
        if (firstHr > 0) {
            const QDate hrDay = QDateTime::fromMSecsSinceEpoch(firstHr).date();
            if (!first.isValid() || hrDay < first)
                first = hrDay;
        }
        m_day = first;
        m_lastDay = m_today.addDays(-1);
    }
}

bool HealthImport::step()
{
    // days and files that need nothing do not count as a unit (at most this many are looked at in one step)
    for (int look = 0; look < 64; look++) {
        if (m_phase == Done)
            return false;
        if (!m_listed)
            enterPhase(m_phase);
        if (m_phase == Sleep || m_phase == Exercise) {
            if (m_index < m_files.size()) {
                const QString file = m_files.at(m_index++);
                if (m_phase == Sleep)
                    m_counts.nights += importSleepFile(m_db, m_root, file, &m_counts.skipped);
                else
                    m_counts.workouts += importExerciseFile(m_db, m_root, file, &m_counts.skipped);
                return true;
            }
        } else if (m_phase == Daily) {
            if (m_day.isValid() && m_day <= m_lastDay) {
                const QDate d = m_day;
                m_day = m_day.addDays(1);
                DailyRecord row;
                if (HealthDb::readDaily(m_db->path(), d, &row) && row.closed)
                    continue;
                DailyTotals totals;
                if (!m_dayTotals || !m_dayTotals(d, &totals))
                    continue; // no data that day
                if (m_db->setDailyTotals(d, totals, QDateTime::currentMSecsSinceEpoch()))
                    m_counts.days++;
                return true;
            }
        }
        // the phase is through
        if (!m_db->setMeta(phaseKey(m_phase), 1))
            return false; // tried again at the next start
        m_phase = Phase(m_phase + 1);
        m_listed = false;
        if (m_phase == Done) {
            qInfo().nospace() << "health database: the one-time import is done: " << m_counts.nights << " nights, "
                              << m_counts.workouts << " workouts, " << m_counts.days << " days ("
                              << m_counts.skipped << " skipped)";
            return false;
        }
    }
    return true;
}

int HealthImport::runAll()
{
    int units = 0;
    while (step())
        units++;
    return units;
}

int HealthImport::importExerciseDays(const QDate &from, const QDate &to)
{
    int added = 0;
    for (QDate d = from; d.isValid() && d <= to; d = d.addDays(1)) {
        const QString name = d.toString("yyyy-MM-dd") + ".log";
        if (QFile::exists(m_root + "exercise/" + name))
            added += importExerciseFile(m_db, m_root, name);
    }
    return added;
}

int HealthImport::importSleepFile(HealthDb *db, const QString &root, const QString &fileName, int *skipped)
{
    QFile file(root + "sleep/" + fileName);
    QJsonDocument doc;
    if (file.open(QIODevice::ReadOnly))
        doc = QJsonDocument::fromJson(file.readAll());
    SleepNight night;
    if (doc.isObject())
        night = SleepNight::fromJson(doc.object());
    if (!night.wakeDate.isValid())
        night.wakeDate = QDate::fromString(fileName.left(10), Qt::ISODate);
    if (!night.valid || !night.wakeDate.isValid()) {
        if (skipped)
            (*skipped)++;
        return 0;
    }
    return db->storeNight(night, QString::fromLatin1(SLEEP_ALGO), "sleep/" + fileName, true) ? 1 : 0;
}

int HealthImport::importExerciseFile(HealthDb *db, const QString &root, const QString &fileName, int *skipped)
{
    QFile file(root + "exercise/" + fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return 0;
    int added = 0;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty())
            continue;
        const QStringList f = line.split(':');
        bool ok = f.size() == 8;
        qint64 v[8] = {0};
        for (int i = 0; ok && i < 8; i++) {
            if (i != 2)
                v[i] = f[i].toLongLong(&ok);
        }
        if (!ok || v[0] <= 0 || v[1] < 0) {
            if (skipped)
                (*skipped)++; // torn by a power loss, or not a workout line
            continue;
        }
        WorkoutRecord w;
        w.startMs = v[0] * 1000;
        w.activeS = (int)v[1];
        w.endMs = w.startMs + v[1] * 1000;
        w.type = f[2];
        w.distanceM = v[3];
        w.kcal = v[4];
        w.avgBpm = (int)v[5];
        w.maxBpm = (int)v[6];
        w.steps = (int)v[7];
        w.importedFrom = "exercise/" + fileName;
        if (db->importWorkout(w))
            added++;
    }
    return added;
}

} // namespace Health
