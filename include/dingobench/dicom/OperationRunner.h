// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QUuid>

#include <functional>
#include <optional>
#include <stop_token>

#include "dingobench/infrastructure/Logger.h"
#include "dingobench/dicom/DiagnosticSession.h"

namespace dingobench::dicom {

enum class ErrorCode {
    InvalidInput, StartFailed, TimedOut, ProcessFailed, Internal, Dns, TcpFailure,
    ConnectionTimeout, AssociationTimeout, AssociationRejected, PresentationContextRejected,
    DimseTimeout, ProtocolFailure, EchoFailure, StorageFailure, FindFailure, MoveFailure, GetFailure, StorageRoleRejected
};

inline constexpr int maximumClientTimeout = 60;
struct ClientTimeouts {
    int connection = 5;
    int association = 5;
    int dimse = 5;
};

struct OperationError {
    ErrorCode code;
    // This message is surfaced to both the UI and the session log.
    QString message;
    std::optional<int> exitCode = std::nullopt;
};

struct OperationResult {
    enum class Status { Succeeded, Cancelled, Failed, Warning };

    Status status = Status::Succeeded;
    std::optional<OperationError> error;
    qint64 elapsedMs = 0;
    QString details;
    std::optional<quint16> dicomStatus;
    std::optional<DiagnosticSession> protocol;

    static OperationResult cancelled();
    static OperationResult failed(OperationError error);
};

struct DiagnosticEvent {
    logging::Severity severity;
    // Supply only safe diagnostic text. Keep raw protocol data and patient data out.
    QString message;
    QString details;
};

using ReportDiagnostic = std::function<void(DiagnosticEvent)>;
using Operation = std::function<OperationResult(std::stop_token, const ReportDiagnostic &)>;

// Call from the owning thread. One operation can run at a time per runner.
// The logger must outlive the runner. Workers must use bounded I/O and observe
// cancellation so destruction can join them. Use the reporter only during the call.
class OperationRunner final : public QObject {
    Q_OBJECT

public:
    explicit OperationRunner(logging::Logger &logger, QObject *parent = nullptr);
    ~OperationRunner() override;

    // Name appears in the log. A null ID indicates busy or empty work.
    QUuid start(QString name, Operation work, bool synthetic = false,
                QString component = QStringLiteral("DICOM"),
                std::optional<DiagnosticSession::Identities> identities = std::nullopt);
    bool cancel(const QUuid &id);

signals:
    void finished(QUuid id, dingobench::dicom::OperationResult result);

private:
    logging::Logger &m_logger;
    QThreadPool m_pool;
    QUuid m_active;
    std::stop_source m_stop;
};

}  // namespace dingobench::dicom
