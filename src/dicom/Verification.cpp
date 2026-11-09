// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/Verification.h"
#include "ClientAssociation.h"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/assoc.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/ofstd/ofstd.h>

#include <memory>
#include <utility>

namespace dingobench::dicom {
namespace {

using logging::Severity;

OperationResult echo(T_ASC_Association &association, ClientPhase &phase, ClientTimeouts timeouts,
                     const ReportDiagnostic &report, DiagnosticSession &session)
{
    constexpr T_ASC_PresentationContextID contextId = 1;
    const auto fail = [&](ErrorCode code, const QString &message, const QString &technical = {}) {
        phase.release = false;
        session.technicalError = technical;
        auto result = OperationResult::failed({code, message});
        result.details = technical;
        return result;
    };
    if (session.association.contexts.first().acceptedSyntax.isEmpty())
        return fail(ErrorCode::PresentationContextRejected,
                    "The peer did not accept the Verification presentation context.",
                    session.association.contexts.first().outcome);
    const auto stop = phase.stop;
    phase.begin(timeouts.dimse);
    OFCondition condition = EC_Normal;
    T_DIMSE_Message request{}, response{};
    request.CommandField = DIMSE_C_ECHO_RQ;
    request.msg.CEchoRQ.MessageID = association.nextMsgID++;
    request.msg.CEchoRQ.DataSetType = DIMSE_DATASET_NULL;
    OFStandard::strlcpy(request.msg.CEchoRQ.AffectedSOPClassUID, UID_VerificationSOPClass,
                        sizeof(request.msg.CEchoRQ.AffectedSOPClassUID));
    DcmDataset *sentSet = nullptr;
    condition = DIMSE_sendMessageUsingMemoryData(&association, contextId, &request, nullptr,
                                                nullptr, nullptr, nullptr, &sentSet);
    const std::unique_ptr<DcmDataset> sent(sentSet);
    T_ASC_PresentationContextID responseContext = 0;
    if (condition.good()) {
        if (sent)
            session.captureCommand(*sent, Direction::Outgoing, QDateTime::currentDateTimeUtc(), contextId);
        DcmDataset *receivedSet = nullptr, *rawStatusDetail = nullptr;
        condition = DIMSE_receiveCommand(&association, DIMSE_NONBLOCKING, timeouts.dimse,
                                         &responseContext, &response, &rawStatusDetail, &receivedSet);
        const std::unique_ptr<DcmDataset> received(receivedSet), statusDetail(rawStatusDetail);
        if (received)
            session.captureCommand(*received, Direction::Incoming, QDateTime::currentDateTimeUtc(),
                                   responseContext, nullptr, statusDetail.get());
    }
    if (condition == DUL_PEERABORTEDASSOCIATION)
        session.association.closure = "Peer A-ABORT reported by DCMTK";
    if (stop.stop_requested())
        return OperationResult::cancelled();
    if (phase.expired || condition == DIMSE_NODATAAVAILABLE || condition == DUL_READTIMEOUT)
        return fail(ErrorCode::DimseTimeout, "DIMSE timed out waiting for the C-ECHO response.");
    if (condition.bad())
        return fail(ErrorCode::ProtocolFailure, "C-ECHO did not receive a valid response.", conditionText(condition));
    if (response.CommandField != DIMSE_C_ECHO_RSP || responseContext != contextId ||
        response.msg.CEchoRSP.MessageIDBeingRespondedTo != request.msg.CEchoRQ.MessageID ||
        response.msg.CEchoRSP.DataSetType != DIMSE_DATASET_NULL)
        return fail(ErrorCode::ProtocolFailure, "C-ECHO response did not match the request.");

    const auto status = response.msg.CEchoRSP.DimseStatus;
    const auto statusText = formatDicomStatus(status);
    report({status == STATUS_Success ? Severity::Info : Severity::Warning,
            "C-ECHO response " + statusText + u'.'});
    auto result = status == STATUS_Success ? OperationResult{}
        : OperationResult::failed({ErrorCode::EchoFailure, "C-ECHO returned failure status " + statusText + u'.'});
    result.details = "C-ECHO response status " + statusText + u'.';
    result.dicomStatus = status;
    return result;
}

}  // namespace

QString formatDicomStatus(quint16 status)
{
    return QStringLiteral("0x") + QString::number(status, 16).toUpper().rightJustified(4, u'0');
}

Operation verificationOperation(profiles::Profile source, profiles::Profile destination,
                                VerificationTimeouts timeouts, bool detailedCapture)
{
    return [source = std::move(source), destination = std::move(destination), timeouts, detailedCapture](
               std::stop_token stop, const ReportDiagnostic &report) {
        DiagnosticSession session;
        session.detailedCapture = detailedCapture;
        session.association.contexts.append({1, UID_VerificationSOPClass, {},
            {UID_LittleEndianImplicitTransferSyntax, UID_LittleEndianExplicitTransferSyntax}});
        auto result = withClientAssociation(source, destination, timeouts, stop, report, session, "C-ECHO",
            [&](T_ASC_Association &association, ClientPhase &phase) {
                return echo(association, phase, timeouts, report, session);
            });
        result.protocol = std::move(session);
        return result;
    };
}

}  // namespace dingobench::dicom
