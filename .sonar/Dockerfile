# Image for .sonar/coverage.sh: sonarqube/scan.sh builds it and runs the script in it as the scanning user.
# A native x86_64 build of the daemon and the QML plugin's data code against Debian's Qt 6 (QtSensors included; the
# AsteroidOS additions to QtSensors, QHrmSensor and QStepCounterSensor, and mlite's MGConfItem come from
# tests/compat), gcc --coverage, gcovr, and a D-Bus daemon for dbus-run-session (the Logger and the data loaders
# talk D-Bus). The base image is pinned; the packages are trixie's.
FROM debian:trixie-20261005-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        g++ make cmake gcovr \
        qt6-base-dev qt6-sensors-dev libsqlite3-dev pkgconf \
        dbus-daemon dbus-bin dbus-session-bus-common \
    && rm -rf /var/lib/apt/lists/*

# dbus-daemon refuses to start for a uid without a passwd entry: coverage.sh adds the scanning user's
RUN chmod a+w /etc/passwd
