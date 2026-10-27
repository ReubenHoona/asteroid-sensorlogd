// The daemon's classes with their private members open to the tests: the plugins keep their state (offsets, the
// last record time, timers, retry delays) private and have no seams, and the tests must set the last record time
// to yesterday to see a midnight, or read a retry timer's interval. The source stays untouched: every header these
// classes include is included first, then the class headers with `private` read as `public` (access does not change
// the layout or the mangled names with GCC).
#ifndef SENSORLOGD_PEEK_H
#define SENSORLOGD_PEEK_H

#include <QDBusAbstractAdaptor>
#include <QDBusInterface>
#include <QDBusServiceWatcher>
#include <QDate>
#include <QDateTime>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QTimer>
#include <QtSensors/QPressureSensor>

#include <MGConfItem>
#include <QtSensors/QHrmSensor>
#include <QtSensors/QStepCounterSensor>

#define private public
#include "../daemon/logger.h"
#undef private

#endif
