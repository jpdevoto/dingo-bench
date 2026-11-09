// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/OperationRunner.h"

#include <QElapsedTimer>
#include <QThread>

#include <utility>

namespace dingobench::dicom {
namespace {
QString category(ErrorCode code)
{
    switch (code) {
    case ErrorCode::InvalidInput: return "Invalid input";
    case ErrorCode::StartFailed: return "Start failed";
    case ErrorCode::TimedOut: return "Timed out";
    case ErrorCode::ProcessFailed: return "Process failed";
    case ErrorCode::Internal: return "Internal error";
    case ErrorCode::Dns: return "DNS resolution failed or timed out";
    case ErrorCode::TcpFailure: return "TCP connection refused, unreachable or unavailable";
    case ErrorCode::ConnectionTimeout: return "TCP connection timed out";
    case ErrorCode::AssociationTimeout: return "Association negotiation timed out";
    case ErrorCode::AssociationRejected: return "Peer rejected the association";
    case ErrorCode::PresentationContextRejected: return "No usable presentation context";
    case ErrorCode::DimseTimeout: return "DIMSE response timed out";
    case ErrorCode::ProtocolFailure: return "Protocol failure";
    case ErrorCode::EchoFailure: return "Verification failed";
    case ErrorCode::StorageFailure: return "Storage failed";
    case ErrorCode::FindFailure: return "Query failed";
    case ErrorCode::MoveFailure: return "C-MOVE failed";
    case ErrorCode::GetFailure: return "C-GET failed";
    case ErrorCode::StorageRoleRejected: return "Peer did not negotiate the required Storage role";
    }
    return {};
}
}  // namespace
using logging::Severity;

OperationResult OperationResult::cancelled()
{
    return {Status::Cancelled, std::nullopt};
}

OperationResult OperationResult::failed(OperationError error)
{
    return {Status::Failed, std::move(error)};
}

OperationRunner::OperationRunner(logging::Logger &logger, QObject *parent)
    : QObject(parent), m_logger(logger)
{
    m_pool.setMaxThreadCount(1);
}

OperationRunner::~OperationRunner()
{
    m_stop.request_stop();
    m_pool.waitForDone();
}

QUuid OperationRunner::start(QString name, Operation work, bool synthetic, QString component,
                             std::optional<DiagnosticSession::Identities> identities)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active.isNull() || !work)
        return {};

    const auto id = QUuid::createUuid();
    m_active = id;
    m_stop = std::stop_source{};
    m_pool.start([this, id, token = m_stop.get_token(), name = std::move(name),
                  work = std::move(work), synthetic, component = std::move(component), identities = std::move(identities)] {
        QElapsedTimer elapsed;
        elapsed.start();
        const auto started = QDateTime::currentDateTimeUtc();
        const auto correlation = id.toString(QUuid::WithoutBraces);
        DiagnosticSession captured;
        captured.id = id;
        const ReportDiagnostic report = [&](DiagnosticEvent event) {
            if (auto logged = m_logger.log(event.severity, component, std::move(event.message), correlation,
                    event.details.isEmpty() ? name : name + u'\n' + event.details, synthetic))
                captured.addLog(std::move(*logged));
        };
        report({Severity::Info, QStringLiteral("Operation started.")});
        auto result = OperationResult::cancelled();
        if (!token.stop_requested()) {
            try {
                result = work(token, report);
            } catch (...) {
                result = OperationResult::failed(
                    {ErrorCode::Internal, QStringLiteral("The operation raised an unexpected error.")});
            }
        }

        result.elapsedMs = elapsed.elapsed();
        if (identities && !result.protocol)
            result.protocol.emplace();
        if (result.protocol) {
            auto &session = *result.protocol;
            session.id = id;
            if (identities)
                session.identities = *identities;
            if (session.association.connection.isEmpty())
                session.association.connection = "Not attempted";
            if (session.association.requestedEndpoint.isEmpty() && session.identities.destination) {
                const auto &peer = *session.identities.destination;
                const auto host = peer.address.trimmed();
                if (!host.isEmpty() && peer.port > 0)
                    session.association.requestedEndpoint = (host.contains(u':') ? u'[' + host + u']' : host)
                        + u':' + QString::number(peer.port);
            }
            session.operation = name;
            session.started = started;
            session.ended = QDateTime::currentDateTimeUtc();
            session.durationMs = result.elapsedMs;
            session.synthetic = synthetic;
            session.status = result.dicomStatus;
            session.result = result.status == OperationResult::Status::Succeeded ? "Succeeded"
                : result.status == OperationResult::Status::Cancelled ? "Cancelled"
                : result.status == OperationResult::Status::Warning ? "Warning" : "Failed";
            if (result.error) {
                session.error = result.error->message;
                session.errorCategory = category(result.error->code);
            }
        }
        const auto duration = QStringLiteral("Elapsed %1 ms.").arg(result.elapsedMs);
        switch (result.status) {
        case OperationResult::Status::Succeeded:
            report({Severity::Info, QStringLiteral("Operation completed."), duration});
            break;
        case OperationResult::Status::Warning:
            report({Severity::Warning, QStringLiteral("Operation completed with warnings."), duration});
            break;
        case OperationResult::Status::Cancelled:
            report({Severity::Info, QStringLiteral("Operation cancelled."), duration});
            break;
        case OperationResult::Status::Failed:
            report({Severity::Error, result.error ? result.error->message
                                                  : QStringLiteral("Operation failed."), duration});
            break;
        }
        if (result.protocol) {
            result.protocol->logs = std::move(captured.logs);
            result.protocol->logsOmitted = captured.logsOmitted;
        }
        QMetaObject::invokeMethod(this, [this, id, result = std::move(result)] {
            m_active = {};
            emit finished(id, result);
        }, Qt::QueuedConnection);
    });
    return id;
}

bool OperationRunner::cancel(const QUuid &id)
{
    Q_ASSERT(QThread::currentThread() == thread());
    return !id.isNull() && id == m_active && m_stop.request_stop();
}

}  // namespace dingobench::dicom
