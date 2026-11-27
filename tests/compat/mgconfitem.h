// Test stand-in for mlite's MGConfItem (dconf keys; mlite is not packaged in Debian). The barometer plugin reads its
// calibration offset through it; tests set the values with MGConfItem::setTestValue(), which, as a dconf write does,
// tells the items of the key that are alive (valueChanged) when the value is another. testCreated() counts the items
// made (a dconf read each on the watch).
#ifndef COMPAT_MGCONFITEM_H
#define COMPAT_MGCONFITEM_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>

class MGConfItem : public QObject
{
    Q_OBJECT
public:
    explicit MGConfItem(const QString &key, QObject *parent = nullptr) : QObject(parent), m_key(key)
    {
        live().append(this);
        testCreated()++;
    }
    ~MGConfItem() override { live().removeAll(this); }
    QString key() const { return m_key; }
    QVariant value() const { return store().value(m_key); }
    QVariant value(const QVariant &def) const { return store().value(m_key, def); }

    void set(const QVariant &value) { store().insert(m_key, value); emit valueChanged(); }

    static void setTestValue(const QString &key, const QVariant &value)
    {
        const QVariant before = store().value(key);
        store().insert(key, value);
        if (before == value)
            return;
        const QList<MGConfItem *> items = live();
        for (MGConfItem *item : items)
            if (live().contains(item) && item->m_key == key)
                emit item->valueChanged();
    }
    static void clearTestValues() { store().clear(); }
    static QVariant testValue(const QString &key) { return store().value(key); }
    static int &testCreated()
    {
        static int n = 0;
        return n;
    }

signals:
    void valueChanged();

private:
    static QHash<QString, QVariant> &store()
    {
        static QHash<QString, QVariant> values;
        return values;
    }
    static QList<MGConfItem *> &live()
    {
        static QList<MGConfItem *> items;
        return items;
    }
    QString m_key;
};

#endif
