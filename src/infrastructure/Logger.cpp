// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/infrastructure/Logger.h"

#include <QMetaObject>
#include <QtCore/qlogging.h>

#include <cstdio>
#include <utility>

namespace dingobench::logging {
namespace {

QString bounded(QString text, qsizetype maximum)
{
    if (text.size() <= maximum)
        return text;
    qsizetype end = maximum - 1;
    if (text.at(end - 1).isHighSurrogate())
        --end;
    text.truncate(end);
    return text + QChar(0x2026);
}

QMutex handlerMutex;
Logger *handlerLogger = nullptr;
QtMessageHandler previousHandler = nullptr;

void captureQtError(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const bool isError = type == QtCriticalMsg || type == QtFatalMsg;
    const QString safeMessage = isError ? (type == QtFatalMsg
        ? QStringLiteral("Unexpected Qt fatal diagnostic.")
        : QStringLiteral("Unexpected Qt critical diagnostic.")) : QString();
    QtMessageHandler previous;
    {
        const QMutexLocker guard(&handlerMutex);
        previous = previousHandler;
        if (handlerLogger && isError)
            handlerLogger->log(Severity::Error, QStringLiteral("Qt"), safeMessage);
    }
    const QString &output = isError ? safeMessage : message;
    if (previous)
        previous(type, context, output);
    else
        std::fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, context, output)));
}

}  // namespace

QString severityName(Severity severity)
{
    switch (severity) {
    case Severity::Debug: return QStringLiteral("DEBUG");
    case Severity::Info: return QStringLiteral("INFO");
    case Severity::Warning: return QStringLiteral("WARNING");
    case Severity::Error: return QStringLiteral("ERROR");
    }
    return {};
}

Logger::Logger(int capacity, Severity minimum)
    : m_capacity(qMax(1, capacity)), m_minimum(minimum)
{
}

std::optional<Event> Logger::log(Severity severity, QString component, QString message,
                 QString operation, QString details, bool synthetic)
{
    const QMutexLocker guard(&m_mutex);
    if (severity < m_minimum)
        return std::nullopt;
    if (component.trimmed().isEmpty())
        component = QStringLiteral("Application");
    if (message.trimmed().isEmpty())
        message = QStringLiteral("(No message provided)");
    if (m_events.size() == m_capacity) {
        m_events.removeFirst();
        ++m_evicted;
    }
    m_events.append(Event{m_nextSequence++, QDateTime::currentDateTimeUtc(), severity,
                          bounded(std::move(component), 128), bounded(std::move(message), 4096),
                          bounded(std::move(operation), 128), bounded(std::move(details), 8192),
                          synthetic});
    queueNotification();
    return m_events.constLast();
}

QList<Event> Logger::snapshot() const
{
    const QMutexLocker guard(&m_mutex);
    return m_events;
}

LogSnapshot Logger::session() const
{
    const QMutexLocker guard(&m_mutex);
    return {m_events, m_nextSequence - 1, m_evicted};
}

void Logger::clear()
{
    const QMutexLocker guard(&m_mutex);
    if (m_events.isEmpty())
        return;
    m_events.clear();
    queueNotification();
}

void Logger::setCapacity(int capacity)
{
    const QMutexLocker guard(&m_mutex);
    m_capacity = qMax(1, capacity);
    if (m_events.size() > m_capacity) {
        m_evicted += m_events.size() - m_capacity;
        m_events.remove(0, m_events.size() - m_capacity);
        queueNotification();
    }
}

void Logger::setMinimumSeverity(Severity minimum)
{
    const QMutexLocker guard(&m_mutex);
    m_minimum = minimum;
}

void Logger::queueNotification()
{
    // Call with the buffer mutex held. Coalesce a burst into one GUI update.
    if (m_notificationPending)
        return;
    m_notificationPending = true;
    QMetaObject::invokeMethod(this, [this] {
        {
            const QMutexLocker guard(&m_mutex);
            m_notificationPending = false;
        }
        emit eventsChanged();
    }, Qt::QueuedConnection);
}

QtErrorCapture::QtErrorCapture(Logger &logger)
{
    const QMutexLocker guard(&handlerMutex);
    if (handlerLogger)
        return;
    handlerLogger = &logger;
    previousHandler = qInstallMessageHandler(captureQtError);
    m_installed = true;
}

QtErrorCapture::~QtErrorCapture()
{
    if (!m_installed)
        return;
    const QMutexLocker guard(&handlerMutex);
    qInstallMessageHandler(previousHandler);
    handlerLogger = nullptr;
}

}  // namespace dingobench::logging
