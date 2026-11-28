/*
 * Copyright (C) 2026 eos
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "healthLanguage.h"
#include "eoscatalogues.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QFileInfo>
#include <QTranslator>

static const char LOCALE1_PATH[] = "/org/freedesktop/locale1";
static const char LOCALE1_SERVICE[] = "org.freedesktop.locale1";
static const char LOCALE1_INTERFACE[] = "org.freedesktop.locale1";
static const char PROPERTIES_INTERFACE[] = "org.freedesktop.DBus.Properties";

QString HealthText::text(const char *id, const char *english)
{
    const QString t = qtTrId(id);
    return t == QLatin1String(id) ? QString::fromUtf8(english) : t;
}

QString HealthText::textN(const char *id, int n, const char *englishOne, const char *englishOther)
{
    const QString t = qtTrId(id, n);
    if (t != QLatin1String(id))
        return t;
    return QString::fromUtf8(n == 1 ? englishOne : englishOther).replace(QLatin1String("%n"), QString::number(n));
}

HealthLanguage::HealthLanguage(const QString &name, const QString &directory, QObject *parent)
    : QObject(parent), m_name(name), m_dir(directory), m_lang(qEnvironmentVariable("LANG")), m_bus(QString())
{
}

HealthLanguage::~HealthLanguage()
{
    remove();
}

void HealthLanguage::remove()
{
    if (m_translator) {
        QCoreApplication::removeTranslator(m_translator);
        delete m_translator;
        m_translator = nullptr;
    }
}

QString HealthLanguage::nearest(const QString &name, const QString &directory, const QLocale &locale)
{
    return EosCatalogues::nearest(name, directory, locale);
}

bool HealthLanguage::install()
{
    remove();
    // the locale's catalogue as every program of the image chooses it (eoscatalogues.h): an English region of
    // en_001 (en_NZ, en_AU...) reads en_GB's, a region without one its language's (de_AT: de_DE); the build's
    // English catalogue has one form for every count ("1 steps left"): the code's English is better
    QTranslator *t = new QTranslator(this);
    if (!EosCatalogues::load(t, m_name, m_dir, QLocale())
        || QFileInfo(t->filePath()).fileName() == m_name + QStringLiteral(".en.qm")) {
        delete t;
        return false;
    }
    QCoreApplication::installTranslator(t);
    m_translator = t;
    return true;
}

bool HealthLanguage::watch(const QDBusConnection &bus)
{
    if (!bus.isConnected())
        return false;
    m_bus = bus;
    // no sender in the match: only localed has this path, and a match on its bus-activated name would have QtDBus
    // follow its NameOwnerChanged every time localed starts and exits
    return m_bus.connect(QString(), QLatin1String(LOCALE1_PATH), QLatin1String(PROPERTIES_INTERFACE),
                         QStringLiteral("PropertiesChanged"), this,
                         SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
}

QString HealthLanguage::langOf(const QStringList &locale)
{
    for (const QString &entry : locale) {
        if (entry.startsWith(QLatin1String("LANG=")))
            return entry.mid(5).trimmed();
    }
    return QString();
}

static QStringList stringList(const QVariant &v)
{
    if (v.canConvert<QDBusArgument>())
        return qdbus_cast<QStringList>(v.value<QDBusArgument>());
    if (v.canConvert<QDBusVariant>())
        return stringList(v.value<QDBusVariant>().variant());
    return v.toStringList();
}

void HealthLanguage::propertiesChanged(const QString &interface, const QVariantMap &changed,
                                       const QStringList &invalidated)
{
    if (interface != QLatin1String(LOCALE1_INTERFACE))
        return;
    const auto it = changed.constFind(QStringLiteral("Locale"));
    if (it != changed.constEnd()) {
        apply(langOf(stringList(it.value())));
        return;
    }
    // announced without its value: asked for once
    if (invalidated.contains(QStringLiteral("Locale")))
        fetch();
}

void HealthLanguage::fetch()
{
    if (!m_bus.isConnected())
        return;
    QDBusMessage get = QDBusMessage::createMethodCall(QLatin1String(LOCALE1_SERVICE), QLatin1String(LOCALE1_PATH),
                                                      QLatin1String(PROPERTIES_INTERFACE), QStringLiteral("Get"));
    get << QLatin1String(LOCALE1_INTERFACE) << QStringLiteral("Locale");
    QDBusPendingCallWatcher *w = new QDBusPendingCallWatcher(m_bus.asyncCall(get), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        QDBusPendingReply<QDBusVariant> reply = *call;
        if (!reply.isError())
            apply(langOf(stringList(reply.value().variant())));
        call->deleteLater();
    });
}

bool HealthLanguage::apply(const QString &lang)
{
    if (lang.isEmpty() || lang == m_lang)
        return false;
    m_lang = lang;
    qputenv("LANG", lang.toUtf8());
    QLocale::setDefault(QLocale(lang));
    install();
    emit languageChanged(lang);
    return true;
}
