/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * R6 K-05: the Health notifications' language (daemon/healthLanguage.h). Without a catalogue the code's English
 * shows (with its plural), never the id; localed's Locale makes a language the daemon's, its catalogue replaces the
 * one before, a region without a catalogue reads its language's (de_AT: de_DE); the daemon's German and Russian
 * catalogues (i18n/) give stock's texts and Russian's three plural forms. The catalogue cases need TST_I18N_DIR
 * (made with lrelease: tests/CMakeLists.txt). No bus needed: localed's signal is handed to the slot.
 */

#include <QtTest>
#include <QCoreApplication>
#include <QLocale>

#include "../daemon/healthLanguage.h"

using HealthText::text;
using HealthText::textN;

class TstLanguage : public QObject
{
    Q_OBJECT

    static void say(HealthLanguage &l, const char *lang)
    {
        l.propertiesChanged(QStringLiteral("org.freedesktop.locale1"),
                            { { QStringLiteral("Locale"), QStringList{ QStringLiteral("LANG=") + QLatin1String(lang) } } },
                            {});
    }

private slots:
    void initTestCase()
    {
        qputenv("LANG", "en_GB.utf8");
        QLocale::setDefault(QLocale(QStringLiteral("en_GB")));
    }

    void englishWithoutCatalogue()
    {
        QCOMPARE(text("id-tst-none", "Plain English"), QStringLiteral("Plain English"));
        QCOMPARE(text("id-tst-none", "Good morning! Here’s how"), QString::fromUtf8("Good morning! Here’s how"));
        QCOMPARE(textN("id-tst-none-n", 1, "%n step left", "%n steps left"), QStringLiteral("1 step left"));
        QCOMPARE(textN("id-tst-none-n", 0, "%n step left", "%n steps left"), QStringLiteral("0 steps left"));
        QCOMPARE(textN("id-tst-none-n", 150, "%n step left", "%n steps left"), QStringLiteral("150 steps left"));
    }

    void langOf()
    {
        QCOMPARE(HealthLanguage::langOf({ QStringLiteral("LANG=fr_FR.utf8") }), QStringLiteral("fr_FR.utf8"));
        QCOMPARE(HealthLanguage::langOf({ QStringLiteral("LC_TIME=en_GB.utf8"), QStringLiteral("LANG=de_DE.utf8") }),
                 QStringLiteral("de_DE.utf8"));
        QCOMPARE(HealthLanguage::langOf({ QStringLiteral("LC_TIME=en_GB.utf8") }), QString());
        QCOMPARE(HealthLanguage::langOf({}), QString());
    }

    void followsTheLanguage()
    {
#ifndef TST_I18N_DIR
        QSKIP("no lrelease: catalogue cases not built");
#else
        HealthLanguage language(QStringLiteral("tst-health"), QStringLiteral(TST_I18N_DIR));
        QCOMPARE(language.language(), QStringLiteral("en_GB.utf8"));
        QVERIFY(!language.install()); // en_GB: no catalogue, the English
        QCOMPARE(text("id-tst-hello", "Hello"), QStringLiteral("Hello"));
        QSignalSpy changed(&language, &HealthLanguage::languageChanged);

        // another interface or another property: nothing
        language.propertiesChanged(QStringLiteral("org.freedesktop.timedate1"),
                                   { { QStringLiteral("Locale"), QStringList{ QStringLiteral("LANG=fr_FR.utf8") } } }, {});
        language.propertiesChanged(QStringLiteral("org.freedesktop.locale1"),
                                   { { QStringLiteral("X11Layout"), QStringLiteral("us") } }, {});
        QCOMPARE(changed.count(), 0);

        say(language, "de_DE.utf8");
        QCOMPARE(changed.count(), 1);
        QCOMPARE(language.language(), QStringLiteral("de_DE.utf8"));
        QCOMPARE(qgetenv("LANG"), QByteArray("de_DE.utf8"));
        QCOMPARE(QLocale().name(), QStringLiteral("de_DE"));
        QCOMPARE(text("id-tst-hello", "Hello"), QStringLiteral("Hallo"));
        QCOMPARE(textN("id-tst-steps", 1, "%n step", "%n steps"), QStringLiteral("1 Schritt"));
        QCOMPARE(textN("id-tst-steps", 5, "%n step", "%n steps"), QStringLiteral("5 Schritte"));
        // an id the language lacks: English
        QCOMPARE(text("id-tst-only-english", "Only English"), QStringLiteral("Only English"));

        // the same again: nothing to do
        QVERIFY(!language.apply(QStringLiteral("de_DE.utf8")));
        QVERIFY(!language.apply(QString()));
        QCOMPARE(changed.count(), 1);

        // the new language's catalogue replaces the old one
        say(language, "fr_FR.utf8");
        QCOMPARE(text("id-tst-hello", "Hello"), QStringLiteral("Bonjour"));
        QCOMPARE(textN("id-tst-steps", 0, "%n step", "%n steps"), QStringLiteral("0 pas"));
        // the phone's region without a catalogue: the language's (R5 C-04)
        QCOMPARE(HealthLanguage::nearest(QStringLiteral("tst-health"), QStringLiteral(TST_I18N_DIR),
                                         QLocale(QStringLiteral("de_AT"))),
                 QStringLiteral(TST_I18N_DIR "/tst-health.de_DE.qm"));
        say(language, "de_AT.utf8");
        QCOMPARE(text("id-tst-hello", "Hello"), QStringLiteral("Hallo"));
        // a language without a catalogue: English again, not the last one's text
        say(language, "sw_KE.utf8");
        QCOMPARE(text("id-tst-hello", "Hello"), QStringLiteral("Hello"));
        say(language, "en_GB.utf8");
        QCOMPARE(text("id-tst-hello", "Hello"), QStringLiteral("Hello"));
        QCOMPARE(changed.count(), 5);
#endif
    }

    // the daemon's catalogues: stock's own texts
    void daemonCatalogues()
    {
#ifndef TST_I18N_DIR
        QSKIP("no lrelease: catalogue cases not built");
#else
        HealthLanguage language(QStringLiteral("sensorlogd"), QStringLiteral(TST_I18N_DIR));
        say(language, "de_DE.utf8");
        QCOMPARE(text("id-health-hr-high", "High heart rate"), QStringLiteral("Hohe Herzfrequenz"));
        QCOMPARE(text("id-health-goal-complete", "Goal complete!"), QStringLiteral("Ziel erreicht!"));
        QCOMPARE(text("id-health-hr-high-detail", "x").arg(120).arg(QStringLiteral("2:00 PM")).arg(QStringLiteral("2:10 PM")),
                 QString::fromUtf8("Deine Herzfrequenz ist auf über 120 bpm angestiegen, während du zwischen "
                                   "2:00 PM und 2:10 PM inaktiv warst."));
        QCOMPARE(textN("id-health-steps-left", 1, "%n step left\nthis hour", "%n steps left\nthis hour"),
                 QString::fromUtf8("Noch 1 Schritt\nin dieser Stunde"));
        QCOMPARE(textN("id-health-steps-left", 150, "%n step left\nthis hour", "%n steps left\nthis hour"),
                 QString::fromUtf8("Noch 150 Schritte\nin dieser Stunde"));
        // a text stock has no string for: the English
        QCOMPARE(text("id-health-activity-detected", "Activity detected"), QStringLiteral("Activity detected"));

        say(language, "ru_RU.utf8");
        QCOMPARE(text("id-health-hr-high", "High heart rate"), QString::fromUtf8("Учащенный пульс"));
        // Russian: one (1, 21), few (2-4), many (5-20)
        const char *one = "%n step left\nthis hour";
        const char *other = "%n steps left\nthis hour";
        QCOMPARE(textN("id-health-steps-left", 1, one, other), QString::fromUtf8("1 шаг до цели\nна час"));
        QCOMPARE(textN("id-health-steps-left", 3, one, other), QString::fromUtf8("3 шага до цели\nна час"));
        QCOMPARE(textN("id-health-steps-left", 11, one, other), QString::fromUtf8("11 шагов до цели\nна час"));
        QCOMPARE(textN("id-health-steps-left", 21, one, other), QString::fromUtf8("21 шаг до цели\nна час"));
        say(language, "en_GB.utf8");
        QCOMPARE(text("id-health-hr-high", "High heart rate"), QStringLiteral("High heart rate"));
#endif
    }
};

QTEST_GUILESS_MAIN(TstLanguage)
#include "tst_language.moc"
