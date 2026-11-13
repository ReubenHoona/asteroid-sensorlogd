/*
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HEALTHNOTIFIER_H
#define HEALTHNOTIFIER_H

#include <QString>
#include <QVariantMap>

/*
 * A local notification of the Health app (the freedesktop notification service lipstick implements, as
 * asteroid-btsyncd's notifications are posted).
 */
struct HealthNotification {
    QString summary;
    QString body;
    QString icon;        // an icon name of the icon theme (asteroid-icons-ion)
    QVariantMap hints;   // x-asteroid-category, x-asteroid-health (JSON), x-nemo-preview-*, x-nemo-feedback ...
    uint replaces = 0;   // the id of the notification this one replaces (0: a new one)
};

class NotificationPoster
{
public:
    virtual ~NotificationPoster() {}
    // posts it; the id the service gave it, 0 if it could not be posted
    virtual uint post(const HealthNotification &notification) = 0;
};

class DBusNotificationPoster : public NotificationPoster
{
public:
    uint post(const HealthNotification &notification) override;
};

#endif // HEALTHNOTIFIER_H
