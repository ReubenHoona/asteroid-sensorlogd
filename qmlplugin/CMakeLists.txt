set(SRC
    sensorlogdqmlplugin.cpp
    stepsDataLoader.cpp
    hrDataLoader.cpp
    weightDataLoader.cpp
    barometerDataLoader.cpp
    loggerSettings.cpp
    healthMetricsLoader.cpp
    sleepDataLoader.cpp
    ../common.cpp
    ../daemon/sensorPlugins/heartrateDb.cpp
    ${HEALTH_METRICS_SOURCES}
)

SET(HEADERS
    sensorlogdqmlplugin.h
    stepsDataLoader.h
    hrDataLoader.h
    weightDataLoader.h
    barometerDataLoader.h
    loggerSettings.h
    healthMetricsLoader.h
    sleepDataLoader.h
    ../common.h
    ../daemon/sensorPlugins/heartrateDb.h
)

qt6_add_qml_module(sensorlogdqmlplugin
    URI org.asteroid.sensorlogd
    VERSION 1.0
    PLUGIN_TARGET sensorlogdqmlplugin
    CLASS_NAME LogdPlugin
    NO_GENERATE_PLUGIN_SOURCE
    SOURCES ${SRC} ${HEADERS}
    QML_FILES ${controls-qml}
)

target_link_libraries(
    sensorlogdqmlplugin
    PUBLIC
	    Qt::Qml
        Qt::DBus
        Qt::Sensors
        ${mlite6_LIBRARIES}
        ${sqlite3_LIBRARIES}
)
target_include_directories(sensorlogdqmlplugin PRIVATE ${mlite6_INCLUDE_DIRS} ${sqlite3_INCLUDE_DIRS})

install(
    TARGETS sensorlogdqmlplugin
	DESTINATION ${KDE_INSTALL_QMLDIR}/org/asteroid/sensorlogd
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/qmldir"
	DESTINATION ${KDE_INSTALL_QMLDIR}/org/asteroid/sensorlogd
)
