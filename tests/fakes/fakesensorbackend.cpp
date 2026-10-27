#include "fakesensorbackend.h"

#include <QHash>
#include <QtSensors/QPressureSensor>
#include <QtSensors/QSensorBackendFactory>
#include <QtSensors/QSensorManager>

#include <QtSensors/QHrmSensor>
#include <QtSensors/QStepCounterSensor>

namespace {
struct State {
    QHash<QByteArray, FakeSensorBackend *> last;
    QHash<QByteArray, int> created;
    QHash<QByteArray, int> startFailures;
    QHash<QByteArray, int> createFailures;
};

State &state()
{
    static State s;
    return s;
}

class Factory : public QSensorBackendFactory
{
public:
    QSensorBackend *createBackend(QSensor *sensor) override
    {
        if (FakeSensorBackend::takeCreateFailure(sensor->type()))
            return nullptr;
        return new FakeSensorBackend(sensor);
    }
};

const QByteArray fakeId("fake.sensorlogd-tests");
}

FakeSensorBackend::FakeSensorBackend(QSensor *sensor) : QSensorBackend(sensor)
{
    const QByteArray type = sensor->type();
    if (type == QStepCounterSensor::sensorType)
        m_reading = setReading<QStepCounterReading>(nullptr);
    else if (type == QHrmSensor::sensorType)
        m_reading = setReading<QHrmReading>(nullptr);
    else
        m_reading = setReading<QPressureReading>(nullptr);
    state().last[type] = this;
    state().created[type]++;
}

FakeSensorBackend::~FakeSensorBackend()
{
    for (auto it = state().last.begin(); it != state().last.end(); ++it)
        if (it.value() == this)
            it.value() = nullptr;
}

void FakeSensorBackend::start()
{
    m_starts++;
    int &failures = state().startFailures[sensor()->type()];
    if (failures > 0) {
        failures--;
        sensorStopped(); // what sensorfw's backend does when it has no sensor interface
        return;
    }
    m_running = true;
}

void FakeSensorBackend::stop()
{
    m_stops++;
    m_running = false;
}

void FakeSensorBackend::install()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    qputenv("QT_SENSORS_LOAD_PLUGINS", "0"); // only this backend, never one installed on the machine
    static Factory factory;
    for (const char *type : {QStepCounterSensor::sensorType, QHrmSensor::sensorType, QPressureSensor::sensorType})
        QSensorManager::registerBackend(type, fakeId, &factory);
}

void FakeSensorBackend::reset()
{
    state().last.clear();
    state().created.clear();
    state().startFailures.clear();
    state().createFailures.clear();
}

FakeSensorBackend *FakeSensorBackend::last(const QByteArray &type) { return state().last.value(type); }
int FakeSensorBackend::created(const QByteArray &type) { return state().created.value(type); }
void FakeSensorBackend::failStarts(const QByteArray &type, int count) { state().startFailures[type] = count; }
void FakeSensorBackend::failCreates(const QByteArray &type, int count) { state().createFailures[type] = count; }

bool FakeSensorBackend::takeCreateFailure(const QByteArray &type)
{
    int &failures = state().createFailures[type];
    if (failures <= 0)
        return false;
    failures--;
    return true;
}

void FakeSensorBackend::pushSteps(int steps, quint64 timestamp)
{
    auto *r = static_cast<QStepCounterReading *>(m_reading);
    r->setSteps(steps);
    r->setTimestamp(timestamp);
    newReadingAvailable();
}

void FakeSensorBackend::pushHeartRate(int bpm, int status, quint64 timestamp)
{
    auto *r = static_cast<QHrmReading *>(m_reading);
    r->setBpm(bpm);
    r->setTimestamp(timestamp);
    static_cast<QHrmSensor *>(sensor())->setStatus(static_cast<QHrmSensor::Status>(status));
    newReadingAvailable();
}

void FakeSensorBackend::pushPressure(qreal pascals, quint64 timestamp)
{
    auto *r = static_cast<QPressureReading *>(m_reading);
    r->setPressure(pascals);
    r->setTimestamp(timestamp);
    newReadingAvailable();
}
