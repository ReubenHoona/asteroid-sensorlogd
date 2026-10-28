/*
 * Copyright (C) 2023 Arseniy Movshev <dodoradio@outlook.com>
 * This file is part of sensorlogd, a sensor logger for the AsteroidOS smartwatch OS.
 *
 * sensorlogd is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * sensorlogd is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDebug>
#include <QSocketNotifier>
#include "logger.h"

static int quitPipe[2] = { -1, -1 };

static void onQuitSignal(int)
{
    char c = 1;
    if (write(quitPipe[1], &c, 1) < 0) {
        // nothing to do in a signal handler
    }
}

int main(int argc, char **argv)
{
    QCoreApplication qcoreapp(argc, argv);
    if (!QDBusConnection::systemBus().isConnected()) {
        fprintf(stderr, "Cannot connect to the D-Bus system bus.\n");
        return 3;
    }
    if (!QDBusConnection::sessionBus().isConnected()) {
        fprintf(stderr, "Cannot connect to the D-Bus session bus.\n");
        return 2;
    }
    QCoreApplication::setOrganizationName("asteroid");
    QCoreApplication::setOrganizationDomain("asteroidos.org");
    QCoreApplication::setApplicationName("sensorlogd");

    // systemd stops the daemon with SIGTERM: quit the event loop, so the
    // plugins write the readings they collected but did not log yet
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, quitPipe) == 0) {
        QSocketNotifier *n = new QSocketNotifier(quitPipe[0], QSocketNotifier::Read, &qcoreapp);
        QObject::connect(n, &QSocketNotifier::activated, &qcoreapp, &QCoreApplication::quit);
        struct sigaction sa = {};
        sa.sa_handler = onQuitSignal;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
    }

    Logger sensorsLogger;
    qDebug() << "sensorlogd starting";
    qcoreapp.exec();
    return 0;
}
