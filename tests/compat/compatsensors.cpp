// The test stand-ins for AsteroidOS's QHrmSensor and QStepCounterSensor (see their headers).
#include "QtSensors/qhrmsensor.h"
#include "QtSensors/qstepcountersensor.h"

class QHrmReadingPrivate
{
public:
    int bpm = 0;
};

IMPLEMENT_READING(QHrmReading)

int QHrmReading::bpm() const { return d->bpm; }
void QHrmReading::setBpm(int bpm) { d->bpm = bpm; }

char const * const QHrmSensor::sensorType("QHrmSensor");

QHrmSensor::QHrmSensor(QObject *parent) : QSensor(QHrmSensor::sensorType, parent) {}
QHrmSensor::~QHrmSensor() = default;
QHrmReading *QHrmSensor::reading() const { return static_cast<QHrmReading *>(QSensor::reading()); }
QHrmSensor::Status QHrmSensor::status() const { return m_status; }

void QHrmSensor::setStatus(Status status)
{
    if (m_status != status) {
        m_status = status;
        emit statusChanged(status);
    }
}

class QStepCounterReadingPrivate
{
public:
    int steps = 0;
};

IMPLEMENT_READING(QStepCounterReading)

int QStepCounterReading::steps() const { return d->steps; }
void QStepCounterReading::setSteps(int steps) { d->steps = steps; }

char const * const QStepCounterSensor::sensorType("QStepCounterSensor");

QStepCounterSensor::QStepCounterSensor(QObject *parent) : QSensor(QStepCounterSensor::sensorType, parent) {}
QStepCounterSensor::~QStepCounterSensor() = default;
QStepCounterReading *QStepCounterSensor::reading() const
{
    return static_cast<QStepCounterReading *>(QSensor::reading());
}
