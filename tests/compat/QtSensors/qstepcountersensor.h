// Test stand-in for AsteroidOS's QStepCounterSensor (a patch to QtSensors in meta-asteroid; not in Debian's
// QtSensors). Same public API as the SDK's qstepcountersensor.h, built on the real QSensor / QSensorReading.
#ifndef COMPAT_QSTEPCOUNTERSENSOR_H
#define COMPAT_QSTEPCOUNTERSENSOR_H

#include <QtSensors/qsensor.h>

class QStepCounterReadingPrivate;

class QStepCounterReading : public QSensorReading
{
    Q_OBJECT
    Q_PROPERTY(int steps READ steps)
    DECLARE_READING(QStepCounterReading)
public:
    int steps() const;
    void setSteps(int steps);
};

class QStepCounterSensor : public QSensor
{
    Q_OBJECT
public:
    explicit QStepCounterSensor(QObject *parent = nullptr);
    ~QStepCounterSensor() override;
    QStepCounterReading *reading() const;
    static char const * const sensorType;
};

#endif
