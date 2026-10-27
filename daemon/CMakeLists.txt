add_executable(
    sensorlogd
    sensorlogd.cpp
    logger.cpp
    logger.h
    sensorPlugins/stepCounter.cpp
    sensorPlugins/stepCounter.h
    sensorPlugins/heartrateSensor.cpp
    sensorPlugins/heartrateSensor.h
    sensorPlugins/barometerSensor.cpp
    sensorPlugins/barometerSensor.h
    ../common.cpp
    ../common.h
)

set_target_properties(sensorlogd PROPERTIES AUTOMOC ON)
target_link_libraries(
    sensorlogd
    PRIVATE
        Qt::Core
        Qt::DBus
        Qt::Qml
        Qt::Sensors
        ${mlite6_LIBRARIES}
        ${keepalive_LIBRARIES}
)
target_include_directories(sensorlogd PRIVATE ${mlite6_INCLUDE_DIRS} ${keepalive_INCLUDE_DIRS})
install(TARGETS sensorlogd DESTINATION bin)
install(
    FILES systemd/asteroid-sensorlogd.service
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user
)
