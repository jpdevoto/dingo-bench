// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/StorageSend.h"
#include "ClientAssociation.h"
#include "DicomFileInput.h"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcistrma.h>
#include <dcmtk/dcmdata/dcmetinf.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmdata/dcvrui.h>
#include <dcmtk/dcmdata/dcxfer.h>
#include <dcmtk/dcmnet/assoc.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/ofstd/ofstd.h>

#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <memory>
#include <utility>

namespace dingobench::dicom {
namespace {

using logging::Severity;
using State = StorageFile::State;

StorageFile readFile(const QString &path, DcmFileFormat &format)
{
    StorageFile file;
    file.path = path;
    const QFileInfo info(path);
    file.size = info.size();
    file.modified = info.lastModified();
    if (!info.isFile() || !info.isReadable()) {
        file.message = "File is missing or unreadable.";
        return file;
    }
    if (readPart10(path, format).bad()) {
        file.message = "Invalid DICOM Part 10 file.";
        return file;
    }
    auto &dataset = *format.getDataset();
    auto &meta = *format.getMetaInfo();
    OFString sopClass, sopInstance, metaClass, metaInstance, metaSyntax;
    const auto uid = [](DcmItem &item, const DcmTagKey &tag, OFString &value) {
        return item.findAndGetOFStringArray(tag, value).good() && !value.empty() &&
            DcmUniqueIdentifier::checkStringValue(value, "1").good();
    };
    if (!uid(dataset, DCM_SOPClassUID, sopClass) || !uid(dataset, DCM_SOPInstanceUID, sopInstance) ||
        !uid(meta, DCM_MediaStorageSOPClassUID, metaClass) || !uid(meta, DCM_MediaStorageSOPInstanceUID, metaInstance) ||
        !uid(meta, DCM_TransferSyntaxUID, metaSyntax) || sopClass != metaClass || sopInstance != metaInstance) {
        file.message = "Missing, invalid, or inconsistent SOP and transfer syntax metadata.";
        return file;
    }
    file.sopClass = QString::fromLatin1(sopClass.c_str());
    file.sopInstance = QString::fromLatin1(sopInstance.c_str());
    file.transferSyntax = QString::fromLatin1(metaSyntax.c_str());
    const DcmXfer syntax(dataset.getOriginalXfer());
    if (syntax.getXfer() == EXS_Unknown || metaSyntax != syntax.getXferID() ||
        !dataset.canWriteXfer(syntax.getXfer(), syntax.getXfer())) {
        file.message = "Original transfer syntax cannot be sent by this DCMTK build.";
        return file;
    }
    if (!dcmIsaStorageSOPClassUID(sopClass.c_str(), ESSC_All)) {
        file.message = "SOP Class is not a recognized Storage SOP Class.";
        return file;
    }
    file.state = State::Ready;
    file.message = "Ready";
    return file;
}

QString contextKey(const StorageFile &file)
{
    return file.sopClass + u'|' + file.transferSyntax;
}

bool matchesValidated(const StorageFile &current, const StorageFile &previous)
{
    return current.state == State::Ready && current.size == previous.size &&
        current.modified == previous.modified && current.sopInstance == previous.sopInstance &&
        current.sopClass == previous.sopClass && current.transferSyntax == previous.transferSyntax;
}

struct WireProgress {
    StorageFile &file;
    const ReportStorageFile &update;
    QElapsedTimer throttle;
};

void progress(void *opaque, Uint64 bytes)
{
    auto &wire = *static_cast<WireProgress *>(opaque);
    wire.file.bytesSent = bytes;
    if (wire.throttle.elapsed() >= 100) {
        wire.update(wire.file);
        wire.throttle.restart();
    }
}

OperationResult sendOne(T_ASC_Association &association, ClientPhase &phase, int seconds, DcmDataset &dataset,
                        const PresentationContext &context, StorageFile &file,
                        DiagnosticSession &session, const ReportStorageFile &update)
{
    const auto contextId = static_cast<T_ASC_PresentationContextID>(context.id);
    T_DIMSE_Message request{}, response{};
    request.CommandField = DIMSE_C_STORE_RQ;
    auto &rq = request.msg.CStoreRQ;
    rq.MessageID = association.nextMsgID++;
    rq.Priority = DIMSE_PRIORITY_MEDIUM;
    rq.DataSetType = DIMSE_DATASET_PRESENT;
    OFStandard::strlcpy(rq.AffectedSOPClassUID, file.sopClass.toLatin1().constData(), sizeof(rq.AffectedSOPClassUID));
    OFStandard::strlcpy(rq.AffectedSOPInstanceUID, file.sopInstance.toLatin1().constData(), sizeof(rq.AffectedSOPInstanceUID));

    file.state = State::Sending;
    file.message = "Sending; waiting for peer confirmation";
    update(file);
    WireProgress wire{file, update, {}};
    wire.throttle.start();
    phase.begin(seconds);
    DcmDataset *sentSet = nullptr;
    auto condition = DIMSE_sendMessageUsingMemoryData(&association, contextId, &request, nullptr, &dataset,
                                                     progress, &wire, &sentSet);
    const std::unique_ptr<DcmDataset> sent(sentSet);
    T_ASC_PresentationContextID responseContext = 0;
    if (condition.good()) {
        if (sent)
            session.captureCommand(*sent, Direction::Outgoing, QDateTime::currentDateTimeUtc(), contextId, &dataset);
        phase.begin(seconds);
        DcmDataset *receivedSet = nullptr, *detail = nullptr;
        condition = DIMSE_receiveCommand(&association, DIMSE_NONBLOCKING, seconds, &responseContext,
                                         &response, &detail, &receivedSet);
        const std::unique_ptr<DcmDataset> received(receivedSet), statusDetail(detail);
        if (received)
            session.captureCommand(*received, Direction::Incoming, QDateTime::currentDateTimeUtc(),
                                   responseContext, nullptr, statusDetail.get());
    }

    const auto &rsp = response.msg.CStoreRSP;
    if (condition.good() && response.CommandField == DIMSE_C_STORE_RSP && responseContext == contextId &&
        rsp.MessageIDBeingRespondedTo == rq.MessageID && rsp.DataSetType == DIMSE_DATASET_NULL &&
        (!(rsp.opts & O_STORE_AFFECTEDSOPCLASSUID) || qstrcmp(rsp.AffectedSOPClassUID, rq.AffectedSOPClassUID) == 0) &&
        (!(rsp.opts & O_STORE_AFFECTEDSOPINSTANCEUID) ||
         qstrcmp(rsp.AffectedSOPInstanceUID, rq.AffectedSOPInstanceUID) == 0)) {
        file.status = rsp.DimseStatus;
        file.state = rsp.DimseStatus == STATUS_Success ? State::Stored :
            (rsp.DimseStatus & 0xf000) == 0xb000 ? State::Warning : State::Failed;
        file.message = "C-STORE response " + statusDescription(rsp.DimseStatus);
        return {};
    }

    phase.release = false;
    file.state = State::Unconfirmed;
    if (condition == DUL_PEERABORTEDASSOCIATION)
        session.association.closure = "Peer A-ABORT reported by DCMTK";
    if (phase.stop.stop_requested()) {
        file.message = "Cancelled; storage was not confirmed.";
        return OperationResult::cancelled();
    }
    const bool timedOut = phase.expired || condition == DIMSE_NODATAAVAILABLE || condition == DUL_READTIMEOUT;
    file.message = timedOut ? "DIMSE timeout; storage was not confirmed."
                            : "Connection or response failed; storage was not confirmed.";
    if (condition.bad())
        session.technicalError = conditionText(condition);
    return OperationResult::failed({timedOut ? ErrorCode::DimseTimeout : ErrorCode::ProtocolFailure, file.message});
}

}  // namespace

Operation inspectStorageFiles(QStringList paths, QString directory, QStringList existing, ReportStorageFile update)
{
    return [paths = std::move(paths), directory = std::move(directory), existing = std::move(existing),
            update = std::move(update)](std::stop_token stop, const ReportDiagnostic &) {
        QSet<QString> seen(existing.begin(), existing.end());
        const auto inspect = [&](const QString &path) {
            const auto canonical = QFileInfo(path).canonicalFilePath();
            const auto key = canonical.isEmpty() ? path : canonical;
            if (!seen.contains(key)) {
                seen.insert(key);
                DcmFileFormat format;
                update(readFile(key, format));
            }
        };
        for (const auto &path : paths) {
            if (stop.stop_requested() || seen.size() >= maximumStorageQueue)
                break;
            inspect(path);
        }
        if (!directory.isEmpty()) {
            QDirIterator files(directory, QDir::Files | QDir::NoSymLinks);
            while (!stop.stop_requested() && seen.size() < maximumStorageQueue && files.hasNext())
                inspect(files.next());
        }
        return stop.stop_requested() ? OperationResult::cancelled() : OperationResult{};
    };
}

Operation sendStorageFiles(profiles::Profile source, profiles::Profile destination, QStringList paths,
                           ClientTimeouts timeouts, bool detailedCapture, ReportStorageFile update,
                           QList<StorageFile> validatedQueue)
{
    return [source = std::move(source), destination = std::move(destination), paths = std::move(paths),
            timeouts, detailedCapture, update = std::move(update), validatedQueue = std::move(validatedQueue)](
               std::stop_token stop, const ReportDiagnostic &report) {
        DiagnosticSession session;
        session.detailedCapture = detailedCapture;
        QList<StorageFile> files;
        QHash<QString, int> contextIndices;
        QHash<QString, StorageFile> expected;
        for (const auto &file : validatedQueue)
            expected.insert(file.path, file);
        const auto finishFile = [&](const StorageFile &file) {
            update(file);
            const auto severity = file.state == State::Stored ? Severity::Info
                : file.state == State::Warning || file.state == State::Skipped ? Severity::Warning : Severity::Error;
            report({severity, file.message, file.sopInstance.isEmpty() ? QString() :
                "SOP Instance UID " + file.sopInstance + " · Transfer syntax " + file.transferSyntax});
        };
        for (const auto &path : paths) {
            StorageFile file;
            file.path = path;
            if (stop.stop_requested()) {
                file.state = State::Skipped;
                file.message = "Cancelled before validation.";
            } else {
                DcmFileFormat format;
                file = readFile(path, format);
                const auto previous = expected.constFind(path);
                if (previous != expected.constEnd() && !matchesValidated(file, *previous)) {
                    file.state = State::Invalid;
                    file.message = "File changed after queue validation. Remove and add it again to review its metadata.";
                }
                const auto key = contextKey(file);
                if (file.state == State::Ready && !contextIndices.contains(key)) {
                    if (contextIndices.size() == 128) {
                        file.state = State::Rejected;
                        file.message = "Association limit of 128 distinct SOP Class and syntax pairs reached.";
                    } else {
                        const int index = int(contextIndices.size());
                        contextIndices.insert(key, index);
                        session.association.contexts.append({index * 2 + 1, file.sopClass, {}, {file.transferSyntax}});
                    }
                }
            }
            if (file.state != State::Ready)
                finishFile(file);
            files.append(std::move(file));
        }

        auto result = stop.stop_requested() ? OperationResult::cancelled() :
            OperationResult::failed({ErrorCode::InvalidInput, "No validated files could be sent."});
        if (!stop.stop_requested() && !contextIndices.isEmpty()) {
            result = withClientAssociation(source, destination, timeouts, stop, report, session, "C-STORE",
                [&](T_ASC_Association &association, ClientPhase &phase) {
                    for (auto &file : files) {
                        if (stop.stop_requested())
                            return OperationResult::cancelled();
                        if (file.state != State::Ready)
                            continue;
                        const auto &context = session.association.contexts.at(contextIndices.value(contextKey(file)));
                        if (context.acceptedSyntax != file.transferSyntax) {
                            file.state = State::Rejected;
                            file.message = "Peer did not accept the original transfer syntax for this SOP Class.";
                            finishFile(file);
                            continue;
                        }
                        DcmFileFormat format;
                        const auto current = readFile(file.path, format);
                        if (stop.stop_requested())
                            return OperationResult::cancelled();
                        if (!matchesValidated(current, file)) {
                            file.state = State::Invalid;
                            file.message = "File changed or became unreadable after validation.";
                            finishFile(file);
                            continue;
                        }
                        auto transfer = sendOne(association, phase, timeouts.dimse, *format.getDataset(), context,
                                                file, session, update);
                        finishFile(file);
                        if (transfer.status != OperationResult::Status::Succeeded)
                            return transfer;
                    }
                    return OperationResult{};
                });
        }
        for (auto &file : files) {
            if (file.state == State::Ready) {
                file.state = State::Skipped;
                file.message = result.status == OperationResult::Status::Cancelled
                    ? "Cancelled before sending." : "Not sent; the association did not complete.";
                finishFile(file);
            }
        }
        const auto count = [&](State state) { return std::ranges::count(files, state, &StorageFile::state); };
        const auto stored = count(State::Stored), warnings = count(State::Warning), skipped = count(State::Skipped);
        const auto unconfirmed = count(State::Unconfirmed);
        const auto failures = files.size() - stored - warnings - skipped - unconfirmed;
        const auto summary = QString("%1 stored · %2 warnings · %3 failed · %4 unconfirmed · %5 not sent")
            .arg(stored).arg(warnings).arg(failures).arg(unconfirmed).arg(skipped);
        if (result.status == OperationResult::Status::Succeeded && failures > 0) {
            result.status = OperationResult::Status::Failed;
            result.error = OperationError{ErrorCode::StorageFailure, "One or more instances failed."};
        }
        session.counts = {{"Stored", stored}, {"Warnings", warnings}, {"Failed", failures},
                          {"Unconfirmed", unconfirmed}, {"Not sent", skipped}};
        report({failures || unconfirmed || skipped || warnings ? Severity::Warning : Severity::Info,
                "Storage summary: " + summary});
        result.details = summary;
        if (files.size() == 1)
            result.dicomStatus = files.first().status;
        result.protocol = std::move(session);
        return result;
    };
}

}  // namespace dingobench::dicom
