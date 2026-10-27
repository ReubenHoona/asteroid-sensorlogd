// Test stand-in for mlite's MGConfItem (dconf keys; mlite is not packaged in Debian). The barometer plugin reads its
// calibration offset through it; tests set the values with MGConfItem::setTestValue().
#ifndef COMPAT_MGCONFITEM_H
#define COMPAT_MGCONFITEM_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>

class MGConfItem : public QObject
{
    Q_OBJECT
public:
    explicit MGConfItem(const QString &key, QObject *parent = nullptr) : QObject(parent), m_key(key) {}
    QString key() const { return m_key; }
    QVariant value() const { return store().value(m_key); }
    QVariant value(const QVariant &def) const { return store().value(m_key, def); }

    static void setTestValue(const QString &key, const QVariant &value) { store().insert(key, value); }
    static void clearTestValues() { store().clear(); }

private:
    static QHash<QString, QVariant> &store()
    {
        static QHash<QString, QVariant> values;
        return values;
    }
    QString m_key;
};

#endif
