// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/OperationRunner.h"
#include "dingobench/infrastructure/ProfileStore.h"

#include <QProcess>
#include <QTimer>
#include <memory>

class QTemporaryDir;

namespace dingobench::dicom {

struct WorklistRecord {
    QString file, station, modality, date, issue;
};

struct WorklistServerSettings {
    profiles::Profile source;
    QString directory, executable;
    int port = 11114;
    bool ipv6 = false;
    bool allowWildcard = false;
};

QString findWorklistServerUtility();

class WorklistServer final : public QObject {
    Q_OBJECT

public:
    enum class State { Stopped, Loading, Starting, Listening, Stopping, Error };

    explicit WorklistServer(logging::Logger &logger);
    ~WorklistServer() override;
    void refreshRecords(const QString &directory);
    void generateSamples(const QString &directory);
    void start(WorklistServerSettings settings);
    void stop();
    State state() const { return m_state; }
    QString message() const { return m_message; }
    qint64 processId() const { return m_process.processId(); }
    bool busy() const;
    const QList<WorklistRecord> &records() const { return m_records; }

signals:
    void sessionFinished(dingobench::dicom::DiagnosticSession session);
    void stateChanged();
    void recordsChanged();
    void activity(QString event, QString detail);

private:
    enum class RecordAction { Refresh, Generate, Start };
    struct Scan;
    void prepare(QString directory, RecordAction action);
    void startProbe();
    void workerFinished(QUuid id, const OperationResult &result);
    void stopWith(QString failure);
    void finishStop();
    void setState(State state, QString message, bool synthetic = false);
    void readOutput();
    void readLine(QByteArray line);
    void reportActivity(QString event, QString detail, logging::Severity severity = logging::Severity::Info);

    void finishSession(const QDateTime &ended, QString result, QString closure);
    std::optional<DiagnosticSession> m_diagnostic;
    logging::Logger &m_logger;
    QProcess m_process;
    QTimer m_stopDeadline;
    QList<WorklistRecord> m_records;
    WorklistServerSettings m_settings;
    std::shared_ptr<Scan> m_scan;
    std::unique_ptr<QTemporaryDir> m_stage;
    State m_state = State::Stopped;
    QString m_message = QStringLiteral("Worklist server stopped.");
    QString m_failure, m_launchError, m_session;
    QUuid m_job;
    QByteArray m_buffer;
    RecordAction m_recordAction = RecordAction::Refresh;
    bool m_probeSeen = false, m_probeAssociation = false, m_discardLine = false;
    OperationRunner m_worker;
};

}  // namespace dingobench::dicom
