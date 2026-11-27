// The Health metrics (metrics/*): the numbers of health-fitness-11 (stride and distance, energy, floors, zones,
// Active Zone Minutes, hourly activity), the high and low heart rate alerts (-16), reminders to move and their
// celebrations (-15), the sleep estimate (-17) and the auto-detected exercise prompts (-28), on recorded series:
// minutes of steps and heart rate as the sensor hub's batches give them. Every number of the stock behaviour the
// gaps name (250 steps an hour, minute 50, 9:00-18:00, 10 minutes, the 40/60/85 % zone bands, 1 and 2 Active
// Zone Minutes, 150 a week) is an assertion here. The dates are fixed local dates (Thursday 2026-11-12); no
// assertion depends on the time zone.
#include <cmath>

#include <QTest>

#include <MGConfItem>

#include "../metrics/autoExercise.h"
#include "../metrics/healthMetrics.h"
#include "../metrics/healthProfile.h"
#include "../metrics/hrAlerts.h"
#include "../metrics/moveReminder.h"
#include "../metrics/restingHr.h"
#include "../metrics/sleepEstimator.h"
#include "testenv.h"

using namespace Health;

namespace {

const QDate THU(2026, 11, 12);

qint64 ms(const QDate &d, int h, int m = 0, int s = 0) { return QDateTime(d, QTime(h, m, s)).toMSecsSinceEpoch(); }
qint64 minuteAt(const QDate &d, int h, int m = 0) { return MinuteSeries::minuteOf(ms(d, h, m)); }

// n minutes from h:m with the same steps and heart rate
void fill(MinuteSeries &s, const QDate &d, int h, int m, int n, int steps, double hr)
{
    for (int i = 0; i < n; i++) {
        const qint64 t = ms(d, h, m) + (qint64)i * 60000;
        if (steps > 0)
            s.addSteps(t, steps);
        if (hr > 0)
            s.addHr(t, (int)hr);
    }
}

HealthProfile profile(HealthProfile::Sex sex = HealthProfile::Male, double h = 175, double w = 70, int age = 30)
{
    HealthProfile p;
    p.sex = sex;
    p.heightCm = h;
    p.weightKg = w;
    p.age = age;
    return p;
}

}

class TestHealth : public QObject
{
    Q_OBJECT

private slots:
    // ---- the profile ----
    void profileFromTheDconfValues()
    {
        const QDate today(2026, 11, 12);
        HealthProfile p = HealthProfile::fromValues("Female", 168.4, 61.5, "1990-11-13", QVariant(), 55, today);
        QCOMPARE(p.sex, HealthProfile::Female);
        QCOMPARE(p.heightCm, 168.0);
        QCOMPARE(p.weightKg, 61.5);
        QCOMPARE(p.age, 35); // the birthday is tomorrow
        QCOMPARE(p.restingHr, 55);
        QCOMPARE(p.maxHr(), 185); // 220 - age

        p = HealthProfile::fromValues("male", 180, 80, "1990-11-12", QVariant(), QVariant(), today);
        QCOMPARE(p.age, 36); // the birthday is today
        QCOMPARE(p.sex, HealthProfile::Male);
        QCOMPARE(p.restingHr, 0);

        p = HealthProfile::fromValues(2, QVariant(), QVariant(), QVariant(), 41, QVariant(), today);
        QCOMPARE(p.sex, HealthProfile::Female); // the number 2, as the picker may store it
        QCOMPARE(p.age, 41);
    }

    void unsetOrNonsenseProfileKeepsTheNeutralValues()
    {
        HealthProfile p = HealthProfile::fromValues(QVariant(), QVariant(), QVariant(), QVariant(), QVariant(), QVariant(), THU);
        QCOMPARE(p.sex, HealthProfile::Unspecified);
        QCOMPARE(p.heightCm, 170.0);
        QCOMPARE(p.weightKg, 70.0);
        QCOMPARE(p.age, 30);
        p = HealthProfile::fromValues("non-binary", 12, 5, "2030-01-01", 300, 500, THU);
        QCOMPARE(p.sex, HealthProfile::Unspecified);
        QCOMPARE(p.heightCm, 170.0);
        QCOMPARE(p.weightKg, 70.0);
        QCOMPARE(p.age, 30);
        QCOMPARE(p.restingHr, 0);
    }

    void profileIsReadFromTheKeys()
    {
        MGConfItem::clearTestValues();
        MGConfItem::setTestValue("/org/asteroidos/health/profile/gender", "male");
        MGConfItem::setTestValue("/org/asteroidos/health/profile/height-cm", 181);
        MGConfItem::setTestValue("/org/asteroidos/health/profile/weight-kg", 77.5);
        MGConfItem::setTestValue("/org/asteroidos/health/profile/birth-date", "1986-05-02");
        const HealthProfile p = HealthProfile::load(THU);
        QCOMPARE(p.sex, HealthProfile::Male);
        QCOMPARE(p.heightCm, 181.0);
        QCOMPARE(p.weightKg, 77.5);
        QCOMPARE(p.age, 40);
        MGConfItem::clearTestValues();
    }

    // ---- distance and energy ----
    void strideAndDistanceFromHeightAndSex()
    {
        QVERIFY(qAbs(strideM(profile(HealthProfile::Male, 175)) - 0.72625) < 1e-9);   // 0.415 x height
        QVERIFY(qAbs(strideM(profile(HealthProfile::Female, 165)) - 0.68145) < 1e-9); // 0.413 x height
        QVERIFY(qAbs(strideM(profile(HealthProfile::Unspecified, 170)) - 0.7038) < 1e-9);
        QVERIFY(qAbs(distanceM(10000, profile(HealthProfile::Male, 175)) - 7262.5) < 1e-6);
        QCOMPARE(distanceM(-5, profile()), 0.0);
    }

    void basalEnergyIsMifflinStJeor()
    {
        // 10 w + 6.25 h - 5 a + 5 (male) / - 161 (female)
        QVERIFY(qAbs(bmrKcalPerDay(profile(HealthProfile::Male, 175, 70, 30)) - 1648.75) < 1e-9);
        QVERIFY(qAbs(bmrKcalPerDay(profile(HealthProfile::Female, 165, 60, 40)) - 1270.25) < 1e-9);
        QVERIFY(qAbs(bmrKcalPerDay(profile(HealthProfile::Unspecified, 170, 70, 30)) - 1534.5) < 1e-9);
    }

    void walkingRunningAndHeartRateEnergy()
    {
        const HealthProfile p = profile(HealthProfile::Male, 175, 70, 30);
        Minute walk;
        walk.steps = 100; // 0.5 kcal per kg per km: 0.5 x 70 x 0.072625 km
        QVERIFY(qAbs(activeKcalForMinute(walk, p, 60) - 2.541875) < 1e-6);
        Minute run;
        run.steps = 150; // 140 steps a minute and up: 1.0 kcal per kg per km
        QVERIFY(qAbs(activeKcalForMinute(run, p, 60) - 70 * 0.1089375) < 1e-6);
        Minute hrOnly;
        hrOnly.hr = 130; // moderate zone: Keytel (male) less the resting energy of the minute
        const double gross = (-55.0969 + 0.6309 * 130 + 0.1988 * 70 + 0.2017 * 30) / 4.184;
        QVERIFY(qAbs(activeKcalForMinute(hrOnly, p, 60) - (gross - 1648.75 / 1440)) < 1e-6);
        Minute light;
        light.hr = 90; // light zone: heart rate does not count, no steps
        QCOMPARE(activeKcalForMinute(light, p, 60), 0.0);
        Minute both;
        both.steps = 20;
        both.hr = 130;
        QVERIFY(activeKcalForMinute(both, p, 60) > activeKcalForMinute(walk, p, 60) * 0.1); // the larger of the two
        QVERIFY(qAbs(activeKcalForMinute(both, p, 60) - activeKcalForMinute(hrOnly, p, 60)) < 1e-9);
    }

    void energyBurnedIsRestingPlusActive()
    {
        const HealthProfile p = profile(HealthProfile::Male, 175, 70, 30);
        MinuteSeries s;
        QVERIFY(qAbs(energyKcal(s, ms(THU, 0), ms(THU.addDays(1), 0), p, 60) - 1648.75) < 1e-6); // a whole day
        QVERIFY(qAbs(energyKcal(s, ms(THU, 0), ms(THU, 12), p, 60) - 1648.75 / 2) < 1e-6);      // prorated by time
        fill(s, THU, 9, 0, 30, 100, 0); // 30 minutes of 100 steps
        const double active = 30 * 2.541875;
        QVERIFY(qAbs(energyKcal(s, ms(THU, 0), ms(THU, 12), p, 60) - (1648.75 / 2 + active)) < 1e-6);
        QVERIFY(qAbs(energyKcal(s, ms(THU, 0), ms(THU, 9, 10), p, 60) - (1648.75 * (9.0 + 10 / 60.0) / 24 + 10 * 2.541875)) < 1e-6);
        QCOMPARE(energyKcal(s, ms(THU, 12), ms(THU, 12), p, 60), 0.0);
    }

    // ---- zones and Active Zone Minutes ----
    void zonesByHeartRateReserve()
    {
        // 30 years: maximum 190, resting 60, reserve 130: Moderate from 40 % (112), Vigorous from 60 % (138),
        // Peak from 85 % (171)
        QCOMPARE(zoneStartBpm(Zone::Light, 190, 60), 60);
        QCOMPARE(zoneStartBpm(Zone::Moderate, 190, 60), 112);
        QCOMPARE(zoneStartBpm(Zone::Vigorous, 190, 60), 138);
        QCOMPARE(zoneStartBpm(Zone::Peak, 190, 60), 171);
        QVERIFY(zoneForBpm(111, 190, 60) == Zone::Light);
        QVERIFY(zoneForBpm(112, 190, 60) == Zone::Moderate);
        QVERIFY(zoneForBpm(137, 190, 60) == Zone::Moderate);
        QVERIFY(zoneForBpm(138, 190, 60) == Zone::Vigorous);
        QVERIFY(zoneForBpm(170, 190, 60) == Zone::Vigorous);
        QVERIFY(zoneForBpm(171, 190, 60) == Zone::Peak);
        QVERIFY(zoneForBpm(40, 190, 60) == Zone::Light); // below resting
        QCOMPARE(azmForZone(Zone::Light), 0);
        QCOMPARE(azmForZone(Zone::Moderate), 1);
        QCOMPARE(azmForZone(Zone::Vigorous), 2);
        QCOMPARE(azmForZone(Zone::Peak), 2);
        QCOMPARE(AZM_WEEKLY_GOAL, 150);
        QCOMPARE(HOURLY_STEPS, 250);
    }

    void activeZoneMinutesCountOneAndTwo()
    {
        MinuteSeries s;
        fill(s, THU, 7, 0, 20, 0, 90);   // light: 0
        fill(s, THU, 7, 20, 10, 0, 120); // moderate: 10 x 1
        fill(s, THU, 7, 30, 5, 0, 150);  // vigorous: 5 x 2
        fill(s, THU, 7, 35, 3, 0, 180);  // peak: 3 x 2
        const ZoneMinutes z = zoneMinutes(s, minuteAt(THU, 0), minuteAt(THU, 23), profile(), 60);
        QCOMPARE(z.light, 20);
        QCOMPARE(z.moderate, 10);
        QCOMPARE(z.vigorous, 5);
        QCOMPARE(z.peak, 3);
        QCOMPARE(z.azm, 10 + 10 + 6);
        // only the range asked for
        QCOMPARE(zoneMinutes(s, minuteAt(THU, 7, 20), minuteAt(THU, 7, 30), profile(), 60).azm, 10);
        // minutes without a heart rate are no zone minutes
        MinuteSeries steps;
        fill(steps, THU, 8, 0, 10, 100, 0);
        QCOMPARE(zoneMinutes(steps, 0, minuteAt(THU, 23), profile(), 60).azm, 0);
    }

    // ---- the resting heart rate: one definition (metrics/restingHr.h), on invented days ----
    void restingHeartRateIsTheMedianOfTheRestingMinutes()
    {
        const qint64 day0 = ms(THU, 0), day1 = ms(THU.addDays(1), 0);
        MinuteSeries s;
        fill(s, THU, 1, 0, 100, 0, 58);
        QCOMPARE(restingHrOfMinutes(s, day0, day1), 58);
        // fewer than an hour of resting minutes: none
        MinuteSeries few;
        fill(few, THU, 1, 0, 59, 0, 58);
        QCOMPARE(restingHrOfMinutes(few, day0, day1), 0);
        fill(few, THU, 2, 0, 1, 0, 70);
        QCOMPARE(restingHrOfMinutes(few, day0, day1), 58); // 60 minutes: the median of 59 x 58 and one 70
        // minutes without a heart rate (not worn, on the charger: the hub pauses it) are not resting minutes
        MinuteSeries off;
        fill(off, THU, 1, 0, 59, 0, 50);
        fill(off, THU, 2, 0, 100, 0, 0);
        QCOMPARE(restingHrOfMinutes(off, day0, day1), 0);
        // only the window counts
        QCOMPARE(restingHrOfMinutes(s, ms(THU, 2), day1), 0);
        // within 30-120
        MinuteSeries low, high;
        fill(low, THU, 1, 0, 100, 0, 25);
        fill(high, THU, 1, 0, 100, 0, 150);
        QCOMPARE(restingHrOfMinutes(low, day0, day1), 30);
        QCOMPARE(restingHrOfMinutes(high, day0, day1), 120);
    }

    void aStepAndTheTenMinutesAfterItAreNotResting()
    {
        const qint64 day0 = ms(THU, 0), day1 = ms(THU.addDays(1), 0);
        MinuteSeries s;
        fill(s, THU, 8, 0, 60, 0, 50);     // still
        fill(s, THU, 9, 0, 30, 100, 120);  // a walk
        fill(s, THU, 9, 30, 70, 0, 80);    // seated after it: the first 10 minutes are not resting yet
        // 60 x 50 and 60 x 80: the median 65 (with the 10 minutes it would be 70 x 80 and the median 80)
        QCOMPARE(restingHrOfMinutes(s, day0, day1), 65);
        // a step in the 10 minutes before the window's first minute counts too
        MinuteSeries edge;
        fill(edge, THU, 7, 55, 1, 20, 0);
        fill(edge, THU, 8, 0, 70, 0, 60);
        QCOMPARE(restingHrOfMinutes(edge, ms(THU, 8), day1), 60); // 64 resting minutes (8:06 on)
        MinuteSeries edge2;
        fill(edge2, THU, 7, 55, 1, 20, 0);
        fill(edge2, THU, 8, 0, 65, 0, 60);
        QCOMPARE(restingHrOfMinutes(edge2, ms(THU, 8), day1), 0); // 59 resting minutes
    }

    void aWorkoutIsNotResting()
    {
        const qint64 day0 = ms(THU, 0), day1 = ms(THU.addDays(1), 0);
        MinuteSeries s;
        fill(s, THU, 6, 0, 61, 0, 60);
        fill(s, THU, 18, 0, 100, 0, 110); // a ride: no steps, a high heart rate
        QCOMPARE(restingHrOfMinutes(s, day0, day1), 110); // were it resting
        QCOMPARE(restingHrOfMinutes(s, day0, day1, {{ms(THU, 18), ms(THU, 19, 40)}}), 60);
    }

    void aUsualDaysRestingHeartRate()
    {
        // asleep to 7:00 (52-56), a walk, a desk morning (68), a run at noon, a desk afternoon (68), an evening
        // (64), asleep from 22:00 (54): 1360 resting minutes, the median in the evening's
        MinuteSeries s;
        for (int i = 0; i < 420; i++)
            fill(s, THU, i / 60, i % 60, 1, 0, 52 + i % 5);
        fill(s, THU, 7, 0, 30, 90, 100);
        fill(s, THU, 7, 30, 270, 0, 68);
        fill(s, THU, 12, 0, 30, 160, 150);
        fill(s, THU, 12, 30, 330, 0, 68);
        fill(s, THU, 18, 0, 240, 0, 64);
        fill(s, THU, 22, 0, 120, 0, 54);
        QCOMPARE(restingHrOfMinutes(s, ms(THU, 0), ms(THU.addDays(1), 0)), 64);
        // the night alone (the 24 hours up to its end hold only it here)
        QCOMPARE(restingHrOfMinutes(s, ms(THU, 0), ms(THU, 7)), 54);
    }

    void theWindowIsTheNightsOrTheDays()
    {
        const QPair<qint64, qint64> day = restingWindow(THU, 0);
        QCOMPARE(day.first, ms(THU, 0));
        QCOMPARE(day.second, ms(THU.addDays(1), 0));
        const QPair<qint64, qint64> night = restingWindow(THU, ms(THU, 6, 40));
        QCOMPARE(night.first, ms(THU, 6, 40) - 24LL * 3600 * 1000);
        QCOMPARE(night.second, ms(THU, 6, 40));
    }

    void theRestingRateInForce()
    {
        HealthProfile p = profile();
        QCOMPARE(effectiveRestingHr(p, 55), 55);
        QCOMPARE(effectiveRestingHr(p, 0), 60);
        p.restingHr = 48;
        QCOMPARE(effectiveRestingHr(p, 55), 48); // the profile's own wins
    }

    // ---- the alert thresholds: stock's defaults, the user's own first ----
    void theAlertDefaultsAreStocks()
    {
        QCOMPARE(DEFAULT_HIGH_BPM, 120);
        QCOMPARE(DEFAULT_LOW_BPM, 50);
        HrAlertThresholds t;
        QCOMPARE(t.highBpm(), 120);
        QCOMPARE(t.lowBpm(), 50);
        t.high = 135;
        t.low = 45;
        QCOMPARE(t.highBpm(), 135); // a value the user set stays in force
        QCOMPARE(t.lowBpm(), 45);
    }

    // ---- steps from the day file ----
    void stepRecordsAreSpreadOverTheirInterval()
    {
        MinuteSeries s;
        const qint64 t1 = ms(THU, 10, 9) / 1000, t2 = ms(THU, 10, 19, 30) / 1000;
        s.addStepRecords({{double(t1), 100}, {double(t2), 160}}, 600000);
        // the first record is from its interval, 10:00-10:09: 10 a minute
        for (int m = 0; m < 10; m++)
            QCOMPARE(s.at(minuteAt(THU, 10, m)).steps, 10);
        // the second from 10:10 (its interval's start) to 10:19: 60 steps, 6 a minute
        for (int m = 10; m < 20; m++)
            QCOMPARE(s.at(minuteAt(THU, 10, m)).steps, 6);
        QVERIFY(!s.has(minuteAt(THU, 10, 20)));
        QCOMPARE(s.stepsBetween(ms(THU, 10), ms(THU, 11) - 1), 160);
    }

    void aRemainderGoesToTheLastMinutes()
    {
        MinuteSeries s;
        s.addStepRecords({{double(ms(THU, 10, 9) / 1000), 25}}, 600000);
        int sum = 0;
        for (int m = 0; m < 10; m++) {
            const int v = s.at(minuteAt(THU, 10, m)).steps;
            QCOMPARE(v, m < 5 ? 2 : 3);
            sum += v;
        }
        QCOMPARE(sum, 25);
    }

    void aRecordAfterAGapKeepsToItsOwnInterval()
    {
        // 4 hours without steps (the hub batch only brought the end of the gap): the 300 steps are in the
        // interval of the record, not spread over the gap
        MinuteSeries s;
        s.addStepRecords({{double(ms(THU, 6, 0) / 1000), 500}, {double(ms(THU, 10, 5) / 1000), 800}}, 600000);
        QCOMPARE(s.stepsBetween(ms(THU, 6, 1), ms(THU, 9, 59)), 0);
        QCOMPARE(s.stepsBetween(ms(THU, 10, 0), ms(THU, 10, 9)), 300);
    }

    void aCounterThatStartedAgainIsNoNegativeSteps()
    {
        MinuteSeries s;
        s.addStepRecords({{double(ms(THU, 9, 9) / 1000), 500}, {double(ms(THU, 9, 19) / 1000), 30},
                          {double(ms(THU, 9, 29) / 1000), 80}}, 600000);
        QCOMPARE(s.stepsBetween(ms(THU, 9), ms(THU, 9, 59)), 550); // 500, then 0 (restart), then 50
    }

    void liveMinutesAddUp()
    {
        MinuteSeries s;
        s.addSteps(ms(THU, 10, 0, 5), 10);
        s.addSteps(ms(THU, 10, 0, 50), 5);
        s.addSteps(ms(THU, 10, 1, 0), 7);
        s.addHr(ms(THU, 10, 0, 10), 60);
        s.addHr(ms(THU, 10, 0, 40), 70);
        s.addHr(ms(THU, 10, 0, 40), -1); // ignored
        QCOMPARE(s.at(minuteAt(THU, 10)).steps, 15);
        QCOMPARE(s.at(minuteAt(THU, 10)).hr, 65.0);
        QCOMPARE(s.at(minuteAt(THU, 10)).hrCount, 2);
        QCOMPARE(s.at(minuteAt(THU, 10, 1)).steps, 7);
        s.trimBefore(minuteAt(THU, 10, 1));
        QVERIFY(!s.has(minuteAt(THU, 10)));
        QVERIFY(s.has(minuteAt(THU, 10, 1)));
    }

    // ---- floors ----
    void floorsFromTheBarometerAndTheSteps()
    {
        MinuteSeries steps;
        fill(steps, THU, 9, 0, 20, 30, 0);
        const double h0 = ms(THU, 9, 0) / 1000.0;
        // 1000 -> 998.8 hPa is a climb of about 10 m: 3 floors of 3.048 m
        QCOMPARE(floorsClimbed({{h0, 100000}, {h0 + 600, 99880}}, steps), 3);
        QCOMPARE(floorsClimbed({{h0, 1000.0}, {h0 + 600, 998.8}}, steps), 3); // in hPa too
        QVERIFY(altitudeDifferenceM(1000, 998.8) > 9.5 && altitudeDifferenceM(1000, 998.8) < 10.5);
        QVERIFY(altitudeDifferenceM(998.8, 1000) < 0);
        // down: no floors; weather (0.1 hPa, under 1.5 m): none
        QCOMPARE(floorsClimbed({{h0, 99880}, {h0 + 600, 100000}}, steps), 0);
        QCOMPARE(floorsClimbed({{h0, 100000}, {h0 + 600, 99990}}, steps), 0);
        // no steps between the readings (a lift, a hill by car): none
        MinuteSeries still;
        QCOMPARE(floorsClimbed({{h0, 100000}, {h0 + 600, 99880}}, still), 0);
        // two climbs add up; a gap over 40 minutes is not looked at
        QCOMPARE(floorsClimbed({{h0, 100000}, {h0 + 600, 99940}, {h0 + 1200, 99880}}, steps), 3);
        QCOMPARE(floorsClimbed({{h0, 100000}, {h0 + 3000, 99880}}, steps), 0);
        QCOMPARE(floorsClimbed({{h0, 0}, {h0 + 600, 99880}}, steps), 0); // a reading of 0: not a pressure
        QCOMPARE(floorsClimbed({}, steps), 0);
    }

    // a record a minute, as the barometer is recorded since the sensor hub batches it (WO-05)
    void floorsFromMinuteRecords()
    {
        MinuteSeries steps;
        fill(steps, THU, 9, 0, 60, 80, 0);
        const double h0 = ms(THU, 9, 0) / 1000.0;
        // pressure for an altitude (m) above 1013.25 hPa, in Pa
        auto pa = [](double m) { return 101325.0 * std::pow(1.0 - m / 44330.0, 5.255); };
        auto series = [&](const QList<double> &metres) {
            QList<QPointF> p;
            for (int i = 0; i < metres.size(); i++)
                p << QPointF(h0 + 60.0 * i, pa(metres[i]));
            return p;
        };
        // two floors climbed across three minutes (1.4 m, then 2.0 m, then 2.8 m): counted in full
        QCOMPARE(floorsClimbed(series({0, 1.4, 3.4, 6.2, 6.2}), steps), 2);
        // a ramp of 0.8 m a minute for ten minutes: 8 m, two floors (no minute alone is 1.5 m)
        QList<double> ramp;
        for (int i = 0; i <= 10; i++)
            ramp << 0.8 * i;
        QCOMPARE(floorsClimbed(series(ramp), steps), 2);
        // up three floors and down again, twice: six
        QCOMPARE(floorsClimbed(series({0, 9.5, 0, 9.5, 0}), steps), 6);
        // the sensor's noise on the flat for an hour (+-0.3 m): none
        QList<double> flat;
        for (int i = 0; i < 60; i++)
            flat << ((i * 7919) % 13 - 6) * 0.05;
        QCOMPARE(floorsClimbed(series(flat), steps), 0);
        // a lift: 12 m in two minutes without steps, then walking on: none
        MinuteSeries lift;
        fill(lift, THU, 9, 3, 10, 80, 0);
        QCOMPARE(floorsClimbed(series({0, 6, 12, 12, 12.2, 12.1}), lift), 0);
    }

    // ---- hourly activity ----
    void hourlyActivityCountsHoursOf250Steps()
    {
        MinuteSeries s;
        fill(s, THU, 9, 0, 25, 10, 0);   // 250: met
        fill(s, THU, 10, 0, 49, 5, 0);   // 245: not met
        fill(s, THU, 11, 0, 60, 5, 0);   // 300: met
        fill(s, THU, 17, 55, 5, 50, 0);  // 250 in the last hour of the window: met
        fill(s, THU, 18, 0, 60, 10, 0);  // after the window: shown, not counted
        fill(s, THU, 8, 0, 60, 10, 0);   // before it
        const HourlyActivity h = hourlyActivity(s, THU);
        QCOMPARE(h.hoursTotal, 9); // 9:00-18:00
        QCOMPARE(h.hoursMet, 3);
        QCOMPARE(h.hourSteps[9], 250);
        QCOMPARE(h.hourSteps[10], 245);
        QCOMPARE(h.hourSteps[17], 250);
        QCOMPARE(h.hourSteps[18], 600);
        QCOMPARE(h.hourSteps[8], 600);
        const HourlyActivity w = hourlyActivity(s, THU, 8, 12, 300);
        QCOMPARE(w.hoursTotal, 4);
        QCOMPARE(w.hoursMet, 2); // hours 8 (600) and 11 (300)
    }

    // ---- high and low heart rate alerts ----
    void aTenMinuteRunAboveTheHighThresholdAlertsOnce()
    {
        MinuteSeries s;
        fill(s, THU, 14, 0, 12, 0, 130);
        HrAlertDetector d;
        // 9 minutes in: nothing yet; the 10th minute completes the run
        QVERIFY(d.feed(s, ms(THU, 14, 9, 30), 125, true, 40, true).isEmpty());
        const QList<HrAlert> a = d.feed(s, ms(THU, 14, 12, 10), 125, true, 40, true);
        QCOMPARE(a.size(), 1);
        QVERIFY(a[0].kind == HrAlert::High);
        QCOMPARE(a[0].bpm, 130);
        QCOMPARE(a[0].threshold, 125);
        QCOMPARE(a[0].startMs, ms(THU, 14, 0));
        QCOMPARE(a[0].endMs, ms(THU, 14, 10)); // the end of the tenth minute
        // the run goes on: no second alert
        fill(s, THU, 14, 12, 20, 0, 130);
        QVERIFY(d.feed(s, ms(THU, 14, 40), 125, true, 40, true).isEmpty());
    }

    void aLowRunAlertsAsLow()
    {
        MinuteSeries s;
        fill(s, THU, 3, 0, 10, 0, 38);
        HrAlertDetector d;
        const QList<HrAlert> a = d.feed(s, ms(THU, 3, 10, 5), 125, true, 40, true);
        QCOMPARE(a.size(), 1);
        QVERIFY(a[0].kind == HrAlert::Low);
        QCOMPARE(a[0].bpm, 38);
        QCOMPARE(a[0].threshold, 40);
    }

    void aRunBrokenByActivityOrANormalMinuteDoesNotAlert()
    {
        MinuteSeries s;
        fill(s, THU, 14, 0, 5, 0, 130);
        fill(s, THU, 14, 5, 1, 20, 130); // 20 steps: not inactive
        fill(s, THU, 14, 6, 5, 0, 130);
        HrAlertDetector d;
        QVERIFY(d.feed(s, ms(THU, 14, 12), 125, true, 40, true).isEmpty());
        // 9 minutes, one at the threshold itself (not above), 9 more: still none
        MinuteSeries t;
        fill(t, THU, 15, 0, 9, 0, 130);
        fill(t, THU, 15, 9, 1, 0, 125);
        fill(t, THU, 15, 10, 9, 0, 130);
        HrAlertDetector e;
        QVERIFY(e.feed(t, ms(THU, 15, 20), 125, true, 40, true).isEmpty());
        // a minute without a heart rate breaks a run too
        MinuteSeries u;
        fill(u, THU, 16, 0, 5, 0, 130);
        fill(u, THU, 16, 6, 5, 0, 130);
        HrAlertDetector f;
        QVERIFY(f.feed(u, ms(THU, 16, 12), 125, true, 40, true).isEmpty());
    }

    void aNewRunAfterABreakAlertsAgain()
    {
        MinuteSeries s;
        fill(s, THU, 14, 0, 10, 0, 130);
        fill(s, THU, 14, 10, 2, 0, 90);
        fill(s, THU, 14, 12, 10, 0, 140);
        HrAlertDetector d;
        const QList<HrAlert> a = d.feed(s, ms(THU, 14, 30), 125, true, 40, true);
        QCOMPARE(a.size(), 2);
        QCOMPARE(a[0].bpm, 130);
        QCOMPARE(a[1].bpm, 140);
        QCOMPARE(a[1].startMs, ms(THU, 14, 12));
    }

    void disabledThresholdsAndTheCurrentMinute()
    {
        MinuteSeries s;
        fill(s, THU, 14, 0, 10, 0, 130);
        HrAlertDetector off;
        QVERIFY(off.feed(s, ms(THU, 14, 30), 125, false, 40, true).isEmpty());
        // the minute that has not ended is not looked at: 10 minutes at 14:10:00 are complete, at 14:09:59 not
        HrAlertDetector early;
        QVERIFY(early.feed(s, ms(THU, 14, 9, 59), 125, true, 40, true).isEmpty());
        QCOMPARE(early.feed(s, ms(THU, 14, 10, 0), 125, true, 40, true).size(), 1);
        MinuteSeries lows;
        fill(lows, THU, 14, 0, 10, 0, 30);
        HrAlertDetector noLow;
        QVERIFY(noLow.feed(lows, ms(THU, 14, 30), 125, true, 40, false).isEmpty());
    }

    void aLongGapIsNotWalkedMinuteByMinute()
    {
        MinuteSeries s;
        fill(s, THU, 14, 0, 2, 0, 130);
        HrAlertDetector d;
        QVERIFY(d.feed(s, ms(THU.addDays(5), 9), 125, true, 40, true).isEmpty());
        QVERIFY(d.seenUpTo() >= minuteAt(THU.addDays(5), 9) - 1);
    }

    // ---- reminders to move ----
    void aReminderAtMinute50UnderTheHourlyGoal()
    {
        MoveReminderSettings cfg;
        cfg.enabled = true;
        MinuteSeries s;
        fill(s, THU, 10, 0, 10, 10, 0); // 100 steps in the hour
        MoveReminder r;
        QVERIFY(r.evaluate(ms(THU, 10, 49, 59), s, cfg, true).isEmpty());
        const QList<MoveEvent> e = r.evaluate(ms(THU, 10, 50), s, cfg, true);
        QCOMPARE(e.size(), 1);
        QVERIFY(e[0].kind == MoveEvent::Remind);
        QCOMPARE(e[0].steps, 100);
        QCOMPARE(e[0].stepsLeft, 150); // "150 steps left this hour"
        QVERIFY(r.evaluate(ms(THU, 10, 55), s, cfg, true).isEmpty()); // once an hour
        // the next hour reminds again
        QCOMPARE(r.evaluate(ms(THU, 11, 52), s, cfg, true).size(), 1);
    }

    void reachingTheGoalAfterAReminderIsCelebrated()
    {
        MoveReminderSettings cfg;
        cfg.enabled = true;
        MinuteSeries s;
        fill(s, THU, 10, 0, 10, 10, 0);
        MoveReminder r;
        QCOMPARE(r.evaluate(ms(THU, 10, 50), s, cfg, true).size(), 1);
        fill(s, THU, 10, 50, 5, 30, 0); // 150 more: 250 in the hour
        const QList<MoveEvent> e = r.evaluate(ms(THU, 10, 56), s, cfg, true);
        QCOMPARE(e.size(), 1);
        QVERIFY(e[0].kind == MoveEvent::HourComplete);
        QCOMPARE(e[0].steps, 250);
        QVERIFY(r.evaluate(ms(THU, 10, 58), s, cfg, true).isEmpty()); // once
        // an hour that was met without a reminder has no celebration
        fill(s, THU, 11, 0, 30, 10, 0);
        QVERIFY(r.evaluate(ms(THU, 11, 40), s, cfg, true).isEmpty());
    }

    void exactlyTheGoalIsMet()
    {
        MoveReminderSettings cfg;
        cfg.enabled = true;
        MinuteSeries s;
        fill(s, THU, 10, 0, 49, 5, 0); // 245
        MoveReminder r;
        QCOMPARE(r.evaluate(ms(THU, 10, 50), s, cfg, true)[0].stepsLeft, 5);
        MinuteSeries t;
        fill(t, THU, 10, 0, 50, 5, 0); // 250: no reminder
        MoveReminder q;
        QVERIFY(q.evaluate(ms(THU, 10, 50), t, cfg, true).isEmpty());
    }

    void noReminderOutsideTheWindowTheDaysOrWithoutTheSetting()
    {
        MinuteSeries s;
        MoveReminderSettings cfg;
        MoveReminder off;
        QVERIFY(off.evaluate(ms(THU, 10, 50), s, cfg, true).isEmpty()); // off by default, as stock
        QVERIFY(!cfg.enabled);
        cfg.enabled = true;
        QCOMPARE(cfg.startHour, 9);
        QCOMPARE(cfg.endHour, 18);
        QCOMPARE(cfg.days, 0x7f);
        MoveReminder r;
        QVERIFY(r.evaluate(ms(THU, 8, 50), s, cfg, true).isEmpty());  // before 9:00
        QVERIFY(r.evaluate(ms(THU, 18, 50), s, cfg, true).isEmpty()); // after 18:00
        QCOMPARE(r.evaluate(ms(THU, 17, 50), s, cfg, true).size(), 1); // the last hour of the window
        MoveReminder worn;
        QVERIFY(worn.evaluate(ms(THU, 10, 50), s, cfg, false).isEmpty()); // not on the wrist
        QCOMPARE(worn.evaluate(ms(THU, 10, 51), s, cfg, true).size(), 1);  // then it is: still the same hour
        cfg.days = 1 << 0; // Mondays only
        MoveReminder days;
        QVERIFY(days.evaluate(ms(THU, 10, 50), s, cfg, true).isEmpty());
        QCOMPARE(days.evaluate(ms(QDate(2026, 11, 9), 10, 50), s, cfg, true).size(), 1);
        cfg.days = 0x7f;
        cfg.startHour = 13;
        cfg.endHour = 12; // an empty window
        MoveReminder bad;
        QVERIFY(bad.evaluate(ms(THU, 13, 50), s, cfg, true).isEmpty());
    }

    void everyHourMetCelebratesTheDayOnce()
    {
        MoveReminderSettings cfg;
        cfg.enabled = true;
        MinuteSeries s;
        for (int h = 9; h < 17; h++)
            fill(s, THU, h, 0, 25, 10, 0);
        MoveReminder r;
        QVERIFY(r.evaluate(ms(THU, 16, 59), s, cfg, true).isEmpty()); // 8 of 9: not yet
        fill(s, THU, 17, 0, 25, 10, 0);
        const QList<MoveEvent> e = r.evaluate(ms(THU, 17, 26), s, cfg, true);
        QCOMPARE(e.size(), 1);
        QVERIFY(e[0].kind == MoveEvent::DayComplete);
        QCOMPARE(e[0].hoursMet, 9); // "9 hr"
        QVERIFY(r.evaluate(ms(THU, 17, 30), s, cfg, true).isEmpty());
        // the batch came after the window: still celebrated, once
        MoveReminder late;
        QCOMPARE(late.evaluate(ms(THU, 18, 20), s, cfg, true).size(), 1);
        QVERIFY(late.evaluate(ms(THU, 18, 21), s, cfg, true).isEmpty());
    }

    void theStateSurvivesARestart()
    {
        MoveReminderSettings cfg;
        cfg.enabled = true;
        MinuteSeries s;
        MoveReminder r;
        QCOMPARE(r.evaluate(ms(THU, 10, 50), s, cfg, true).size(), 1);
        MoveReminder again;
        again.setState(r.state());
        QVERIFY(again.evaluate(ms(THU, 10, 53), s, cfg, true).isEmpty());
    }

    void theNextReminderMinuteOfTheWindow()
    {
        MoveReminderSettings cfg;
        QCOMPARE(MoveReminder::nextReminderMs(ms(THU, 10, 30), cfg), qint64(-1)); // off
        cfg.enabled = true;
        QCOMPARE(MoveReminder::nextReminderMs(ms(THU, 10, 30), cfg), ms(THU, 10, 50));
        QCOMPARE(MoveReminder::nextReminderMs(ms(THU, 10, 50), cfg), ms(THU, 11, 50)); // strictly after
        QCOMPARE(MoveReminder::nextReminderMs(ms(THU, 5, 0), cfg), ms(THU, 9, 50));
        QCOMPARE(MoveReminder::nextReminderMs(ms(THU, 17, 55), cfg), ms(THU.addDays(1), 9, 50));
        cfg.days = 1 << 0; // Mondays only: from Thursday the 12th the next is Monday the 16th
        QCOMPARE(MoveReminder::nextReminderMs(ms(THU, 10, 0), cfg), ms(QDate(2026, 11, 16), 9, 50));
        cfg.days = 0;
        QCOMPARE(MoveReminder::nextReminderMs(ms(THU, 10, 0), cfg), qint64(-1));
    }

    // ---- sleep ----
    void aNightIsEstimatedFromStepsAndHeartRate()
    {
        const QDate wake = THU;
        const QDate eve = wake.addDays(-1);
        MinuteSeries s;
        fill(s, eve, 19, 0, 120, 0, 100); // an evening at the desk: heart rate too high to count
        fill(s, eve, 21, 0, 90, 12, 95);  // walking home
        // the night: 22:30 to 06:30 (480 minutes), heart rate 52..64 in a slow wave, resting 56
        for (int i = 0; i < 480; i++) {
            const qint64 t = ms(eve, 22, 30) + (qint64)i * 60000;
            s.addHr(t, 58 + (int)std::lround(6 * std::sin(i / 40.0)));
        }
        // awake at 02:10 for 4 minutes (steps), the heart rate going on
        for (int i = 0; i < 4; i++)
            s.addSteps(ms(wake, 2, 10) + (qint64)i * 60000, 25);
        fill(s, wake, 6, 30, 90, 20, 85); // up
        // a nap at 14:00-14:45, heart rate near resting
        fill(s, wake, 14, 0, 45, 0, 58);

        const SleepNight n = estimateSleep(s, wake, profile(), 56);
        QVERIFY(n.valid);
        QCOMPARE(n.main.startMs, ms(eve, 22, 30));
        QCOMPARE(n.main.endMs, ms(wake, 6, 30));
        QCOMPARE(n.main.asleepMin + n.main.awakeMin, 480);
        QCOMPARE(n.main.awakeMin, 4); // the minutes with steps are awake inside the night
        QCOMPARE(n.main.asleepMin, 476);
        QCOMPARE(n.main.deepMin + n.main.remMin + n.main.lightMin, n.main.asleepMin);
        QVERIFY(n.main.deepMin > 0.15 * n.main.asleepMin && n.main.deepMin < 0.25 * n.main.asleepMin);
        QVERIFY(n.main.remMin > 0 && n.main.remMin < 0.25 * n.main.asleepMin);
        QVERIFY(n.main.lightMin > n.main.deepMin);
        QVERIFY(!n.goalMet); // 476 of 480 minutes
        QCOMPARE(n.goalMin, 480);
        QVERIFY(n.score >= 0 && n.score <= 100);
        QCOMPARE(n.scoreLabel, sleepScoreLabel(n.score));
        QCOMPARE(n.naps.size(), 1);
        QCOMPARE(n.naps[0].startMs, ms(wake, 14, 0));
        QCOMPARE(n.napMin, 45);
        QCOMPARE(n.wakeDate, wake);
        // the evening at a heart rate of 100 is no sleep: the night starts at 22:30
        QVERIFY(n.main.startMs >= ms(eve, 22, 30));
    }

    void theGoalIsMetByAFullNight()
    {
        MinuteSeries s;
        for (int i = 0; i < 500; i++)
            s.addHr(ms(THU.addDays(-1), 22, 0) + (qint64)i * 60000, 55);
        const SleepNight n = estimateSleep(s, THU, profile(), 56);
        QVERIFY(n.valid);
        QCOMPARE(n.main.asleepMin, 500);
        QVERIFY(n.goalMet);
        // the duration is a full 50 points, a flat heart rate at resting gives the 25 for it
        QVERIFY(n.score >= 75);
        QCOMPARE(estimateSleep(s, THU, profile(), 56, 600).goalMet, false); // against another goal
    }

    void awakeTimeInTheNightIsBridgedOnlyWhenShort()
    {
        MinuteSeries s;
        const QDate eve = THU.addDays(-1);
        // 23:00-01:00 asleep, 25 minutes up (a gap over 15 minutes), 01:25-06:00 asleep
        for (int i = 0; i < 120; i++)
            s.addHr(ms(eve, 23, 0) + (qint64)i * 60000, 55);
        fill(s, THU, 1, 0, 25, 15, 80);
        for (int i = 0; i < 275; i++)
            s.addHr(ms(THU, 1, 25) + (qint64)i * 60000, 55);
        const SleepNight n = estimateSleep(s, THU, profile(), 56);
        QVERIFY(n.valid);
        QCOMPARE(n.main.startMs, ms(THU, 1, 25)); // the longer part is the night, the first is a session of its own
        QCOMPARE(n.main.asleepMin, 275);
        QCOMPARE(n.naps.size(), 0); // the part before the awake spell is a session of the night, not a nap
    }

    void noWornWatchIsNoSleep()
    {
        MinuteSeries steps; // steps only, no heart rate: the watch was off the wrist
        fill(steps, THU, 7, 0, 30, 20, 0);
        QVERIFY(!estimateSleep(steps, THU, profile(), 56).valid);
        QVERIFY(!estimateSleep(MinuteSeries(), THU, profile(), 56).valid);
        // 45 minutes is no night
        MinuteSeries shortOne;
        for (int i = 0; i < 45; i++)
            shortOne.addHr(ms(THU.addDays(-1), 23, 0) + (qint64)i * 60000, 55);
        QVERIFY(!estimateSleep(shortOne, THU, profile(), 56).valid);
    }

    void sleepScoreBands()
    {
        QCOMPARE(sleepScoreLabel(100), QString("Excellent"));
        QCOMPARE(sleepScoreLabel(90), QString("Excellent"));
        QCOMPARE(sleepScoreLabel(89), QString("Good"));
        QCOMPARE(sleepScoreLabel(80), QString("Good"));
        QCOMPARE(sleepScoreLabel(79), QString("Fair"));
        QCOMPARE(sleepScoreLabel(60), QString("Fair"));
        QCOMPARE(sleepScoreLabel(59), QString("Poor"));
        QCOMPARE(sleepScoreLabel(0), QString("Poor"));
    }

    void aNightRoundTripsThroughJson()
    {
        MinuteSeries s;
        for (int i = 0; i < 400; i++)
            s.addHr(ms(THU.addDays(-1), 23, 0) + (qint64)i * 60000, 55 + (i % 7));
        fill(s, THU, 14, 0, 40, 0, 56);
        const SleepNight n = estimateSleep(s, THU, profile(), 56);
        QVERIFY(n.valid);
        const SleepNight back = SleepNight::fromJson(n.toJson());
        QCOMPARE(back.valid, n.valid);
        QCOMPARE(back.main.startMs, n.main.startMs);
        QCOMPARE(back.main.endMs, n.main.endMs);
        QCOMPARE(back.main.deepMin, n.main.deepMin);
        QCOMPARE(back.main.remMin, n.main.remMin);
        QCOMPARE(back.main.lightMin, n.main.lightMin);
        QCOMPARE(back.score, n.score);
        QCOMPARE(back.scoreLabel, n.scoreLabel);
        QCOMPARE(back.naps.size(), n.naps.size());
        QCOMPARE(back.wakeDate, n.wakeDate);
        const QVariantMap m = n.toMap();
        const QVariantMap main = m["main"].toMap();
        QCOMPARE(main["durationMin"].toInt(), n.main.asleepMin + n.main.awakeMin);
        QCOMPARE(main["deepPct"].toInt() + main["remPct"].toInt() + main["lightPct"].toInt() + main["awakePct"].toInt() >= 99, true);
        QCOMPARE(m["goalMin"].toInt(), 480);
    }

    // ---- auto-detected exercise ----
    void tenMinutesOfWalkingOffersToStartTracking()
    {
        MinuteSeries s;
        fill(s, THU, 10, 0, 10, 100, 0);
        AutoExercise a;
        AutoExerciseSettings cfg;
        QVERIFY(a.feed(s, ms(THU, 10, 9, 30), cfg, false).isEmpty()); // 9 minutes
        const QList<ExerciseOffer> o = a.feed(s, ms(THU, 10, 10, 5), cfg, false);
        QCOMPARE(o.size(), 1);
        QVERIFY(o[0].kind == ExerciseOffer::Start);
        QCOMPARE(o[0].activity, QString("walking"));
        QCOMPARE(o[0].sinceMs, ms(THU, 10, 0));
        // the walk goes on: one offer
        fill(s, THU, 10, 10, 30, 100, 0);
        QVERIFY(a.feed(s, ms(THU, 10, 45), cfg, false).isEmpty());
    }

    void runningIsToldFromWalkingByTheCadence()
    {
        MinuteSeries s;
        fill(s, THU, 10, 0, 10, 160, 0);
        AutoExercise a;
        const QList<ExerciseOffer> o = a.feed(s, ms(THU, 10, 11), AutoExerciseSettings(), false);
        QCOMPARE(o.size(), 1);
        QCOMPARE(o[0].activity, QString("running"));
    }

    void aSlowStrollOrABrokenWalkIsNoActivity()
    {
        MinuteSeries slow;
        fill(slow, THU, 10, 0, 30, 60, 0); // 60 a minute: under the walking cadence
        AutoExercise a;
        QVERIFY(a.feed(slow, ms(THU, 10, 31), AutoExerciseSettings(), false).isEmpty());
        MinuteSeries broken;
        fill(broken, THU, 11, 0, 5, 100, 0);
        fill(broken, THU, 11, 6, 5, 100, 0); // a minute without steps between
        AutoExercise b;
        QVERIFY(b.feed(broken, ms(THU, 11, 12), AutoExerciseSettings(), false).isEmpty());
        // 10 minutes at 75 a minute: each minute counts but the mean is under 90
        MinuteSeries mean;
        fill(mean, THU, 12, 0, 10, 75, 0);
        AutoExercise c;
        QVERIFY(c.feed(mean, ms(THU, 12, 11), AutoExerciseSettings(), false).isEmpty());
    }

    void anotherWalkNeedsAQuietSpellFirst()
    {
        MinuteSeries s;
        fill(s, THU, 10, 0, 10, 100, 0);
        AutoExercise a;
        AutoExerciseSettings cfg;
        QCOMPARE(a.feed(s, ms(THU, 10, 11), cfg, false).size(), 1);
        fill(s, THU, 10, 10, 3, 0, 0);   // only 3 quiet minutes
        fill(s, THU, 10, 13, 10, 100, 0);
        QVERIFY(a.feed(s, ms(THU, 10, 24), cfg, false).isEmpty());
        fill(s, THU, 10, 23, 6, 0, 0);   // 6 quiet minutes: armed again
        fill(s, THU, 10, 29, 10, 100, 0);
        const QList<ExerciseOffer> o = a.feed(s, ms(THU, 10, 40), cfg, false);
        QCOMPARE(o.size(), 1);
        QCOMPARE(o[0].sinceMs, ms(THU, 10, 29));
    }

    void autoStartOffSilencesTheStartPrompt()
    {
        MinuteSeries s;
        fill(s, THU, 10, 0, 12, 100, 0);
        AutoExercise a;
        AutoExerciseSettings cfg;
        cfg.start = false;
        QVERIFY(a.feed(s, ms(THU, 10, 12), cfg, false).isEmpty());
    }

    void finishedWorkingOutAfterAStopInActivity()
    {
        MinuteSeries s;
        fill(s, THU, 10, 0, 20, 100, 0);
        fill(s, THU, 10, 20, 5, 0, 0);
        AutoExercise a;
        AutoExerciseSettings cfg;
        // a workout going: no start prompt; the stop after 5 quiet minutes
        QVERIFY(a.feed(s, ms(THU, 10, 24), cfg, true).isEmpty());
        const QList<ExerciseOffer> o = a.feed(s, ms(THU, 10, 25, 1), cfg, true);
        QCOMPARE(o.size(), 1);
        QVERIFY(o[0].kind == ExerciseOffer::Stop);
        QCOMPARE(o[0].sinceMs, ms(THU, 10, 20));
        QVERIFY(a.feed(s, ms(THU, 10, 40), cfg, true).isEmpty()); // once per quiet spell
        // moving again and stopping again: another prompt
        fill(s, THU, 10, 40, 5, 100, 0);
        QVERIFY(a.feed(s, ms(THU, 10, 45), cfg, true).isEmpty());
        QCOMPARE(a.feed(s, ms(THU, 10, 50, 1), cfg, true).size(), 1);
        // auto-stop off: none
        MinuteSeries t;
        fill(t, THU, 11, 0, 20, 100, 0);
        fill(t, THU, 11, 20, 8, 0, 0);
        AutoExercise b;
        AutoExerciseSettings noStop;
        noStop.stop = false;
        QVERIFY(b.feed(t, ms(THU, 11, 30), noStop, true).isEmpty());
    }

    void noStopPromptWithoutAWorkout()
    {
        MinuteSeries s;
        fill(s, THU, 10, 0, 20, 100, 0);
        fill(s, THU, 10, 20, 8, 0, 0);
        AutoExercise a;
        const QList<ExerciseOffer> o = a.feed(s, ms(THU, 10, 29), AutoExerciseSettings(), false);
        for (const ExerciseOffer &x : o)
            QVERIFY(x.kind != ExerciseOffer::Stop);
    }
};

SENSORLOGD_TEST_MAIN(TestHealth)
#include "tst_health.moc"
