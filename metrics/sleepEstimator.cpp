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

const char *const SLEEP_ALGO_V2 = "eos-motion-hr-2";

static const int HR_REACH_MIN = 5;       // a heart rate this near counts for a minute
static const int HR_REACH_WORN_MIN = 20; // the same with the off-body sensor saying on the wrist (guess)
static const int MERGE_GAP_MIN = 15;     // awake time that does not end a session
static const int MAIN_MIN = 60;          // the shortest main sleep (stock: sleep after about an hour still)
static const int NAP_MIN = 20;           // the shortest nap (Google's sleep page; version 1 had 30)
static const int MOVE_NEAR_MIN = 3;      // no Deep or REM this near a movement or a step (guess)
static const int STAGE_MIN_RUN = 3;      // a shorter run of a sleep stage takes the one before it (guess)
// a motion state is not taken for longer than this after its record (guess: a stream that stopped, sensorfwd gone)
static const qint64 MAX_KNOWN_MS = 4LL * 3600 * 1000;
// this many motion records within BUSY_SPAN_MS: re-armed at once, so the display was on and the watch in use (guess)
static const int BUSY_RECORDS = 4;
static const qint64 BUSY_SPAN_MS = 3LL * 60 * 1000;
static const qint64 STILL_COVER_MS = 30000; // a still minute: known still for at least half of it (guess)
// a session's ends whose 10-minute mean heart rate is this far above its lower quartile are not sleep (guess)
static const int ONSET_MEAN_MIN = 10;
static const double ONSET_MARGIN_BPM = 10;

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

QString sleepStageName(int stage)
{
    switch (stage) {
    case StageAwake: return QStringLiteral("Awake");
    case StageDeep: return QStringLiteral("Deep");
    case StageRem: return QStringLiteral("REM");
    default: return QStringLiteral("Light");
    }
}

namespace {

qint64 recordMs(const QPointF &p) { return (qint64)std::llround(p.x()) * 1000; }
int recordValue(const QPointF &p) { return (int)std::lround(p.y()); }

// what happens to the minutes [aMs, bMs) of the window: fn(index, overlap ms)
template<typename F>
void forMinutes(qint64 fromMinute, int n, qint64 aMs, qint64 bMs, F fn)
{
    aMs = qMax(aMs, MinuteSeries::msOf(fromMinute));
    bMs = qMin(bMs, MinuteSeries::msOf(fromMinute + n));
    while (aMs < bMs) {
        const int i = (int)(MinuteSeries::minuteOf(aMs) - fromMinute);
        const qint64 end = qMin(bMs, MinuteSeries::msOf(fromMinute + i + 1));
        fn(i, end - aMs);
        aMs = end;
    }
}

struct Run {
    int first, last; // indexes of the window's minutes, candidates at both ends
};

} // namespace

/*
 * The motion records (activityStreams.h): both detectors are one-shot, and both are armed when a 2 is written.
 * Since an arming, the first event says the state until the other detector reports (the next record is a 0 or 1
 * then) or, if nothing reports, until the next arming: the other detector was armed and silent all that time. After
 * the second event both are spent: only its moment is known, until the next arming. A movement's minute is moved,
 * and so is the time a movement held until stillness was reported; stillness counts for a minute it covers half of.
 */
QVector<char> motionMinutes(const QList<QPointF> &records, qint64 fromMinute, int n)
{
    QVector<char> out(qMax(0, n), (char)MotionUnknown);
    if (n <= 0 || records.isEmpty())
        return out;
    QVector<qint64> still(n, 0);
    QVector<char> moved(n, 0), busy(n, 0);
    const int count = records.size();
    for (int k = 0; k < count; k++) {
        const int v = recordValue(records[k]);
        if (v != 0 && v != 1)
            continue; // an arming: what comes after it is known from the next event on
        const qint64 t = recordMs(records[k]);
        const bool last = k + 1 >= count;
        const qint64 next = last ? MinuteSeries::msOf(fromMinute + n) : recordMs(records[k + 1]);
        const int nextV = last ? 2 : recordValue(records[k + 1]);
        // the first event since an arming (the record before it is one); the first record of all is not known to be
        const bool first = k > 0 && recordValue(records[k - 1]) == 2;
        qint64 until = t;
        if (nextV == 0 || nextV == 1 || first)
            until = qMin(next, t + MAX_KNOWN_MS);
        if (v == 1)
            forMinutes(fromMinute, n, t, qMax(until, t + 1), [&](int i, qint64) { moved[i] = 1; });
        else
            forMinutes(fromMinute, n, t, until, [&](int i, qint64 overlap) { still[i] += overlap; });
    }
    // records that come thick and fast: the detectors re-armed at once, the display on, the watch in use
    for (int k = 0; k + BUSY_RECORDS - 1 < count; k++) {
        const qint64 a = recordMs(records[k]), b = recordMs(records[k + BUSY_RECORDS - 1]);
        if (b - a <= BUSY_SPAN_MS)
            forMinutes(fromMinute, n, a, b + 1, [&](int i, qint64) { busy[i] = 1; });
    }
    for (int i = 0; i < n; i++)
        out[i] = busy[i] ? MotionBusy
                 : moved[i] ? MotionMoved
                 : still[i] >= STILL_COVER_MS ? MotionStill : MotionUnknown;
    return out;
}

QVector<signed char> offBodyMinutes(const QList<QPointF> &records, qint64 fromMinute, int n)
{
    QVector<signed char> out(qMax(0, n), (signed char)-1);
    if (n <= 0 || records.isEmpty())
        return out;
    QVector<qint64> on(n, 0);
    QVector<char> off(n, 0);
    for (int k = 0; k < records.size(); k++) {
        const int v = recordValue(records[k]);
        const qint64 t = recordMs(records[k]);
        const qint64 next = k + 1 < records.size() ? recordMs(records[k + 1]) : MinuteSeries::msOf(fromMinute + n);
        if (v == 1)
            forMinutes(fromMinute, n, t, next, [&](int i, qint64) { off[i] = 1; });
        else if (v == 0)
            forMinutes(fromMinute, n, t, next, [&](int i, qint64 overlap) { on[i] += overlap; });
    }
    for (int i = 0; i < n; i++)
        out[i] = off[i] ? 1 : on[i] >= 60000 ? 0 : -1;
    return out;
}

QVector<char> bedtimeMinutes(const QList<QPointF> &records, qint64 fromMinute, int n)
{
    QVector<char> out(qMax(0, n), 0);
    QList<QPointF> sorted = records;
    std::stable_sort(sorted.begin(), sorted.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    char state = 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        const double minuteStart = MinuteSeries::msOf(fromMinute + i) / 1000.0;
        const double minuteEnd = MinuteSeries::msOf(fromMinute + i + 1) / 1000.0;
        // the state the minute begins in (a change at its start included); Bedtime on at any time of it counts
        while (k < sorted.size() && sorted[k].x() <= minuteStart) {
            state = sorted[k].y() >= 0.5 ? 1 : 0;
            k++;
        }
        bool on = state == 1;
        while (k < sorted.size() && sorted[k].x() < minuteEnd) {
            state = sorted[k].y() >= 0.5 ? 1 : 0;
            on = on || state == 1;
            k++;
        }
        out[i] = on ? 1 : 0;
    }
    return out;
}

SleepNight estimateSleep(const MinuteSeries &series, const QDate &wakeDate, const HealthProfile &profile, int restingHr,
                         int goalMin, const SleepContext &context)
{
    SleepNight night;
    night.wakeDate = wakeDate;
    night.goalMin = goalMin;
    const qint64 fromMin = MinuteSeries::minuteOf(QDateTime(wakeDate.addDays(-1), QTime(18, 0)).toMSecsSinceEpoch());
    const qint64 toMin =
            MinuteSeries::minuteOf(QDateTime(wakeDate, QTime(SLEEP_WINDOW_END_HOUR, 0)).toMSecsSinceEpoch());
    const int n = (int)(toMin - fromMin);
    if (n <= 0 || series.isEmpty())
        return night;

    const QVector<signed char> off = offBodyMinutes(context.offBody, fromMin, n);
    const QVector<char> inBed = bedtimeMinutes(context.bedtime, fromMin, n);
    const QVector<char> motion = motionMinutes(context.motion, fromMin, n);
    QVector<char> cadence(n, 0);
    for (const QPointF &c : context.cadence) {
        const qint64 i = MinuteSeries::minuteOf(recordMs(c)) - fromMin;
        if (i >= 0 && i < n && c.y() >= 1)
            cadence[i] = 1;
    }

    // the heart rate of every minute: its own, else the nearest within reach on both sides; never off the wrist
    QVector<double> hr(n, 0);
    QVector<char> reading(n, 0), worn(n, 0), stepping(n, 0), candidate(n, 0);
    for (int i = 0; i < n; i++) {
        const Minute m = series.at(fromMin + i);
        stepping[i] = m.steps > 0 || cadence[i];
        if (off[i] == 1)
            continue; // off the wrist: not worn, and a reading of the minute is not the wearer's
        if (m.hr > 0) {
            hr[i] = m.hr;
            reading[i] = 1;
            worn[i] = 1;
        }
    }
    for (int i = 0; i < n; i++) {
        if (reading[i] || off[i] == 1)
            continue;
        // the off-body sensor knows it was on: a longer gap in the readings is still the wearer's (guess)
        const int reach = off[i] == 0 ? HR_REACH_WORN_MIN : HR_REACH_MIN;
        int before = -1, after = -1;
        bool blockedBefore = false, blockedAfter = false;
        for (int d = 1; d <= reach; d++) {
            if (!blockedBefore && before < 0 && i - d >= 0) {
                if (off[i - d] == 1)
                    blockedBefore = true; // the watch came off in between
                else if (reading[i - d])
                    before = i - d;
            }
            if (!blockedAfter && after < 0 && i + d < n) {
                if (off[i + d] == 1)
                    blockedAfter = true;
                else if (reading[i + d])
                    after = i + d;
            }
        }
        if (before >= 0 && after >= 0) {
            hr[i] = (i - before <= after - i) ? hr[before] : hr[after];
            worn[i] = 1;
        }
        if (off[i] == 0)
            worn[i] = 1;
    }
    // asleep: a heart rate within the lower fifth of the reserve above resting; in the daytime (from 08:00 of the
    // morning's day) within the lower tenth, as near resting as a nap is (an hour at the desk is no sleep: guess)
    const double calm = restingHr + 0.20 * qMax(1, profile.maxHr() - restingHr);
    const double napCalm = restingHr + 0.10 * qMax(1, profile.maxHr() - restingHr);
    // in Bedtime the lower fourth (R5 E-30, guess): the mode says the wearer means to sleep
    const double bedCalm = restingHr + 0.25 * qMax(1, profile.maxHr() - restingHr);
    const qint64 dayFrom = MinuteSeries::minuteOf(QDateTime(wakeDate, QTime(8, 0)).toMSecsSinceEpoch()) - fromMin;
    for (int i = 0; i < n; i++)
        candidate[i] = worn[i] && !stepping[i] && motion[i] != MotionBusy &&
                       (hr[i] > 0 ? hr[i] <= (i >= dayFrom ? napCalm : inBed[i] ? bedCalm : calm)
                                  : motion[i] == MotionStill);

    // sessions: candidate runs joined over gaps of up to MERGE_GAP_MIN in which the watch stayed on
    auto findRuns = [&]() {
        QList<Run> found;
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
                // a gap of awake minutes: another candidate within MERGE_GAP_MIN minutes joins it to the session,
                // unless the watch was off the wrist in between
                int k = j;
                bool offInGap = false;
                while (k < n && !candidate[k]) {
                    offInGap = offInGap || off[k] == 1;
                    k++;
                }
                if (k < n && !offInGap && k - last - 1 <= MERGE_GAP_MIN) {
                    last = k;
                    j = k + 1;
                    continue;
                }
                break;
            }
            found.append({first, last});
            i = last + 1;
        }
        return found;
    };
    // Falling asleep and waking up: the session begins at its first minute whose heart rate, and the mean of the
    // ONSET_MEAN_MIN minutes from it on, are at the session's own sleeping level (its lower quartile plus
    // ONSET_MARGIN_BPM) and ends at the last such minute (looking back); what is before and after is time awake in
    // bed or on the sofa (guess)
    auto trimEnds = [&](const Run &r) {
        QVector<double> rates;
        for (int k = r.first; k <= r.last; k++)
            if (candidate[k] && reading[k])
                rates.append(hr[k]);
        if (rates.size() < ONSET_MEAN_MIN)
            return false;
        std::sort(rates.begin(), rates.end());
        const double limit = rates[rates.size() / 4] + ONSET_MARGIN_BPM;
        auto low = [&](int k, int step) {
            if (!candidate[k] || !reading[k] || hr[k] > limit)
                return false;
            double sum = 0;
            int c = 0;
            for (int j = k; j >= r.first && j <= r.last && qAbs(j - k) < ONSET_MEAN_MIN; j += step) {
                if (candidate[j] && reading[j]) {
                    sum += hr[j];
                    c++;
                }
            }
            return sum / c <= limit;
        };
        int onset = -1, offset = -1;
        for (int k = r.first; k <= r.last && onset < 0; k++)
            if (low(k, 1))
                onset = k;
        if (onset < 0)
            return false; // nothing at a sleeping level: left as it is
        for (int k = r.last; k >= onset && offset < 0; k--)
            if (low(k, -1))
                offset = k;
        bool changed = false;
        for (int k = r.first; k <= r.last; k++) {
            if ((k < onset || k > offset) && candidate[k]) {
                candidate[k] = 0;
                changed = true;
            }
        }
        return changed;
    };
    QList<Run> runs = findRuns();
    bool trimmed = false;
    for (const Run &r : runs)
        trimmed = trimEnds(r) || trimmed;
    if (trimmed)
        runs = findRuns();

    struct Made {
        SleepSession s;
        int readings = 0; // asleep minutes with a heart rate reading
        int moved = 0;    // asleep minutes with a movement
    };
    auto sessionOf = [&](const Run &r) {
        Made made;
        SleepSession &s = made.s;
        s.startMs = MinuteSeries::msOf(fromMin + r.first);
        s.endMs = MinuteSeries::msOf(fromMin + r.last + 1);
        const int len = r.last - r.first + 1;
        // the 5-minute mean heart rate of the asleep minutes that have one (the single readings' noise out)
        QVector<int> idx;
        for (int k = r.first; k <= r.last; k++)
            if (candidate[k] && hr[k] > 0)
                idx.append(k);
        QVector<double> smooth(len, -1);
        QVector<double> sorted;
        for (int k = 0; k < idx.size(); k++) {
            double sum = 0;
            int c = 0;
            for (int j = qMax(0, k - 2); j <= qMin(idx.size() - 1, k + 2); j++) {
                sum += hr[idx[j]];
                c++;
            }
            smooth[idx[k] - r.first] = sum / c;
            sorted.append(sum / c);
        }
        std::sort(sorted.begin(), sorted.end());
        const double deepBelow = sorted.isEmpty() ? 0 : sorted[(int)std::floor(0.20 * (sorted.size() - 1))];
        const double remAbove = sorted.isEmpty() ? 0 : sorted[(int)std::ceil(0.80 * (sorted.size() - 1))];
        const bool split = !sorted.isEmpty() && deepBelow < remAbove;

        QVector<int> st(len, StageAwake);
        int asleepSeen = 0;
        double sum = 0;
        for (int k = r.first; k <= r.last; k++) {
            if (!candidate[k])
                continue; // awake inside the session
            const bool inFirstHour = asleepSeen < 60;
            asleepSeen++;
            if (reading[k]) {
                sum += hr[k];
                made.readings++;
            }
            if (motion[k] == MotionMoved)
                made.moved++;
            // a minute without a reading (worn and still) goes by the nearest one that has one
            double sm = smooth[k - r.first];
            for (int d = 1; sm < 0 && d < len; d++) {
                if (k - d >= r.first && smooth[k - d - r.first] >= 0)
                    sm = smooth[k - d - r.first];
                else if (k + d <= r.last && smooth[k + d - r.first] >= 0)
                    sm = smooth[k + d - r.first];
            }
            bool nearMove = false;
            for (int d = -MOVE_NEAR_MIN; d <= MOVE_NEAR_MIN && !nearMove; d++) {
                const int j = k + d;
                nearMove = j >= 0 && j < n && (motion[j] == MotionMoved || motion[j] == MotionBusy || stepping[j]);
            }
            int stage = StageLight;
            if (!nearMove && split && sm >= 0) {
                if (sm <= deepBelow)
                    stage = StageDeep;
                else if (!inFirstHour && sm >= remAbove)
                    stage = StageRem;
            }
            st[k - r.first] = stage;
        }
        // a sleep stage held for less than STAGE_MIN_RUN minutes takes the one before it (at the start or after an
        // awake spell: the one after it)
        for (int a = 0; a < len;) {
            int b = a;
            while (b < len && st[b] == st[a])
                b++;
            if (st[a] != StageAwake && b - a < STAGE_MIN_RUN) {
                const int fill = a > 0 && st[a - 1] != StageAwake ? st[a - 1]
                                 : b < len && st[b] != StageAwake ? st[b] : st[a];
                for (int k = a; k < b; k++)
                    st[k] = fill;
            }
            a = b;
        }
        // the totals and the timeline
        for (int a = 0; a < len;) {
            int b = a;
            while (b < len && st[b] == st[a])
                b++;
            const int minutes = b - a;
            switch (st[a]) {
            case StageAwake: s.awakeMin += minutes; break;
            case StageDeep: s.deepMin += minutes; break;
            case StageRem: s.remMin += minutes; break;
            default: s.lightMin += minutes; break;
            }
            SleepStageSpan span;
            span.startMs = MinuteSeries::msOf(fromMin + r.first + a);
            span.endMs = MinuteSeries::msOf(fromMin + r.first + b);
            span.stage = st[a];
            s.stages.append(span);
            a = b;
        }
        s.asleepMin = s.lightMin + s.deepMin + s.remMin;
        s.avgHr = made.readings > 0 ? sum / made.readings : 0;
        return made;
    };

    // the main sleep: the longest session that began between 20:00 (19:00 in Bedtime) and 08:00
    const qint64 mainFrom = QDateTime(wakeDate.addDays(-1), QTime(20, 0)).toMSecsSinceEpoch();
    const qint64 mainFromInBed = QDateTime(wakeDate.addDays(-1), QTime(19, 0)).toMSecsSinceEpoch();
    const qint64 mainTo = QDateTime(wakeDate, QTime(8, 0)).toMSecsSinceEpoch();
    const qint64 napFrom = mainTo; // 08:00
    int mainIndex = -1;
    int mainLen = 0;
    for (int r = 0; r < runs.size(); r++) {
        const qint64 start = MinuteSeries::msOf(fromMin + runs[r].first);
        const int length = runs[r].last - runs[r].first + 1;
        const int asleepCount = std::count(candidate.begin() + runs[r].first, candidate.begin() + runs[r].last + 1, 1);
        const bool began = start >= mainFrom || (inBed[runs[r].first] && start >= mainFromInBed);
        if (began && start <= mainTo && asleepCount >= MAIN_MIN && length > mainLen) {
            mainIndex = r;
            mainLen = length;
        }
    }
    int mainMoved = 0;
    if (mainIndex >= 0) {
        night.valid = true;
        const Made made = sessionOf(runs[mainIndex]);
        night.main = made.s;
        mainMoved = made.moved;
    }
    for (int r = 0; r < runs.size(); r++) {
        if (r == mainIndex)
            continue;
        const Made made = sessionOf(runs[r]);
        const SleepSession &s = made.s;
        // a nap is in the daytime (a part of the night the main sleep did not take in is not one), near resting,
        // and the heart rate was read in most of it (not a watch lying still with the sensor unsure)
        // one still going at the window's end is the next night's (an early night), not a nap
        if (s.startMs >= napFrom && runs[r].last < n - 1 && s.asleepMin >= NAP_MIN && s.avgHr <= napCalm
            && 2 * made.readings >= s.asleepMin) {
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
    const double heart = qBound(0.0, 1.0 - qMax(0.0, m.avgHr - restingHr) / 20.0, 1.0);
    const int total = m.asleepMin + m.awakeMin;
    const double restless = total > 0 ? (double)(m.awakeMin + mainMoved) / total : 0;
    const double calmness = qBound(0.0, 1.0 - restless / 0.25, 1.0);
    const double restoration = 25.0 * (0.6 * heart + 0.4 * calmness);
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

// (the format of the sleep/<date>.json files of a sensorlogd from before health.db: read for the import, no timeline)
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
        QVariantList stages;
        for (const SleepStageSpan &span : s.stages)
            stages << QVariantMap{{"startMs", span.startMs}, {"endMs", span.endMs}, {"stage", span.stage},
                                  {"name", sleepStageName(span.stage)}};
        return QVariantMap{{"startMs", s.startMs}, {"endMs", s.endMs},
                           {"durationMin", s.asleepMin + s.awakeMin}, {"asleepMin", s.asleepMin},
                           {"awakeMin", s.awakeMin}, {"remMin", s.remMin}, {"lightMin", s.lightMin},
                           {"deepMin", s.deepMin}, {"awakePct", pct(s.awakeMin)}, {"remPct", pct(s.remMin)},
                           {"lightPct", pct(s.lightMin)}, {"deepPct", pct(s.deepMin)}, {"avgHr", s.avgHr},
                           {"stages", stages}};
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
