// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/OperationRunner.h"
#include "dingobench/infrastructure/ProfileStore.h"

#include <QProcess>
#include <QTimer>
#include <memory>

class QTemporaryDir;

namespace dingobench::dicom {

struct ReceiveEvent {
    QDateTime timestamp;
    QString association, callingAe, calledAe, peer, action, message;
    QString sopClass, sopInstance, path, moveOriginatorAe;
    std::optional<quint16> moveOriginatorId;
    qint64 bytes = 0;
    std::optional<quint16> status;
};

// Returns an existing canonical directory outside Git checkouts, or empty.
QString receiveDirectory(const QString &path);

class StorageReceiver final : public QObject {
    Q_OBJECT

public:
    enum class State { Stopped, Starting, Listening, Stopping, Error };

    explicit StorageReceiver(logging::Logger &logger, QObject *parent = nullptr);
    ~StorageReceiver() override;
    void start(const profiles::Profile &source, const QString &output, bool enforceCalledAe);
    void stop();
    State state() const { return m_state; }
    QString message() const { return m_message; }
    QString listeningAe() const { return m_state == State::Listening ? m_listenerAe : QString{}; }
    QString listeningEndpoint() const { return m_state == State::Listening ? m_endpoint : QString{}; }
    bool active() const { return m_process.state() != QProcess::NotRunning; }
    qint64 processId() const { return m_process.processId(); }

signals:
    void sessionFinished(dingobench::dicom::DiagnosticSession session);
    void stateChanged();
    void eventReceived(dingobench::dicom::ReceiveEvent event);

private:
    void setState(State state, QString message);
    void readEvents();
    void captureEvent(const QJsonObject &object, const ReceiveEvent &event, std::optional<logging::Event> logged);
    void resetAfterExit();

    void finishSession(const QDateTime &ended, QString result, QString closure);
    std::optional<DiagnosticSession> m_diagnostic;
    logging::Logger &m_logger;
    QProcess m_process;
    QTimer m_deadline;
    std::unique_ptr<QTemporaryDir> m_stage;
    QByteArray m_buffer;
    QString m_output, m_listenerId, m_listenerAe, m_endpoint, m_failure;
    std::optional<DiagnosticSession::Identity> m_identity;
    State m_state = State::Stopped;
    QString m_message = QStringLiteral("Listener stopped.");
    bool m_stopRequested = false;
};

Operation inspectReceivedFile(QString path, std::function<void(DatasetSnapshot)> deliver);

}  // namespace dingobench::dicom
