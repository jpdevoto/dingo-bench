// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/GetRetrieve.h"
#include "ClientAssociation.h"
#include "ReceivedObject.h"
#include "RetrieveIdentifier.h"
#include "dingobench/dicom/StorageReceive.h"
#include "dingobench/dicom/Verification.h"

#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/assoc.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/ofstd/ofstd.h>

#include <QDir>
#include <QTemporaryDir>

namespace dingobench::dicom {
namespace {

using logging::Severity;
constexpr auto getSopClass = UID_GETStudyRootQueryRetrieveInformationModel;
constexpr T_ASC_PresentationContextID getContext = 1;

bool storageRole(T_ASC_SC_ROLE role)
{
    return role == ASC_SC_ROLE_SCP || role == ASC_SC_ROLE_SCUSCP;
}

void updateArchiveCounts(GetProgress &progress, const T_DIMSE_C_GetRSP &response)
{
    const auto field = [flags = response.opts](unsigned bit, quint16 count) -> std::optional<quint16> {
        return flags & bit ? std::optional<quint16>(count) : std::nullopt;
    };
    progress.status = response.DimseStatus;
    progress.remaining = field(O_GET_NUMBEROFREMAININGSUBOPERATIONS, response.NumberOfRemainingSubOperations);
    progress.completed = field(O_GET_NUMBEROFCOMPLETEDSUBOPERATIONS, response.NumberOfCompletedSubOperations);
    progress.failed = field(O_GET_NUMBEROFFAILEDSUBOPERATIONS, response.NumberOfFailedSubOperations);
    progress.warnings = field(O_GET_NUMBEROFWARNINGSUBOPERATIONS, response.NumberOfWarningSubOperations);
}

OperationResult exchangeGet(T_ASC_Association &association, ClientPhase &phase, int timeout, DcmDataset &identifier,
    const QString &output, const QString &stage, DiagnosticSession &session, const ReportDiagnostic &report,
    const std::function<void(GetProgress)> &onProgress)
{
    if (session.association.contexts.first().acceptedSyntax.isEmpty())
        return OperationResult::failed({ErrorCode::PresentationContextRejected,
            "The archive did not accept Study Root C-GET. Check that C-GET is enabled on the archive."});

    int accepted = 0, usable = 0;
    for (const auto &proposal : session.association.contexts) {
        T_ASC_PresentationContext context{};
        if (proposal.requestStorageScp &&
            ASC_findAcceptedPresentationContext(association.params, T_ASC_PresentationContextID(proposal.id), &context).good()) {
            ++accepted;
            usable += storageRole(context.acceptedRole);
        }
    }
    report({usable ? Severity::Info : Severity::Warning,
        QString("C-GET storage contexts with requestor SCP role: %1 of %2 proposed.").arg(usable)
            .arg(session.association.contexts.size() - 1)});
    if (!usable)
        return accepted ? OperationResult::failed({ErrorCode::StorageRoleRejected,
            "Storage contexts were accepted without the requestor SCP role. The archive cannot return objects on this association."})
            : OperationResult::failed({ErrorCode::PresentationContextRejected,
                "The archive accepted C-GET but rejected all proposed Storage SOP Class or transfer syntax contexts."});

    GetProgress progress;
    const auto stop = phase.stop;
    bool cancelSent = false;
    const auto localSummary = [&] {
        return QString("%1 local file(s) saved · %2 local C-STORE failure(s)")
            .arg(progress.filesSaved).arg(progress.storesFailed);
    };
    const auto fail = [&](ErrorCode code, const QString &message, const QString &technical = {}) {
        phase.release = false;
        session.technicalError = technical;
        auto result = OperationResult::failed({code, message});
        result.details = localSummary();
        return result;
    };
    const auto cancelled = [&](std::optional<quint16> status = std::nullopt) {
        phase.release = status.has_value();
        auto result = OperationResult::cancelled();
        result.dicomStatus = status;
        result.details = localSummary() + (status == STATUS_GET_Cancel ? " · Archive confirmed cancellation"
            : status ? " · Archive sent a final response during local cancellation"
                     : " · Local association closed without a cancel acknowledgement");
        report({Severity::Warning, result.details});
        return result;
    };
    const auto ioFailure = [&](const OFCondition &condition) {
        if (condition == DUL_PEERABORTEDASSOCIATION)
            session.association.closure = "Peer A-ABORT reported by DCMTK";
        if (stop.stop_requested())
            return cancelled();
        if (phase.readLimitReached)
            return fail(ErrorCode::ProtocolFailure, "Incoming C-GET data exceeded the diagnostic size limit.");
        if (phase.expired || condition == DIMSE_NODATAAVAILABLE || condition == DUL_READTIMEOUT)
            return fail(ErrorCode::DimseTimeout, "DIMSE timed out during C-GET.");
        return fail(ErrorCode::ProtocolFailure, "C-GET could not complete the DIMSE exchange.", conditionText(condition));
    };
    const auto send = [&](T_ASC_PresentationContextID context, T_DIMSE_Message &message, DcmDataset *dataset = nullptr) {
        DcmDataset *raw = nullptr;
        const auto condition = DIMSE_sendMessageUsingMemoryData(&association, context, &message, nullptr,
                                                               dataset, nullptr, nullptr, &raw);
        const std::unique_ptr<DcmDataset> command(raw);
        if (condition.good() && command)
            session.captureCommand(*command, Direction::Outgoing, QDateTime::currentDateTimeUtc(), context, dataset);
        return condition;
    };

    T_DIMSE_Message outgoing{};
    outgoing.CommandField = DIMSE_C_GET_RQ;
    auto &get = outgoing.msg.CGetRQ;
    get.MessageID = association.nextMsgID++;
    get.Priority = DIMSE_PRIORITY_MEDIUM;
    get.DataSetType = DIMSE_DATASET_PRESENT;
    OFStandard::strlcpy(get.AffectedSOPClassUID, getSopClass, sizeof(get.AffectedSOPClassUID));
    phase.begin(timeout);
    auto condition = send(getContext, outgoing, &identifier);
    if (condition.bad())
        return ioFailure(condition);
    report({Severity::Info, "Sent Study Root C-GET. C-STORE objects are received on this association."});

    for (;;) {
        if (!cancelSent && stop.stop_requested()) {
            cancelSent = true;
            phase.stop = {};
            phase.begin(1);
            T_DIMSE_Message cancel{};
            cancel.CommandField = DIMSE_C_CANCEL_RQ;
            cancel.msg.CCancelRQ.MessageIDBeingRespondedTo = get.MessageID;
            cancel.msg.CCancelRQ.DataSetType = DIMSE_DATASET_NULL;
            if (send(getContext, cancel).bad())
                return cancelled();
        } else if (!cancelSent) {
            phase.begin(timeout);
        }
        phase.limitRead(1024 * 1024);
        T_DIMSE_Message incoming{};
        T_ASC_PresentationContextID context = 0;
        DcmDataset *rawCommand = nullptr, *rawDetail = nullptr;
        condition = DIMSE_receiveCommand(&association, DIMSE_NONBLOCKING, cancelSent ? 1 : timeout,
                                         &context, &incoming, &rawDetail, &rawCommand);
        const std::unique_ptr<DcmDataset> command(rawCommand), detail(rawDetail);
        if (!cancelSent && stop.stop_requested() && condition == DIMSE_NODATAAVAILABLE)
            continue;
        if (condition.bad())
            return ioFailure(condition);
        const auto capture = [&](DcmDataset *dataset = nullptr) {
            if (command)
                session.captureCommand(*command, Direction::Incoming, QDateTime::currentDateTimeUtc(),
                                       context, dataset, detail.get());
        };
        const auto receiveDataset = [&](std::unique_ptr<DcmDataset> &dataset, size_t limit) {
            phase.limitRead(limit);
            DcmDataset *raw = nullptr;
            T_ASC_PresentationContextID datasetContext = 0;
            auto read = DIMSE_receiveDataSetInMemory(&association, DIMSE_NONBLOCKING, cancelSent ? 1 : timeout,
                                                     &datasetContext, &raw, nullptr, nullptr);
            dataset.reset(raw);
            if (read.good() && (!dataset || datasetContext != context))
                read = DIMSE_NOVALIDPRESENTATIONCONTEXTID;
            return read;
        };
        if (incoming.CommandField == DIMSE_C_STORE_RQ) {
            const auto &store = incoming.msg.CStoreRQ;
            T_ASC_PresentationContext negotiated{};
            capture();
            if (context == getContext || store.DataSetType == DIMSE_DATASET_NULL ||
                ASC_findAcceptedPresentationContext(association.params, context, &negotiated).bad() ||
                !dcmIsaStorageSOPClassUID(negotiated.abstractSyntax, ESSC_All) ||
                OFString(negotiated.abstractSyntax) != store.AffectedSOPClassUID ||
                !storageRole(negotiated.acceptedRole))
                return fail(ErrorCode::ProtocolFailure, "Incoming C-STORE did not match a negotiated Storage SCP context.");
            if (!cancelSent)
                phase.begin(timeout);
            std::unique_ptr<DcmDataset> dataset;
            condition = receiveDataset(dataset, datasetWireLimit);
            if (condition.bad())
                return ioFailure(condition);
            const auto outcome = saveReceived(std::move(dataset), store, output, stage, stop);
            if (outcome.status == STATUS_STORE_Success)
                ++progress.filesSaved;
            else
                ++progress.storesFailed;
            report({outcome.status == STATUS_STORE_Success ? Severity::Debug : Severity::Warning, outcome.message,
                    "C-STORE status " + formatDicomStatus(outcome.status)});
            if (onProgress)
                onProgress(progress);

            T_DIMSE_Message response{};
            response.CommandField = DIMSE_C_STORE_RSP;
            auto &reply = response.msg.CStoreRSP;
            reply.MessageIDBeingRespondedTo = store.MessageID;
            reply.DataSetType = DIMSE_DATASET_NULL;
            reply.DimseStatus = outcome.status;
            reply.opts = O_STORE_AFFECTEDSOPCLASSUID | O_STORE_AFFECTEDSOPINSTANCEUID;
            OFStandard::strlcpy(reply.AffectedSOPClassUID, store.AffectedSOPClassUID, sizeof(reply.AffectedSOPClassUID));
            OFStandard::strlcpy(reply.AffectedSOPInstanceUID, store.AffectedSOPInstanceUID,
                                sizeof(reply.AffectedSOPInstanceUID));
            if (!cancelSent) {
                // Finish the response for a fully read object before attempting C-CANCEL.
                const bool stopping = stop.stop_requested();
                if (stopping)
                    phase.stop = {};
                phase.begin(stopping ? 1 : timeout);
            }
            condition = send(context, response);
            if (!cancelSent)
                phase.stop = stop;
            if (condition.bad())
                return ioFailure(condition);
            continue;
        }

        const auto &reply = incoming.msg.CGetRSP;
        if (incoming.CommandField != DIMSE_C_GET_RSP || context != getContext ||
            reply.MessageIDBeingRespondedTo != get.MessageID ||
            ((reply.opts & O_GET_AFFECTEDSOPCLASSUID) && OFString(reply.AffectedSOPClassUID) != getSopClass)) {
            capture();
            return fail(ErrorCode::ProtocolFailure, "C-GET response did not match the request.");
        }
        const auto status = reply.DimseStatus;
        const bool pending = status == STATUS_GET_Pending_SubOperationsAreContinuing;
        std::unique_ptr<DcmDataset> dataset;
        if (reply.DataSetType != DIMSE_DATASET_NULL) {
            if (pending || status == STATUS_GET_Success) {
                capture();
                return fail(ErrorCode::ProtocolFailure, "A pending or successful C-GET response contained an unexpected identifier.");
            }
            condition = receiveDataset(dataset, 8 * 1024 * 1024);
            if (condition.bad())
                return ioFailure(condition);
        }
        capture(dataset.get());
        phase.limitRead();
        updateArchiveCounts(progress, reply);
        if (onProgress)
            onProgress(progress);
        if (pending)
            continue;
        if (stop.stop_requested())
            return cancelled(status);

        OperationResult result;
        if (status == STATUS_GET_Cancel) {
            result = OperationResult::cancelled();
        } else if (status == STATUS_GET_Warning_SubOperationsCompleteOneOrMoreFailures ||
                   (status == STATUS_GET_Success && (progress.storesFailed || progress.failed.value_or(0) ||
                                                    progress.warnings.value_or(0)))) {
            result.status = OperationResult::Status::Warning;
        } else if (status != STATUS_GET_Success) {
            result = OperationResult::failed({ErrorCode::GetFailure, getStatusText(status)});
        }
        result.dicomStatus = status;
        const auto showCount = [](std::optional<quint16> count) {
            return count ? QString::number(*count) : QString("not reported");
        };
        result.details = QString("%1 · %2 · archive reported %3 completed, %4 failed, %5 warning · %6")
            .arg(formatDicomStatus(status), getStatusText(status), showCount(progress.completed),
                 showCount(progress.failed), showCount(progress.warnings), localSummary());
        report({result.status == OperationResult::Status::Succeeded ? Severity::Info : Severity::Warning, result.details});
        return result;
    }
}

}  // namespace

QString getStatusText(quint16 status)
{
    switch (status) {
    case STATUS_GET_Success: return "Archive reports completion";
    case STATUS_GET_Pending_SubOperationsAreContinuing: return "Archive is performing C-STORE sub-operations";
    case STATUS_GET_Cancel: return "Archive cancelled the remaining sub-operations";
    case STATUS_GET_Warning_SubOperationsCompleteOneOrMoreFailures:
        return "Archive completed with failed or warning sub-operations";
    case STATUS_GET_Refused_OutOfResourcesNumberOfMatches: return "Archive could not calculate the number of matches";
    case STATUS_GET_Refused_OutOfResourcesSubOperations:
        return "Archive could not perform C-STORE sub-operations. Check negotiated Storage contexts and local write failures.";
    case STATUS_GET_Error_DataSetDoesNotMatchSOPClass: return "Archive rejected the retrieval identifier";
    case STATUS_GET_Refused_SOPClassNotSupported: return "Archive does not support Study Root C-GET";
    default: return "Archive could not complete C-GET (" + statusDescription(status) + u')';
    }
}

Operation getOperation(profiles::Profile source, profiles::Profile archive, GetRequest request,
                       ClientTimeouts timeouts, bool detailedCapture, std::function<void(GetProgress)> onProgress)
{
    return [source = std::move(source), archive = std::move(archive), request = std::move(request), timeouts,
            detailedCapture, onProgress = std::move(onProgress)](std::stop_token stop, const ReportDiagnostic &report) {
        if (!validRetrieveKeys(request.studyUid, request.seriesUid))
            return OperationResult::failed({ErrorCode::InvalidInput, "Select a study or series with valid exact UIDs."});
        const auto output = receiveDirectory(request.directory);
        if (output.isEmpty())
            return OperationResult::failed({ErrorCode::InvalidInput, "Choose an existing output directory outside Git checkouts."});
        QTemporaryDir stage(QDir(output).filePath(".dingobench-receive-XXXXXX"));
        if (!stage.isValid() || !stagingAllowed(output, stage.path()))
            return OperationResult::failed({ErrorCode::InvalidInput, "Cannot create a staging directory in the selected output directory."});
        DcmDataset identifier;
        if (buildRetrieveIdentifier(identifier, request.studyUid, request.seriesUid).bad())
            return OperationResult::failed({ErrorCode::Internal, "Could not construct the C-GET identifier."});

        const QStringList syntaxes{UID_LittleEndianExplicitTransferSyntax, UID_LittleEndianImplicitTransferSyntax,
                                   UID_BigEndianExplicitTransferSyntax};
        DiagnosticSession session;
        session.detailedCapture = detailedCapture;
        session.association.contexts.append({getContext, getSopClass, {}, syntaxes});
        for (int i = 0; i < numberOfDcmLongSCUStorageSOPClassUIDs; ++i) {
            PresentationContext storage{3 + i * 2, QString::fromLatin1(dcmLongSCUStorageSOPClassUIDs[i]), {}, syntaxes};
            storage.requestStorageScp = true;
            session.association.contexts.append(std::move(storage));
        }
        auto result = withClientAssociation(source, archive, timeouts, stop, report, session, "C-GET",
            [&](T_ASC_Association &association, ClientPhase &phase) {
                return exchangeGet(association, phase, timeouts.dimse, identifier, output, stage.path(),
                                   session, report, onProgress);
            });
        result.protocol = std::move(session);
        return result;
    };
}

}  // namespace dingobench::dicom
