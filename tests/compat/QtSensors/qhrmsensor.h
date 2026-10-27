// Test stand-in for AsteroidOS's QHrmSensor (a patch to QtSensors in meta-asteroid; Debian's QtSensors has no heart
// rate sensor). Same public API as the SDK's qhrmsensor.h, built on the real QSensor / QSensorReading, so the tests'
// fake backend (tests/fakes) feeds readings through QtSensors' own backend machinery.
#ifndef COMPAT_QHRMSENSOR_H
#define COMPAT_QHRMSENSOR_H

#include <QtSensors/qsensor.h>

class QHrmReadingPrivate;

class QHrmReading : public QSensorReading
{
    Q_OBJECT
    Q_PROPERTY(int bpm READ bpm)
    DECLARE_READING(QHrmReading)
public:
    int bpm() const;
    void setBpm(int bpm);
};

class QHrmSensor : public QSensor
{
    Q_OBJECT
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
public:
    enum Status { NoContact, Unreliable, AccuracyLow, AccuracyMedium, AccuracyHigh };
    Q_ENUM(Status)

    explicit QHrmSensor(QObject *parent = nullptr);
    ~QHrmSensor() override;
    QHrmReading *reading() const;
    static char const * const sensorType;

    Status status() const;
    void setStatus(Status status);

Q_SIGNALS:
    void statusChanged(QHrmSensor::Status status);

private:
    Status m_status = NoContact;
};

#endif
