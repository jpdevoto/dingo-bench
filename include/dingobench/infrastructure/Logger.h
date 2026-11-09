// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>

#include <optional>

namespace dingobench::logging {

enum class Severity { Debug, Info, Warning, Error };
QString severityName(Severity severity);

struct Event {
    quint64 sequence;
    QDateTime timestampUtc;
    Severity severity;
    QString component;
    QString message;
    QString operation;
    QString details;
    bool synthetic;
};

struct LogSnapshot {
    QList<Event> events;
    quint64 received;
    quint64 evicted;
};

// The owner must outlive producers. The event loop of the owning thread
// delivers change notifications, while producers only mutate the buffer.
class Logger final : public QObject {
    Q_OBJECT

public:
    explicit Logger(int capacity = 2000, Severity minimum = Severity::Debug);

    // Pass diagnostic metadata that is safe to display in the session log.
    std::optional<Event> log(Severity severity, QString component, QString message,
             QString operation = {}, QString details = {}, bool synthetic = false);
    QList<Event> snapshot() const;
    LogSnapshot session() const;
    void clear();
    void setCapacity(int capacity);
    void setMinimumSeverity(Severity minimum);

signals:
    void eventsChanged();

private:
    void queueNotification();

    mutable QMutex m_mutex;
    QList<Event> m_events;
    quint64 m_nextSequence = 1;
    quint64 m_evicted = 0;
    int m_capacity;
    Severity m_minimum;
    bool m_notificationPending = false;
};

// One scoped bridge for the application's process-wide Qt message handler.
class QtErrorCapture final {
public:
    explicit QtErrorCapture(Logger &logger);
    ~QtErrorCapture();
    QtErrorCapture(const QtErrorCapture &) = delete;
    QtErrorCapture &operator=(const QtErrorCapture &) = delete;

private:
    bool m_installed = false;
};

}  // namespace dingobench::logging
