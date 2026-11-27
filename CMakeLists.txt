cmake_minimum_required(VERSION 3.10.0)

project(sensorlogd
    VERSION 1.0.0
    DESCRIPTION "A sensor logger daemon for AsteroidOS oriented towards health tracking"
)

find_package(ECM 6.0.0 REQUIRED NO_MODULE)
find_package(PkgConfig REQUIRED)
find_package(AsteroidApp6 REQUIRED)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${ASTEROID_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)

include(FeatureSummary)
include(GNUInstallDirs)
include(KDEInstallDirs)
include(ECMFindQmlModule)
include(Asteroid6CMakeSettings)

find_package(Qt6 COMPONENTS Core DBus Qml Sensors REQUIRED)
pkg_check_modules(mlite6 REQUIRED mlite6)
# every heart rate reading (daemon/sensorPlugins/heartrateDb.h)
pkg_check_modules(sqlite3 REQUIRED sqlite3)
# the Health monitor's own system wake (R5 E-04): libkeepalive's BackgroundActivity (dsme iphb, mce keepalive)
pkg_check_modules(keepalive REQUIRED keepalive)

# the Health metrics (distance, energy, floors, zones, alerts, reminders, sleep, auto exercise) and the health
# database (store/: health.db, its one-time import), shared by the daemon and the QML plugin
set(HEALTH_METRICS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/metrics/healthProfile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/metrics/healthMetrics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/metrics/hrAlerts.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/metrics/moveReminder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/metrics/sleepEstimator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/metrics/autoExercise.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/metrics/healthData.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/metrics/restingHr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/metrics/skinTemp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/store/healthDb.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/store/healthImport.cpp
)
add_subdirectory(daemon)
add_subdirectory(qmlplugin)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
