// SPDX-License-Identifier: GPL-3.0-only
#include "FindExchange.h"
#include "ClientAssociation.h"
#include "dingobench/dicom/Verification.h"

#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/assoc.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/ofstd/ofstd.h>

#include <QDate>
#include <QRegularExpression>
#include <memory>

namespace dingobench::dicom {
namespace {

using logging::Severity;

OperationResult exchange(T_ASC_Association &association, ClientPhase &phase, int timeout,
    DcmDataset &identifier, const char *sopClass, const QString &model, const ReportDiagnostic &report,
    DiagnosticSession &session, const std::function<void(QueryMatch)> &onMatch)
{
    constexpr T_ASC_PresentationContextID context = 1;
    constexpr size_t commandLimit = 1024 * 1024;
    constexpr size_t identifierLimit = 8 * 1024 * 1024;
    const auto stop = phase.stop;
    int count = 0, warnings = 0;
    bool capped = false, cancelling = false;
    const auto summary = [&] {
        return QString("%1 · %2 matching response(s) · %3 optional-key warning(s)")
            .arg(model).arg(count).arg(warnings);
    };
    const auto cancelled = [&](bool acknowledged) {
        phase.release = acknowledged;
        auto result = OperationResult::cancelled();
        result.details = summary() + (capped ? " · Result limit reached" : " · Cancelled") +
            (acknowledged ? " · Peer confirmed C-CANCEL" : " · No cancel acknowledgement");
        if (acknowledged)
            result.dicomStatus = STATUS_FIND_Cancel;
        report({Severity::Warning, result.details});
        return result;
    };
    const auto fail = [&](ErrorCode code, const QString &message, const QString &technical = {}) {
        phase.release = false;
        session.technicalError = technical;
        auto result = OperationResult::failed({code, message});
        result.details = summary();
        return result;
    };
    const auto ioFailure = [&](const OFCondition &condition) {
        if (condition == DUL_PEERABORTEDASSOCIATION)
            session.association.closure = "Peer A-ABORT reported by DCMTK";
        if (stop.stop_requested() || cancelling)
            return cancelled(false);
        if (phase.readLimitReached)
            return fail(ErrorCode::ProtocolFailure, "C-FIND response exceeded the diagnostic size limit.");
        if (phase.expired || condition == DIMSE_NODATAAVAILABLE || condition == DUL_READTIMEOUT)
            return fail(ErrorCode::DimseTimeout, "DIMSE timed out waiting for C-FIND.");
        return fail(ErrorCode::ProtocolFailure, "C-FIND did not receive a valid response.", conditionText(condition));
    };
    if (session.association.contexts.first().acceptedSyntax.isEmpty())
        return fail(ErrorCode::PresentationContextRejected, "The peer did not accept the FIND presentation context for " + model + u'.');

    T_DIMSE_Message request{};
    request.CommandField = DIMSE_C_FIND_RQ;
    auto &find = request.msg.CFindRQ;
    find.MessageID = association.nextMsgID++;
    find.DataSetType = DIMSE_DATASET_PRESENT;
    find.Priority = DIMSE_PRIORITY_MEDIUM;
    OFStandard::strlcpy(find.AffectedSOPClassUID, sopClass,
                       sizeof(find.AffectedSOPClassUID));
    const auto send = [&](T_DIMSE_Message &message, DcmDataset *dataset) {
        DcmDataset *raw = nullptr;
        const auto condition = DIMSE_sendMessageUsingMemoryData(&association, context, &message, nullptr,
                                                               dataset, nullptr, nullptr, &raw);
        const std::unique_ptr<DcmDataset> command(raw);
        if (condition.good() && command)
            session.captureCommand(*command, Direction::Outgoing, QDateTime::currentDateTimeUtc(), context, dataset);
        return condition;
    };
    phase.begin(timeout);
    auto condition = send(request, &identifier);
    if (condition.bad())
        return ioFailure(condition);
    session.counts = {{"Matching responses delivered", 0}, {"Optional-key warnings", 0}};
    report({Severity::Info, "Sent C-FIND for " + model + u'.'});
    for (;;) {
        if (!cancelling && (stop.stop_requested() || capped)) {
            cancelling = true;
            phase.stop = {};
            phase.begin(1);
            T_DIMSE_Message cancel{};
            cancel.CommandField = DIMSE_C_CANCEL_RQ;
            cancel.msg.CCancelRQ.MessageIDBeingRespondedTo = find.MessageID;
            cancel.msg.CCancelRQ.DataSetType = DIMSE_DATASET_NULL;
            if (send(cancel, nullptr).bad())
                return cancelled(false);
        } else if (!cancelling) {
            phase.begin(timeout);
        }
        phase.limitRead(commandLimit);
        T_DIMSE_Message response{};
        T_ASC_PresentationContextID receivedContext = 0;
        DcmDataset *rawCommand = nullptr, *rawDetail = nullptr;
        condition = DIMSE_receiveCommand(&association, DIMSE_NONBLOCKING, cancelling ? 1 : timeout,
                                         &receivedContext, &response, &rawDetail, &rawCommand);
        const std::unique_ptr<DcmDataset> command(rawCommand), detail(rawDetail);
        // Let DCMTK consume buffered PDVs before waiting for new socket data.
        // If a partial read breaks framing, close rather than attempting recovery.
        if (!cancelling && stop.stop_requested() && condition == DIMSE_NODATAAVAILABLE)
            continue;
        if (condition.bad())
            return ioFailure(condition);

        const auto capture = [&](DcmDataset *dataset = nullptr) {
            if (command)
                session.captureCommand(*command, Direction::Incoming, QDateTime::currentDateTimeUtc(),
                                       receivedContext, dataset, detail.get());
        };
        const auto &reply = response.msg.CFindRSP;
        if (response.CommandField != DIMSE_C_FIND_RSP || receivedContext != context ||
            reply.MessageIDBeingRespondedTo != find.MessageID ||
            ((reply.opts & O_FIND_AFFECTEDSOPCLASSUID) &&
             OFString(reply.AffectedSOPClassUID) != sopClass)) {
            capture();
            return fail(ErrorCode::ProtocolFailure, "C-FIND response did not match the request.");
        }
        const auto status = reply.DimseStatus;
        const bool pending = status == STATUS_FIND_Pending_MatchesAreContinuing ||
                             status == STATUS_FIND_Pending_WarningUnsupportedOptionalKeys;
        if (pending != (reply.DataSetType != DIMSE_DATASET_NULL)) {
            capture();
            return fail(ErrorCode::ProtocolFailure, "C-FIND response had an invalid identifier flag.");
        }
        if (pending) {
            phase.limitRead(identifierLimit);
            DcmDataset *raw = nullptr;
            T_ASC_PresentationContextID datasetContext = 0;
            condition = DIMSE_receiveDataSetInMemory(&association, DIMSE_NONBLOCKING, cancelling ? 1 : timeout,
                                                      &datasetContext, &raw, nullptr, nullptr);
            const std::unique_ptr<DcmDataset> dataset(raw);
            if (condition.bad())
                return ioFailure(condition);
            phase.limitRead();
            if (!dataset || datasetContext != context) {
                capture();
                return fail(ErrorCode::ProtocolFailure, "C-FIND identifier used the wrong presentation context.");
            }
            if (!cancelling && !stop.stop_requested()) {
                if (count == maximumQueryMatches) {
                    capped = true;
                } else {
                    capture(dataset.get());
                    ++count;
                    warnings += status == STATUS_FIND_Pending_WarningUnsupportedOptionalKeys;
                    session.counts["Matching responses delivered"] = count;
                    session.counts["Optional-key warnings"] = warnings;
                    if (onMatch)
                        onMatch({snapshotDataset(*dataset), status});
                }
            }
            continue;
        }

        capture();
        phase.limitRead();
        if (cancelling || stop.stop_requested()) {
            auto result = cancelled(status == STATUS_FIND_Cancel);
            result.dicomStatus = status;
            return result;
        }
        OperationResult result;
        if (status == STATUS_FIND_Cancel) {
            result = OperationResult::cancelled();
        } else if (status != STATUS_FIND_Success) {
            result = OperationResult::failed({ErrorCode::FindFailure,
                "C-FIND failed: " + statusDescription(status) + u'.'});
        }
        result.dicomStatus = status;
        result.details = summary() + " · " + formatDicomStatus(status);
        report({result.error || warnings || status == STATUS_FIND_Cancel ? Severity::Warning : Severity::Info,
                result.details});
        return result;
    }
}

}  // namespace

bool validFindDate(const QString &value)
{
    static const QRegularExpression pattern(R"(\A(?:[0-9]{8}(?:-(?:[0-9]{8})?)?|-[0-9]{8})\z)");
    if (!pattern.match(value).hasMatch())
        return false;
    const auto bounds = value.split(u'-');
    QDate previous;
    for (const auto &bound : bounds) {
        if (bound.isEmpty())
            continue;
        const QDate date = QDate::fromString(bound, "yyyyMMdd");
        if (bound.size() != 8 || !date.isValid() || (previous.isValid() && date < previous))
            return false;
        previous = date;
    }
    return true;
}

bool validFindText(const QString &value, int max)
{
    for (const auto ch : value) {
        if (ch == u'\\' || ch.category() == QChar::Other_Control)
            return false;
    }
    return value.toUcs4().size() <= max;
}

QString findPersonNameError(const QString &value)
{
    const auto groups = value.split(u'=');
    if (groups.size() > 3)
        return "A DICOM Patient Name can have up to three representation groups.";
    for (const auto &group : groups) {
        if (!validFindText(group, 64) || group.count(u'^') > 4)
            return "Use DICOM Patient Name syntax with at most five components per group and 64 characters per group.";
    }
    return {};
}

bool validFindModality(const QString &value)
{
    static const QRegularExpression modality(R"(\A[A-Z0-9_ ]{1,16}\z)");
    return modality.match(value).hasMatch();
}

OperationResult executeFind(const profiles::Profile &source, const profiles::Profile &destination,
    const char *sopClass, const QString &model, DcmDataset &identifier, ClientTimeouts timeouts,
    bool detailedCapture, std::stop_token stop, const ReportDiagnostic &report,
    const std::function<void(QueryMatch)> &onMatch)
{
    DiagnosticSession session;
    session.detailedCapture = detailedCapture;
    session.association.contexts.append({1, QString::fromLatin1(sopClass), {},
        {UID_LittleEndianImplicitTransferSyntax, UID_LittleEndianExplicitTransferSyntax}});
    auto result = withClientAssociation(source, destination, timeouts, stop, report, session, "C-FIND",
        [&](T_ASC_Association &association, ClientPhase &phase) {
            return exchange(association, phase, timeouts.dimse, identifier, sopClass, model, report, session, onMatch);
        });
    result.protocol = std::move(session);
    return result;
}

}  // namespace dingobench::dicom
