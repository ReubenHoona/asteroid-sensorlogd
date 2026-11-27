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

#include "skinTemp.h"

namespace Health {

namespace {

double round2(double v)
{
    return std::round(v * 100.0) / 100.0;
}

} // namespace

bool skinTempWanted(bool running, const MinuteSeries &series, qint64 nowMs, bool worn, int restingHr, int maxHr)
{
    if (!worn)
        return false;
    const int hour = QDateTime::fromMSecsSinceEpoch(nowMs).time().hour();
    if (hour >= SKIN_STOP_HOUR && hour < SKIN_FROM_HOUR)
        return false;
    // the last SKIN_LOOK_MIN minutes, the one now included
    const qint64 last = MinuteSeries::minuteOf(nowMs);
    const qint64 first = last - SKIN_LOOK_MIN + 1;
    int steps = 0;
    int hrMinutes = 0;
    double hrSum = 0;
    const QMap<qint64, Minute> &data = series.data();
    for (auto it = data.lowerBound(first); it != data.end() && it.key() <= last; ++it) {
        steps += qMax(0, it->steps);
        if (it->hrCount > 0 && it->hr > 0) {
            hrMinutes++;
            hrSum += it->hr;
        }
    }
    const double reserve = qMax(1, maxHr - restingHr);
    const double meanHr = hrMinutes > 0 ? hrSum / hrMinutes : 0;
    if (running) {
        if (steps >= SKIN_STOP_STEPS)
            return false;
        return !(hrMinutes >= SKIN_MIN_HR_MINUTES && meanHr > restingHr + SKIN_AWAKE_RESERVE * reserve);
    }
    const bool startHours = hour >= SKIN_FROM_HOUR || hour < SKIN_START_UNTIL_HOUR;
    return startHours && steps <= SKIN_START_MAX_STEPS && hrMinutes >= SKIN_MIN_HR_MINUTES &&
           meanHr <= restingHr + SKIN_ASLEEP_RESERVE * reserve;
}

SkinTempNight skinTempNight(const QDate &wakeDate, const SleepSession &main,
                            const QVector<QPair<qint64, double>> &minutes, const QList<QPointF> &offBody,
                            const QList<SkinTempNight> &earlier)
{
    SkinTempNight n;
    n.wakeDate = wakeDate;
    n.startMs = main.startMs;
    n.endMs = main.endMs;
    // the asleep spans: the timeline without its Awake spans (a night without a timeline: all of it)
    QVector<QPair<qint64, qint64>> asleep;
    for (const SleepStageSpan &span : main.stages)
        if (span.stage != StageAwake && span.endMs > span.startMs)
            asleep.append(qMakePair(span.startMs, span.endMs));
    if (main.stages.isEmpty() && main.endMs > main.startMs)
        asleep.append(qMakePair(main.startMs, main.endMs));
    const qint64 firstMinute = MinuteSeries::minuteOf(main.startMs);
    const int count = main.endMs > main.startMs ? (int)(MinuteSeries::minuteOf(main.endMs) - firstMinute + 1) : 0;
    const QVector<signed char> off = offBodyMinutes(offBody, firstMinute, count);
    double sum = 0;
    for (const QPair<qint64, double> &m : minutes) {
        // a minute counts by its middle
        const qint64 mid = m.first + 30000;
        bool in = false;
        for (const QPair<qint64, qint64> &a : asleep)
            in = in || (mid >= a.first && mid < a.second);
        if (!in)
            continue;
        const qint64 i = MinuteSeries::minuteOf(m.first) - firstMinute;
        if (i >= 0 && i < off.size() && off[(int)i] == 1)
            continue; // off the wrist
        if (!std::isfinite(m.second) || m.second < SKIN_MIN_C || m.second > SKIN_MAX_C)
            continue;
        sum += m.second;
        n.minutes++;
    }
    n.hasMean = n.minutes >= SKIN_MIN_MINUTES;
    if (n.hasMean)
        n.meanC = round2(sum / n.minutes);
    // the baseline: the nights with a value of the SKIN_BASELINE_DAYS days before this one
    QList<double> before;
    const QDate from = wakeDate.addDays(-SKIN_BASELINE_DAYS);
    for (const SkinTempNight &e : earlier)
        if (e.hasMean && e.wakeDate.isValid() && e.wakeDate < wakeDate && e.wakeDate >= from)
            before.append(e.meanC);
    n.nights = before.size();
    if (!n.hasMean || before.size() < SKIN_BASELINE_NIGHTS)
        return n;
    double mean = 0;
    for (double v : before)
        mean += v;
    mean /= before.size();
    double var = 0;
    for (double v : before)
        var += (v - mean) * (v - mean);
    const double sd = std::sqrt(var / (before.size() - 1));
    const double half = std::min(SKIN_RANGE_MAX_C, std::max(SKIN_RANGE_MIN_C, SKIN_RANGE_SD * sd));
    n.hasVariation = true;
    n.baselineC = round2(mean);
    n.devC = round2(n.meanC - mean);
    n.loC = -round2(half);
    n.hiC = round2(half);
    return n;
}

SkinTempView skinTempView(const QDate &date, const QList<SkinTempNight> &nights, qint64 trackingSinceMs,
                          qint64 trackingEndMs, qint64 nowMs)
{
    SkinTempView v;
    const SkinTempNight *today = nullptr;
    int withValue = 0;
    const QDate from = date.addDays(-SKIN_BASELINE_DAYS);
    for (const SkinTempNight &n : nights) {
        if (n.wakeDate == date)
            today = &n;
        if (n.hasMean && n.wakeDate.isValid() && n.wakeDate <= date && n.wakeDate >= from)
            withValue++;
    }
    if (today)
        v.night = *today;
    const int before = withValue - (today && today->hasMean ? 1 : 0);
    // the nights still to sleep with the watch before one is compared: the baseline's three, then that night
    v.nightsLeft = before >= SKIN_BASELINE_NIGHTS ? 0 : SKIN_BASELINE_NIGHTS + 1 - withValue;
    if (trackingSinceMs > 0 && nowMs >= trackingSinceMs && nowMs - trackingSinceMs < SKIN_TRACKING_MAX_MS) {
        v.state = SkinTracking;
    } else if (today && today->hasVariation) {
        v.state = today->devC > today->hiC ? SkinAboveRange : today->devC < today->loC ? SkinBelowRange : SkinInRange;
    } else if (withValue == 0) {
        v.state = SkinNoData;
    } else if (before < SKIN_BASELINE_NIGHTS) {
        v.state = SkinCalibrating;
    } else if (!today && trackingEndMs > 0 && nowMs >= trackingEndMs && nowMs - trackingEndMs < SKIN_ANALYZING_MS) {
        v.state = SkinAnalyzing; // the baseline is there and the night is not through yet
    } else {
        v.state = SkinNoData;    // the baseline is there, last night has no value
    }
    return v;
}

QString skinTempStateName(int state)
{
    switch (state) {
    case SkinCalibrating: return QStringLiteral("calibrating");
    case SkinTracking: return QStringLiteral("tracking");
    case SkinAnalyzing: return QStringLiteral("analyzing");
    case SkinInRange: return QStringLiteral("inRange");
    case SkinAboveRange: return QStringLiteral("aboveRange");
    case SkinBelowRange: return QStringLiteral("belowRange");
    default: return QStringLiteral("noData");
    }
}

QVariantMap SkinTempView::toMap() const
{
    return QVariantMap{{"state", skinTempStateName(state)}, {"nightsLeft", nightsLeft},
                       {"wakeDate", night.wakeDate}, {"measured", night.wakeDate.isValid()},
                       {"minutes", night.minutes}, {"hasVariation", night.hasVariation}, {"devC", night.devC},
                       {"loC", night.loC}, {"hiC", night.hiC}, {"nights", night.nights}};
}

} // namespace Health
