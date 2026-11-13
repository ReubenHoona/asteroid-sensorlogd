/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cmath>

#include <QDateTime>
#include <QJsonArray>
#include <QTime>

#include "sleepEstimator.h"

namespace Health {

static const int HR_REACH_MIN = 5;      // a heart rate this near counts for a minute
static const int MERGE_GAP_MIN = 15;    // awake time that does not end a session
static const int MAIN_MIN = 60;         // the shortest main sleep
static const int NAP_MIN = 30;          // the shortest nap

QString sleepScoreLabel(int score)
{
    if (score >= 90)
        return QStringLiteral("Excellent");
    if (score >= 80)
        return QStringLiteral("Good");
    if (score >= 60)
        return QStringLiteral("Fair");
    return QStringLiteral("Poor");
}

namespace {

struct Run {
    int first, last; // indexes of the window's minutes, candidates at both ends
};

// stages of one session from the heart rate of its asleep minutes
void stage(SleepSession &s, const QVector<double> &hr, const QVector<char> &asleep, int first, int last)
{
    QVector<int> idx;
    for (int i = first; i <= last; i++)
        if (asleep[i])
            idx.append(i);
    if (idx.isEmpty())
        return;
    // a rolling mean of 5 minutes takes the single readings' noise out
    QVector<double> smooth(idx.size());
    for (int k = 0; k < idx.size(); k++) {
        double sum = 0;
        int n = 0;
        for (int j = qMax(0, k - 2); j <= qMin(idx.size() - 1, k + 2); j++) {
            sum += hr[idx[j]];
            n++;
        }
        smooth[k] = sum / n;
    }
    QVector<double> sorted = smooth;
    std::sort(sorted.begin(), sorted.end());
    const double deepBelow = sorted[(int)std::floor(0.20 * (sorted.size() - 1))];
    const double remAbove = sorted[(int)std::ceil(0.80 * (sorted.size() - 1))];
    double sum = 0;
    for (int k = 0; k < idx.size(); k++) {
        sum += hr[idx[k]];
        const bool inFirstHour = k < 60;
        if (smooth[k] <= deepBelow && deepBelow < remAbove)
            s.deepMin++;
        else if (!inFirstHour && smooth[k] >= remAbove && deepBelow < remAbove)
            s.remMin++;
        else
            s.lightMin++;
    }
    s.avgHr = sum / idx.size();
}

} // namespace

SleepNight estimateSleep(const MinuteSeries &series, const QDate &wakeDate, const HealthProfile &profile, int restingHr,
                         int goalMin)
{
    SleepNight night;
    night.wakeDate = wakeDate;
    night.goalMin = goalMin;
    const qint64 fromMin = MinuteSeries::minuteOf(QDateTime(wakeDate.addDays(-1), QTime(18, 0)).toMSecsSinceEpoch());
    const qint64 toMin = MinuteSeries::minuteOf(QDateTime(wakeDate, QTime(18, 0)).toMSecsSinceEpoch());
    const int n = (int)(toMin - fromMin);
    if (n <= 0 || series.isEmpty())
        return night;

    // the heart rate of every minute: its own, else the nearest within HR_REACH_MIN
    QVector<double> hr(n, 0);
    QVector<char> worn(n, 0), candidate(n, 0);
    for (int i = 0; i < n; i++) {
        const Minute m = series.at(fromMin + i);
        if (m.hr > 0) {
            hr[i] = m.hr;
            worn[i] = 1;
        }
    }
    // a minute between two readings that are no more than 2 x HR_REACH_MIN minutes apart takes the nearer one's
    // heart rate (the watch was on); the minutes before the first reading and after the last do not
    for (int i = 0; i < n; i++) {
        if (worn[i])
            continue;
        int before = -1, after = -1;
        for (int d = 1; d <= HR_REACH_MIN; d++) {
            if (before < 0 && i - d >= 0 && worn[i - d])
                before = i - d;
            if (after < 0 && i + d < n && worn[i + d])
                after = i + d;
        }
        if (before >= 0 && after >= 0)
            hr[i] = (i - before <= after - i) ? hr[before] : hr[after];
    }
    const double calm = restingHr + 0.20 * qMax(1, profile.maxHr() - restingHr);
    for (int i = 0; i < n; i++)
        candidate[i] = hr[i] > 0 && series.at(fromMin + i).steps == 0 && hr[i] <= calm;

    // sessions: candidate runs joined over gaps of up to MERGE_GAP_MIN
    QList<Run> runs;
    int i = 0;
    while (i < n) {
        if (!candidate[i]) { i++; continue; }
        int first = i, last = i;
        int j = i + 1;
        while (j < n) {
            if (candidate[j]) {
                last = j;
                j++;
                continue;
            }
            // a gap of awake minutes: another candidate within MERGE_GAP_MIN minutes joins it to the session
            int k = j;
            while (k < n && !candidate[k])
                k++;
            if (k < n && k - last - 1 <= MERGE_GAP_MIN) {
                last = k;
                j = k + 1;
                continue;
            }
            break;
        }
        runs.append({first, last});
        i = last + 1;
    }

    auto sessionOf = [&](const Run &r) {
        SleepSession s;
        s.startMs = MinuteSeries::msOf(fromMin + r.first);
        s.endMs = MinuteSeries::msOf(fromMin + r.last + 1);
        QVector<char> asleep(n, 0);
        for (int k = r.first; k <= r.last; k++) {
            if (candidate[k]) {
                asleep[k] = 1;
                s.asleepMin++;
            } else {
                s.awakeMin++;
            }
        }
        stage(s, hr, asleep, r.first, r.last);
        return s;
    };

    // the main sleep: the longest session that began between 20:00 and 08:00
    const qint64 mainFrom = QDateTime(wakeDate.addDays(-1), QTime(20, 0)).toMSecsSinceEpoch();
    const qint64 mainTo = QDateTime(wakeDate, QTime(8, 0)).toMSecsSinceEpoch();
    const qint64 napFrom = mainTo; // 08:00
    int mainIndex = -1;
    int mainLen = 0;
    for (int r = 0; r < runs.size(); r++) {
        const qint64 start = MinuteSeries::msOf(fromMin + runs[r].first);
        const int length = runs[r].last - runs[r].first + 1;
        const int asleepCount = std::count(candidate.begin() + runs[r].first, candidate.begin() + runs[r].last + 1, 1);
        if (start >= mainFrom && start <= mainTo && asleepCount >= MAIN_MIN && length > mainLen) {
            mainIndex = r;
            mainLen = length;
        }
    }
    if (mainIndex >= 0) {
        night.valid = true;
        night.main = sessionOf(runs[mainIndex]);
    }
    for (int r = 0; r < runs.size(); r++) {
        if (r == mainIndex)
            continue;
        SleepSession s = sessionOf(runs[r]);
        const double napCalm = restingHr + 0.10 * qMax(1, profile.maxHr() - restingHr);
        // a nap is in the daytime: a part of the night the main sleep did not take in is not one
        if (s.startMs >= napFrom && s.asleepMin >= NAP_MIN && s.avgHr <= napCalm) {
            night.naps.append(s);
            night.napMin += s.asleepMin;
        }
    }
    if (!night.valid)
        return night;

    const SleepSession &m = night.main;
    const double duration = 50.0 * qMin(1.0, (double)m.asleepMin / qMax(1, goalMin));
    const double share = m.asleepMin > 0 ? (double)(m.deepMin + m.remMin) / m.asleepMin : 0;
    const double composition = 25.0 * qBound(0.0, share / 0.40, 1.0);
    const double restoration = 25.0 * qBound(0.0, 1.0 - qMax(0.0, m.avgHr - restingHr) / 20.0, 1.0);
    night.score = (int)std::lround(duration + composition + restoration);
    night.scoreLabel = sleepScoreLabel(night.score);
    night.goalMet = m.asleepMin >= goalMin;
    return night;
}

static QJsonObject sessionJson(const SleepSession &s)
{
    return QJsonObject{{"start", (double)s.startMs}, {"end", (double)s.endMs}, {"asleep", s.asleepMin},
                       {"awake", s.awakeMin}, {"rem", s.remMin}, {"light", s.lightMin}, {"deep", s.deepMin},
                       {"avgHr", s.avgHr}};
}

static SleepSession sessionFromJson(const QJsonObject &o)
{
    SleepSession s;
    s.startMs = (qint64)o.value("start").toDouble();
    s.endMs = (qint64)o.value("end").toDouble();
    s.asleepMin = o.value("asleep").toInt();
    s.awakeMin = o.value("awake").toInt();
    s.remMin = o.value("rem").toInt();
    s.lightMin = o.value("light").toInt();
    s.deepMin = o.value("deep").toInt();
    s.avgHr = o.value("avgHr").toDouble();
    return s;
}

QJsonObject SleepNight::toJson() const
{
    QJsonArray napArray;
    for (const SleepSession &s : naps)
        napArray.append(sessionJson(s));
    return QJsonObject{{"version", 1}, {"valid", valid}, {"wakeDate", wakeDate.toString(Qt::ISODate)},
                       {"main", sessionJson(main)}, {"naps", napArray}, {"score", score},
                       {"scoreLabel", scoreLabel}, {"goalMin", goalMin}, {"goalMet", goalMet}, {"napMin", napMin}};
}

SleepNight SleepNight::fromJson(const QJsonObject &o)
{
    SleepNight n;
    n.valid = o.value("valid").toBool();
    n.wakeDate = QDate::fromString(o.value("wakeDate").toString(), Qt::ISODate);
    n.main = sessionFromJson(o.value("main").toObject());
    for (const QJsonValue &v : o.value("naps").toArray())
        n.naps.append(sessionFromJson(v.toObject()));
    n.score = o.value("score").toInt();
    n.scoreLabel = o.value("scoreLabel").toString();
    n.goalMin = o.value("goalMin").toInt(480);
    n.goalMet = o.value("goalMet").toBool();
    n.napMin = o.value("napMin").toInt();
    return n;
}

QVariantMap SleepNight::toMap() const
{
    auto session = [](const SleepSession &s) {
        const int total = s.asleepMin + s.awakeMin;
        auto pct = [&](int v) { return total > 0 ? (int)std::lround(100.0 * v / total) : 0; };
        return QVariantMap{{"startMs", s.startMs}, {"endMs", s.endMs},
                           {"durationMin", s.asleepMin + s.awakeMin}, {"asleepMin", s.asleepMin},
                           {"awakeMin", s.awakeMin}, {"remMin", s.remMin}, {"lightMin", s.lightMin},
                           {"deepMin", s.deepMin}, {"awakePct", pct(s.awakeMin)}, {"remPct", pct(s.remMin)},
                           {"lightPct", pct(s.lightMin)}, {"deepPct", pct(s.deepMin)}, {"avgHr", s.avgHr}};
    };
    QVariantList napList;
    for (const SleepSession &s : naps)
        napList << session(s);
    QVariantMap m;
    m["valid"] = valid;
    m["wakeDate"] = wakeDate;
    m["main"] = session(main);
    m["naps"] = napList;
    m["napMin"] = napMin;
    m["score"] = score;
    m["scoreLabel"] = scoreLabel;
    m["goalMin"] = goalMin;
    m["goalMet"] = goalMet;
    return m;
}

} // namespace Health
