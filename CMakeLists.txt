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

add_subdirectory(daemon)
add_subdirectory(qmlplugin)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
