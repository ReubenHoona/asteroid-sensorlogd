set(SRC
    sensorlogdqmlplugin.cpp
    stepsDataLoader.cpp
    hrDataLoader.cpp
    weightDataLoader.cpp
    barometerDataLoader.cpp
    loggerSettings.cpp
    ../common.cpp
)

SET(HEADERS
    sensorlogdqmlplugin.h
    stepsDataLoader.h
    hrDataLoader.h
    weightDataLoader.h
    barometerDataLoader.h
    loggerSettings.h
    ../common.h
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
)

install(
    TARGETS sensorlogdqmlplugin
	DESTINATION ${KDE_INSTALL_QMLDIR}/org/asteroid/sensorlogd
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/qmldir"
	DESTINATION ${KDE_INSTALL_QMLDIR}/org/asteroid/sensorlogd
)
