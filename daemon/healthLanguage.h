/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HEALTHLANGUAGE_H
#define HEALTHLANGUAGE_H

#include <QDBusConnection>
#include <QLocale>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QTranslator;

/*
 * The Health notifications in the watch's language (R6 K-05). Their texts are stock's own strings (the Fitbit
 * app's: heart rate alerts, the reminder to move, the goals, the morning brief) with Qt text ids; the catalogues
 * i18n/sensorlogd.<locale>.ts carry stock's translations of them (r6/k05/tools/k05match.py). The English is in the
 * code: text() and textN() give it when the catalogue installed has no text for the id, or when there is none
 * (English, a language stock has no translations for, the host tests), never the id itself.
 */
namespace HealthText {
    // the text of id in the watch's language, else english
    QString text(const char *id, const char *english);
    // a text with a count: the language's form for n (%n replaced), else englishOne (n == 1) or englishOther
    QString textN(const char *id, int n, const char *englishOne, const char *englishOther);
}

/*
 * The daemon's language. install() loads the catalogue of the default locale, <name>.<locale>.qm in directory
 * (QTranslator's own search: de_DE, then de), else the language's catalogue of another region (the phone's de_AT
 * reads de_DE: the rule of AsteroidApp and the launcher, R5 C-04); the English one build_translations makes
 * (<name>.en.qm) is not taken, the code has the English with its plural.
 * A language chosen while the daemon runs is followed (C-03, as the apps' LocaleFollower): Settings > Language, setup
 * and the phone's language set the locale through systemd-localed (org.freedesktop.locale1 SetLocale), and localed
 * announces the new Locale with org.freedesktop.DBus.Properties.PropertiesChanged on the system bus; its LANG becomes
 * the default locale and LANG of the daemon and that language's catalogue is installed. A notification already
 * posted stays as it is; the next one has the new language.
 * Battery: one D-Bus match rule; nothing runs until the language changes.
 */
class HealthLanguage : public QObject
{
    Q_OBJECT

public:
    HealthLanguage(const QString &name, const QString &directory, QObject *parent = nullptr);
    ~HealthLanguage() override;

    // the catalogue of the default locale (QLocale()) in place of the one installed before; false: none (English)
    bool install();
    // listens to localed's announcements on bus; false when the bus is not there
    bool watch(const QDBusConnection &bus);
    // the LANG of a localed Locale list ("LANG=de_DE.utf8", "LC_TIME=..."); "" when it names none
    static QString langOf(const QStringList &locale);
    // the language's catalogue of another region, "" when it has none
    static QString nearest(const QString &name, const QString &directory, const QLocale &locale);
    // the daemon's language: LANG when it started, then the last one applied
    QString language() const { return m_lang; }
    // makes lang the daemon's language; false when it is empty or already the one in use
    bool apply(const QString &lang);

signals:
    void languageChanged(const QString &lang);

public slots:
    // org.freedesktop.DBus.Properties.PropertiesChanged of /org/freedesktop/locale1
    void propertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);

private:
    void remove();
    void fetch();

    QString m_name;
    QString m_dir;
    QString m_lang;
    QDBusConnection m_bus;
    QTranslator *m_translator = nullptr;
};

#endif // HEALTHLANGUAGE_H
