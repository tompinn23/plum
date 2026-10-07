#pragma once

#include <chrono>
#include <deque>

#include <QDateTime>
#include <QObject>
#include <QString>

struct notification {
    enum class level { info, good, warning };

    QString title;
    QString text;
    level kind = level::info;
    // Posts with the same key, about the same commander, close together merge into one with a
    // count. Empty: never merged.
    QString key;
    std::chrono::milliseconds lifetime = std::chrono::seconds(6);

    // Filled in by the poster's dashboard and by the hub, not by whoever writes the notification.
    QString source; // the commander it is about
    QDateTime at;
    int count = 1;
};

// Every notification in the app goes through here, on the UI thread. Dashboards and their panels
// post; the toast overlay, and anything else that wants them, listens.
class notifications : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    void post(notification n);

    // Newest first, capped, for a log page.
    [[nodiscard]] const std::deque<notification> &recent() const { return m_recent; }

    signals:




    void posted(const notification &n); // a new one
    void merged(const notification &n); // a repeat folded into the newest; its count went up

private:
    std::deque<notification> m_recent;
};
