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
enable_testing()

set(top ${CMAKE_CURRENT_SOURCE_DIR}/..)

# the stand-ins and the fake backend (test code, no coverage)
add_library(sensorlogd-testsupport STATIC
    compat/QtSensors/qhrmsensor.h
    compat/QtSensors/qstepcountersensor.h
    compat/mgconfitem.h
    compat/compatsensors.cpp
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
    ${top}/daemon/sensorPlugins/barometerSensor.cpp ${top}/daemon/sensorPlugins/barometerSensor.h
    ${top}/qmlplugin/stepsDataLoader.cpp ${top}/qmlplugin/stepsDataLoader.h
    ${top}/qmlplugin/hrDataLoader.cpp ${top}/qmlplugin/hrDataLoader.h
    ${top}/qmlplugin/weightDataLoader.cpp ${top}/qmlplugin/weightDataLoader.h
    ${top}/qmlplugin/barometerDataLoader.cpp ${top}/qmlplugin/barometerDataLoader.h
)
target_link_libraries(sensorlogd-undertest PUBLIC sensorlogd-testsupport)
if(SENSORLOGD_COVERAGE)
    target_compile_options(sensorlogd-undertest PRIVATE -O0 -g --coverage)
    target_link_options(sensorlogd-undertest PUBLIC --coverage)
endif()

# tst_stepcounter, tst_heartrate and tst_logger test the timer-driven plugins from before the sensor hub's batches
# (108b628) and are not built until they are brought up to it
foreach(name common barometer dataloaders)
    add_executable(tst_${name} tst_${name}.cpp testenv.h peek.h)
    target_link_libraries(tst_${name} PRIVATE sensorlogd-undertest)
    if(SENSORLOGD_JUNIT_DIR)
        add_test(NAME ${name} COMMAND tst_${name} -o ${SENSORLOGD_JUNIT_DIR}/tst_${name}.xml,junitxml -o -,txt)
    else()
        add_test(NAME ${name} COMMAND tst_${name})
    endif()
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
endforeach()
