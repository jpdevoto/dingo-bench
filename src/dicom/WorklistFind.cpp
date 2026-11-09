// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/WorklistFind.h"
#include "FindExchange.h"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcuid.h>

namespace dingobench::dicom {
namespace {

OFCondition buildIdentifier(const WorklistRequest &query, DcmDataset &identifier)
{
    OFCondition condition = EC_Normal;
    const auto add = [&](DcmItem &item, const DcmTagKey &tag, const QString &value = {}) {
        if (condition.good())
            condition = item.putAndInsertString(tag, value.toUtf8().constData());
    };
    add(identifier, DCM_PatientID, query.patientId);
    add(identifier, DCM_PatientName, query.patientName);
    add(identifier, DCM_AccessionNumber);
    add(identifier, DCM_RequestedProcedureID);
    add(identifier, DCM_RequestedProcedureDescription);
    add(identifier, DCM_StudyInstanceUID);

    const auto patientKeys = query.patientId + query.patientName;
    for (const auto ch : patientKeys) {
        if (ch.unicode() > 127) {
            add(identifier, DCM_SpecificCharacterSet, "ISO_IR 192");
            break;
        }
    }

    DcmItem *step = nullptr;
    if (condition.good())
        condition = identifier.findOrCreateSequenceItem(DCM_ScheduledProcedureStepSequence, step, 0);
    if (condition.bad())
        return condition;
    add(*step, DCM_ScheduledStationAETitle, query.stationAe);
    add(*step, DCM_ScheduledProcedureStepStartDate, query.scheduledDate);
    add(*step, DCM_Modality, query.modality);
    add(*step, DCM_ScheduledProcedureStepStartTime);
    add(*step, DCM_ScheduledProcedureStepID);
    add(*step, DCM_ScheduledProcedureStepDescription);
    add(*step, DCM_ScheduledStationName);
    add(*step, DCM_ScheduledProcedureStepLocation);

    DcmItem *code = nullptr;
    if (condition.good())
        condition = step->findOrCreateSequenceItem(DCM_ScheduledProtocolCodeSequence, code, 0);
    if (condition.good()) {
        add(*code, DCM_CodeValue);
        add(*code, DCM_CodingSchemeDesignator);
        add(*code, DCM_CodeMeaning);
    }
    return condition;
}

}  // namespace

QString validateWorklist(const WorklistRequest &query)
{
    if (!query.scheduledDate.isEmpty() && !validFindDate(query.scheduledDate))
        return "Enter a valid scheduled date as YYYYMMDD or an ascending date range, such as 20260101-20260131.";
    if (!query.modality.isEmpty() && !validFindModality(query.modality))
        return "Enter a single modality code in uppercase, such as CT or MR.";
    if (!query.stationAe.isEmpty()) {
        if (query.stationAe.size() > 16 || query.stationAe.trimmed().isEmpty())
            return "Enter a Scheduled Station AE Title of 1–16 printable ASCII characters.";
        for (const auto ch : query.stationAe)
            if (ch.unicode() < 32 || ch.unicode() > 126 || ch == u'\\')
                return "Enter a Scheduled Station AE Title of 1–16 printable ASCII characters without backslashes.";
    }
    if (!validFindText(query.patientId, 64))
        return "Enter a single Patient ID of up to 64 characters without backslashes or control characters.";
    return findPersonNameError(query.patientName);
}

Operation worklistOperation(profiles::Profile source, profiles::Profile destination, WorklistRequest query,
                            ClientTimeouts timeouts, bool detailedCapture,
                            std::function<void(QueryMatch)> onMatch)
{
    return [source = std::move(source), destination = std::move(destination), query = std::move(query),
            timeouts, detailedCapture, onMatch = std::move(onMatch)](std::stop_token stop,
                                                                    const ReportDiagnostic &report) {
        if (const auto error = validateWorklist(query); !error.isEmpty())
            return OperationResult::failed({ErrorCode::InvalidInput, error});
        DcmDataset identifier;
        if (buildIdentifier(query, identifier).bad())
            return OperationResult::failed({ErrorCode::Internal, "Could not construct the Modality Worklist identifier."});
        return executeFind(source, destination, UID_FINDModalityWorklistInformationModel, "Modality Worklist",
                           identifier, timeouts, detailedCapture, stop, report, onMatch);
    };
}

}  // namespace dingobench::dicom
