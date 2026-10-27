// A QtSensors backend for the tests. Registered with QSensorManager for the three sensor types sensorlogd uses
// (QStepCounterSensor, QHrmSensor, QPressureSensor), it stands in for sensorfw's backend: a test reaches the backend
// made last for a type (FakeSensorBackend::last(type)), pushes readings through QtSensors' newReadingAvailable()
// path (so the plugin gets readingChanged() as on the watch), and makes start() or making the backend fail the way
// sensorfw's backend does before sensorfwd is up (start() reports the sensor stopped; no backend at all).
#ifndef FAKESENSORBACKEND_H
#define FAKESENSORBACKEND_H

#include <QByteArray>
#include <QtSensors/QSensorBackend>

class FakeSensorBackend : public QSensorBackend
{
public:
    explicit FakeSensorBackend(QSensor *sensor);
    ~FakeSensorBackend() override;

    void start() override;
    void stop() override;

    // QT_SENSORS_LOAD_PLUGINS=0 and the fake factory registered for the three types (once per process)
    static void install();
    // forgets the backends made so far and the failure settings (call it in init())
    static void reset();
    // the backend made last for a sensor of this type (nullptr: none yet, or its sensor was destroyed)
    static FakeSensorBackend *last(const QByteArray &type);
    static int created(const QByteArray &type);
    // the next COUNT start() calls for this type fail (the sensor reports itself stopped)
    static void failStarts(const QByteArray &type, int count);
    // the next COUNT attempts to make a backend for this type fail (QSensor::start() returns false at once)
    static void failCreates(const QByteArray &type, int count);
    static bool takeCreateFailure(const QByteArray &type);

    void pushSteps(int steps, quint64 timestamp = 1);
    void pushHeartRate(int bpm, int status, quint64 timestamp = 1);
    void pushPressure(qreal pascals, quint64 timestamp = 1);

    bool running() const { return m_running; }
    int starts() const { return m_starts; }
    int stops() const { return m_stops; }

private:
    QSensorReading *m_reading = nullptr;
    bool m_running = false;
    int m_starts = 0;
    int m_stops = 0;
};

#endif
