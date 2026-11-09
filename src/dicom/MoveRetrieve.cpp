// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/MoveRetrieve.h"
#include "ClientAssociation.h"
#include "RetrieveIdentifier.h"
#include "dingobench/dicom/Verification.h"

#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/assoc.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/ofstd/ofstd.h>

#include <QRandomGenerator>
#include <atomic>
#include <memory>

namespace dingobench::dicom {
namespace {

using logging::Severity;
constexpr auto moveSopClass = UID_MOVEStudyRootQueryRetrieveInformationModel;

MoveProgress progressFor(const T_DIMSE_C_MoveRSP &reply)
{
    const auto readCount = [flags = reply.opts](unsigned flag, quint16 count) -> std::optional<quint16> {
        if (flags & flag)
            return count;
        return std::nullopt;
    };
    return {reply.DimseStatus, readCount(O_MOVE_NUMBEROFREMAININGSUBOPERATIONS, reply.NumberOfRemainingSubOperations),
        readCount(O_MOVE_NUMBEROFCOMPLETEDSUBOPERATIONS, reply.NumberOfCompletedSubOperations),
        readCount(O_MOVE_NUMBEROFFAILEDSUBOPERATIONS, reply.NumberOfFailedSubOperations),
        readCount(O_MOVE_NUMBEROFWARNINGSUBOPERATIONS, reply.NumberOfWarningSubOperations)};
}

OperationResult exchangeMove(T_ASC_Association &association, ClientPhase &phase, int timeout,
    const MoveRequest &request, DcmDataset &identifier, const ReportDiagnostic &report,
    DiagnosticSession &session, const std::function<void(MoveProgress)> &onProgress)
{
    constexpr T_ASC_PresentationContextID context = 1;
    constexpr size_t commandLimit = 1024 * 1024;
    constexpr size_t identifierLimit = 8 * 1024 * 1024;
    const auto stop = phase.stop;
    bool cancelSent = false;
    const auto fail = [&](ErrorCode code, const QString &message, const QString &technical = {}) {
        phase.release = false;
        session.technicalError = technical;
        return OperationResult::failed({code, message});
    };
    const auto cancelled = [&](std::optional<quint16> status = std::nullopt) {
        phase.release = status.has_value();
        auto result = OperationResult::cancelled();
        result.dicomStatus = status;
        result.details = status == STATUS_MOVE_Cancel ? "The archive confirmed cancellation."
            : status ? "The archive sent a final response during local cancellation."
                     : "Local wait ended without archive cancellation acknowledgement. C-STORE may continue.";
        report({Severity::Warning, result.details});
        return result;
    };
    const auto ioFailure = [&](const OFCondition &condition) {
        if (condition == DUL_PEERABORTEDASSOCIATION)
            session.association.closure = "Peer A-ABORT reported by DCMTK";
        if (stop.stop_requested())
            return cancelled();
        if (phase.readLimitReached)
            return fail(ErrorCode::ProtocolFailure, "C-MOVE response exceeded the diagnostic size limit.");
        if (phase.expired || condition == DIMSE_NODATAAVAILABLE || condition == DUL_READTIMEOUT)
            return fail(ErrorCode::DimseTimeout, "DIMSE timed out waiting for C-MOVE. The archive may still be sending C-STORE.");
        return fail(ErrorCode::ProtocolFailure, "C-MOVE could not complete the DIMSE exchange.", conditionText(condition));
    };
    if (session.association.contexts.first().acceptedSyntax.isEmpty())
        return fail(ErrorCode::PresentationContextRejected, "The peer did not accept the Study Root MOVE context.");

    const auto send = [&](T_DIMSE_Message &message, DcmDataset *dataset = nullptr) {
        DcmDataset *raw = nullptr;
        const auto condition = DIMSE_sendMessageUsingMemoryData(&association, context, &message, nullptr,
                                                               dataset, nullptr, nullptr, &raw);
        const std::unique_ptr<DcmDataset> command(raw);
        if (condition.good() && command)
            session.captureCommand(*command, Direction::Outgoing, QDateTime::currentDateTimeUtc(), context, dataset);
        return condition;
    };
    T_DIMSE_Message outgoing{};
    outgoing.CommandField = DIMSE_C_MOVE_RQ;
    auto &move = outgoing.msg.CMoveRQ;
    move.MessageID = request.messageId;
    move.Priority = DIMSE_PRIORITY_MEDIUM;
    move.DataSetType = DIMSE_DATASET_PRESENT;
    OFStandard::strlcpy(move.AffectedSOPClassUID, moveSopClass, sizeof(move.AffectedSOPClassUID));
    const auto destinationAe = request.destinationAe.trimmed();
    OFStandard::strlcpy(move.MoveDestination, destinationAe.toLatin1().constData(), sizeof(move.MoveDestination));
    phase.begin(timeout);
    auto condition = send(outgoing, &identifier);
    if (condition.bad())
        return ioFailure(condition);
    report({Severity::Info, QString("Sent Study Root C-MOVE to move destination AE %1.").arg(destinationAe),
            QString("Message ID %1 · %2 level").arg(request.messageId).arg(request.seriesUid.isEmpty() ? "STUDY" : "SERIES")});

    for (;;) {
        if (!cancelSent && stop.stop_requested()) {
            cancelSent = true;
            phase.stop = {};
            phase.begin(1);
            T_DIMSE_Message cancel{};
            cancel.CommandField = DIMSE_C_CANCEL_RQ;
            cancel.msg.CCancelRQ.MessageIDBeingRespondedTo = request.messageId;
            cancel.msg.CCancelRQ.DataSetType = DIMSE_DATASET_NULL;
            if (send(cancel).bad())
                return cancelled();
        } else if (!cancelSent) {
            phase.begin(timeout);
        }
        phase.limitRead(commandLimit);
        T_DIMSE_Message incoming{};
        T_ASC_PresentationContextID receivedContext = 0;
        DcmDataset *rawCommand = nullptr, *rawDetail = nullptr;
        condition = DIMSE_receiveCommand(&association, DIMSE_NONBLOCKING, cancelSent ? 1 : timeout,
                                         &receivedContext, &incoming, &rawDetail, &rawCommand);
        const std::unique_ptr<DcmDataset> command(rawCommand), detail(rawDetail);
        if (!cancelSent && stop.stop_requested() && condition == DIMSE_NODATAAVAILABLE)
            continue;
        if (condition.bad())
            return ioFailure(condition);

        const auto capture = [&](DcmDataset *dataset = nullptr) {
            if (command)
                session.captureCommand(*command, Direction::Incoming, QDateTime::currentDateTimeUtc(),
                                       receivedContext, dataset, detail.get());
        };
        const auto &reply = incoming.msg.CMoveRSP;
        if (incoming.CommandField != DIMSE_C_MOVE_RSP || receivedContext != context ||
            reply.MessageIDBeingRespondedTo != request.messageId ||
            ((reply.opts & O_MOVE_AFFECTEDSOPCLASSUID) && OFString(reply.AffectedSOPClassUID) != moveSopClass)) {
            capture();
            return fail(ErrorCode::ProtocolFailure, "C-MOVE response did not match the request.");
        }
        const auto status = reply.DimseStatus;
        const bool pending = status == STATUS_MOVE_Pending_SubOperationsAreContinuing;
        std::unique_ptr<DcmDataset> dataset;
        if (reply.DataSetType != DIMSE_DATASET_NULL) {
            if (pending || status == STATUS_MOVE_Success) {
                capture();
                return fail(ErrorCode::ProtocolFailure, "A pending or successful C-MOVE response contained an unexpected identifier.");
            }
            phase.limitRead(identifierLimit);
            DcmDataset *raw = nullptr;
            T_ASC_PresentationContextID datasetContext = 0;
            condition = DIMSE_receiveDataSetInMemory(&association, DIMSE_NONBLOCKING, cancelSent ? 1 : timeout,
                                                      &datasetContext, &raw, nullptr, nullptr);
            dataset.reset(raw);
            if (condition.bad())
                return ioFailure(condition);
            if (!dataset || datasetContext != context) {
                capture();
                return fail(ErrorCode::ProtocolFailure, "C-MOVE response identifier used the wrong presentation context.");
            }
        }
        capture(dataset.get());
        phase.limitRead();
        const auto progress = progressFor(reply);
        if (onProgress)
            onProgress(progress);
        if (pending)
            continue;
        if (stop.stop_requested())
            return cancelled(status);

        OperationResult result;
        if (status == STATUS_MOVE_Cancel) {
            result = OperationResult::cancelled();
        } else if (status == STATUS_MOVE_Warning_SubOperationsCompleteOneOrMoreFailures) {
            result.status = OperationResult::Status::Warning;
        } else if (status != STATUS_MOVE_Success) {
            result = OperationResult::failed({ErrorCode::MoveFailure, moveStatusText(status)});
        }
        result.dicomStatus = status;
        const auto show = [](std::optional<quint16> count) {
            return count ? QString::number(*count) : QString("not reported");
        };
        result.details = QString("%1 · %2 · archive reported %3 completed, %4 failed, %5 warning")
            .arg(formatDicomStatus(status), moveStatusText(status), show(progress.completed),
                 show(progress.failed), show(progress.warnings));
        report({status == STATUS_MOVE_Success ? Severity::Info : Severity::Warning, result.details});
        return result;
    }
}

}  // namespace

quint16 nextMoveMessageId()
{
    static std::atomic<quint32> sequence{QRandomGenerator::global()->bounded(65535u)};
    return quint16(sequence.fetch_add(1, std::memory_order_relaxed) % 65535u + 1);
}

QString validateMove(const MoveRequest &request)
{
    if (!validRetrieveKeys(request.studyUid, request.seriesUid))
        return "Select a study or series with valid exact Study and Series Instance UIDs.";
    if (!profiles::validAeTitle(request.destinationAe))
        return "Enter a Move Destination AE Title of 1–16 printable ASCII characters without backslashes.";
    if (!request.messageId)
        return "A C-MOVE message ID is required.";
    return {};
}

QString moveStatusText(quint16 status)
{
    switch (status) {
    case STATUS_MOVE_Success: return "Archive reports completion";
    case STATUS_MOVE_Pending_SubOperationsAreContinuing: return "Archive is performing C-STORE sub-operations";
    case STATUS_MOVE_Cancel: return "Archive cancelled the remaining sub-operations";
    case STATUS_MOVE_Warning_SubOperationsCompleteOneOrMoreFailures:
        return "Archive completed with failed or warning sub-operations";
    case STATUS_MOVE_Refused_OutOfResourcesNumberOfMatches: return "Archive could not calculate the number of matches";
    case STATUS_MOVE_Refused_OutOfResourcesSubOperations:
        return "Archive could not perform C-STORE sub-operations. Check target reachability, listener, and storage support.";
    case STATUS_MOVE_Refused_MoveDestinationUnknown:
        return "Archive reported an unknown Move Destination AE. Check its AE-to-host and port mapping.";
    case STATUS_MOVE_Error_DataSetDoesNotMatchSOPClass: return "Archive rejected the retrieval identifier";
    case STATUS_MOVE_Refused_NotAuthorized: return "Archive did not authorize this retrieval";
    case STATUS_MOVE_Refused_SOPClassNotSupported: return "Archive did not support Study Root C-MOVE";
    default: return "Archive could not complete C-MOVE (" + statusDescription(status) + u')';
    }
}

Operation moveOperation(profiles::Profile source, profiles::Profile archive, MoveRequest request,
                        ClientTimeouts timeouts, bool detailedCapture, std::function<void(MoveProgress)> onProgress)
{
    return [source = std::move(source), archive = std::move(archive), request = std::move(request),
            timeouts, detailedCapture, onProgress = std::move(onProgress)](
               std::stop_token stop, const ReportDiagnostic &report) {
        if (const auto error = validateMove(request); !error.isEmpty())
            return OperationResult::failed({ErrorCode::InvalidInput, error});
        DcmDataset identifier;
        if (buildRetrieveIdentifier(identifier, request.studyUid, request.seriesUid).bad())
            return OperationResult::failed({ErrorCode::Internal, "Could not construct the C-MOVE identifier."});

        DiagnosticSession session;
        session.detailedCapture = detailedCapture;
        session.association.contexts.append({1, moveSopClass, {},
            {UID_LittleEndianImplicitTransferSyntax, UID_LittleEndianExplicitTransferSyntax}});
        auto result = withClientAssociation(source, archive, timeouts, stop, report, session, "C-MOVE",
            [&](T_ASC_Association &association, ClientPhase &phase) {
                return exchangeMove(association, phase, timeouts.dimse, request, identifier, report, session, onProgress);
            });
        result.protocol = std::move(session);
        return result;
    };
}

}  // namespace dingobench::dicom
