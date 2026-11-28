# Host tests for sensorlogd: a stand-alone project (the top-level build needs AsteroidOS's ECM set-up, AsteroidApp6
# and mlite6, which a desktop does not have). It builds the daemon's and the QML plugin's sources against the
# machine's Qt 6 (Core, DBus, Sensors, Test) with the stand-ins in compat/ (QHrmSensor, QStepCounterSensor:
# AsteroidOS's additions to QtSensors; MGConfItem: mlite) and the fake QtSensors backend in fakes/.
#   cmake -S tests -B build-tests && cmake --build build-tests && dbus-run-session -- ctest --test-dir build-tests
# -DSENSORLOGD_COVERAGE=ON compiles the sources under test with --coverage (.sonar/coverage.sh does).
cmake_minimum_required(VERSION 3.16)
project(sensorlogd-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

option(SENSORLOGD_COVERAGE "compile the sources under test with gcc --coverage" OFF)
# a folder for Qt Test's JUnit reports (one per program, for SonarQube's cxx xunit import); empty: none
set(SENSORLOGD_JUNIT_DIR "" CACHE PATH "write each test program's results as JUnit XML into this folder")

find_package(Qt6 REQUIRED COMPONENTS Core DBus Sensors Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
enable_testing()

set(top ${CMAKE_CURRENT_SOURCE_DIR}/..)

# the stand-ins and the fake backend (test code, no coverage)
add_library(sensorlogd-testsupport STATIC
    compat/QtSensors/qhrmsensor.h
    compat/QtSensors/qstepcountersensor.h
    compat/mgconfitem.h
    compat/compatsensors.cpp
    compat/keepalive/backgroundactivity.h
    compat/backgroundactivity.cpp
    fakes/fakesensorbackend.h
    fakes/fakesensorbackend.cpp
)
target_include_directories(sensorlogd-testsupport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/compat ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sensorlogd-testsupport PUBLIC Qt6::Core Qt6::DBus Qt6::Sensors Qt6::Test)

# the code under test, as daemon/CMakeLists.txt and qmlplugin/CMakeLists.txt build it (sensorlogd.cpp, the
# daemon's main(), and the QML registration plugin are not part of it)
add_library(sensorlogd-undertest STATIC
    ${top}/common.cpp ${top}/common.h
    ${top}/daemon/logger.cpp ${top}/daemon/logger.h
    ${top}/daemon/sensorPlugins/stepCounter.cpp ${top}/daemon/sensorPlugins/stepCounter.h
    ${top}/daemon/sensorPlugins/heartrateSensor.cpp ${top}/daemon/sensorPlugins/heartrateSensor.h
    ${top}/daemon/sensorPlugins/heartrateDb.cpp ${top}/daemon/sensorPlugins/heartrateDb.h
    ${top}/daemon/sensorPlugins/barometerSensor.cpp ${top}/daemon/sensorPlugins/barometerSensor.h
    ${top}/daemon/sensorPlugins/activityStreams.cpp ${top}/daemon/sensorPlugins/activityStreams.h
    ${top}/daemon/sensorPlugins/offBody.cpp ${top}/daemon/sensorPlugins/offBody.h
    ${top}/daemon/sensorPlugins/skinTempSensor.cpp ${top}/daemon/sensorPlugins/skinTempSensor.h
    ${top}/qmlplugin/stepsDataLoader.cpp ${top}/qmlplugin/stepsDataLoader.h
    ${top}/qmlplugin/hrDataLoader.cpp ${top}/qmlplugin/hrDataLoader.h
    ${top}/qmlplugin/weightDataLoader.cpp ${top}/qmlplugin/weightDataLoader.h
    ${top}/qmlplugin/barometerDataLoader.cpp ${top}/qmlplugin/barometerDataLoader.h
    ${top}/qmlplugin/healthMetricsLoader.cpp ${top}/qmlplugin/healthMetricsLoader.h
    ${top}/qmlplugin/sleepDataLoader.cpp ${top}/qmlplugin/sleepDataLoader.h
    ${top}/qmlplugin/workoutLoader.cpp ${top}/qmlplugin/workoutLoader.h
    ${top}/qmlplugin/loggerSettings.cpp ${top}/qmlplugin/loggerSettings.h
    ${top}/daemon/healthMonitor.cpp ${top}/daemon/healthMonitor.h
    ${top}/daemon/healthNotifier.cpp ${top}/daemon/healthNotifier.h
    ${top}/daemon/healthLanguage.cpp ${top}/daemon/healthLanguage.h ${top}/daemon/eoscatalogues.h
    ${top}/daemon/healthWake.h
    ${top}/daemon/keepaliveWake.cpp ${top}/daemon/keepaliveWake.h
    ${top}/metrics/healthProfile.cpp ${top}/metrics/healthProfile.h
    ${top}/metrics/healthMetrics.cpp ${top}/metrics/healthMetrics.h
    ${top}/metrics/hrAlerts.cpp ${top}/metrics/hrAlerts.h
    ${top}/metrics/moveReminder.cpp ${top}/metrics/moveReminder.h
    ${top}/metrics/sleepEstimator.cpp ${top}/metrics/sleepEstimator.h
    ${top}/metrics/autoExercise.cpp ${top}/metrics/autoExercise.h
    ${top}/metrics/healthData.cpp ${top}/metrics/healthData.h
    ${top}/metrics/restingHr.cpp ${top}/metrics/restingHr.h
    ${top}/metrics/skinTemp.cpp ${top}/metrics/skinTemp.h
    ${top}/metrics/goals.cpp ${top}/metrics/goals.h
    ${top}/store/healthDb.cpp ${top}/store/healthDb.h
    ${top}/store/healthImport.cpp ${top}/store/healthImport.h
)
target_link_libraries(sensorlogd-undertest PUBLIC sensorlogd-testsupport PkgConfig::SQLITE3)
if(SENSORLOGD_COVERAGE)
    target_compile_options(sensorlogd-undertest PRIVATE -O0 -g --coverage)
    target_link_options(sensorlogd-undertest PUBLIC --coverage)
endif()

# tst_stepcounter and tst_logger test the timer-driven plugins from before the sensor hub's batches (108b628) and
# are not built until they are brought up to it; tst_heartrate tests the batched heart rate and its database,
# tst_store the health database and its one-time import, tst_api the daemon's D-Bus methods for it, tst_activity
# the hub's motion and cadence streams (WO-05), tst_workouthub the workout latency asked of the hub (WO-08),
# tst_offbody the hub's off-body state, the monitor's worn() and what is left out off the wrist (WO-03), tst_sleep
# the sleep estimate of version 2 on invented nights, its timeline in health.db and the day's naps (WO-06),
# tst_skintemp the skin temperature: the hub's readings, when they are taken, the night's value and variation (WO-13).
# tst_language the notifications' language (R6 K-05): the English without a catalogue, a language change from
# localed, the nearest catalogue, and the daemon's own German and Russian catalogues (i18n/), made with Qt's lrelease
# when Qt6LinguistTools is there (else those cases are skipped).
foreach(name common heartrate barometer dataloaders health monitor healthloaders store api activity workouthub offbody
        sleep skintemp language)
    add_executable(tst_${name} tst_${name}.cpp testenv.h peek.h)
    target_link_libraries(tst_${name} PRIVATE sensorlogd-undertest)
    if(SENSORLOGD_JUNIT_DIR)
        add_test(NAME ${name} COMMAND tst_${name} -o ${SENSORLOGD_JUNIT_DIR}/tst_${name}.xml,junitxml -o -,txt)
    else()
        add_test(NAME ${name} COMMAND tst_${name})
    endif()
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
endforeach()

find_package(Qt6 QUIET OPTIONAL_COMPONENTS LinguistTools)
if(TARGET Qt6::lrelease)
    set(qmdir ${CMAKE_CURRENT_BINARY_DIR}/i18n)
    file(MAKE_DIRECTORY ${qmdir})
    set(qms "")
    foreach(ts ${CMAKE_CURRENT_SOURCE_DIR}/i18n/tst-health.de_DE.ts ${CMAKE_CURRENT_SOURCE_DIR}/i18n/tst-health.fr.ts
            ${CMAKE_CURRENT_SOURCE_DIR}/i18n/tst-health.en_GB.ts ${CMAKE_CURRENT_SOURCE_DIR}/i18n/tst-health.en.ts
            ${top}/i18n/sensorlogd.de_DE.ts ${top}/i18n/sensorlogd.ru.ts ${top}/i18n/sensorlogd.en_GB.ts)
        get_filename_component(base ${ts} NAME_WE)
        get_filename_component(ext ${ts} EXT)
        string(REGEX REPLACE "\\.ts$" ".qm" qm "${base}${ext}")
        add_custom_command(OUTPUT ${qmdir}/${qm} COMMAND Qt6::lrelease -silent ${ts} -qm ${qmdir}/${qm} DEPENDS ${ts}
            VERBATIM)
        list(APPEND qms ${qmdir}/${qm})
    endforeach()
    add_custom_target(tst_language_catalogues DEPENDS ${qms})
    add_dependencies(tst_language tst_language_catalogues)
    target_compile_definitions(tst_language PRIVATE TST_I18N_DIR="${qmdir}")
endif()
