// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/QueryFind.h"
#include "FindExchange.h"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcuid.h>

#include <QRegularExpression>

namespace dingobench::dicom {
namespace {

bool validUid(const QString &uid)
{
    static const QRegularExpression pattern(R"(\A(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*))+\z)");
    return uid.size() <= 64 && pattern.match(uid).hasMatch();
}

OFCondition buildIdentifier(const QueryRequest &query, DcmDataset &identifier)
{
    OFCondition condition = EC_Normal;
    bool unicode = false;
    const auto add = [&](const DcmTagKey &tag, const QString &value) {
        for (const auto ch : value)
            unicode |= ch.unicode() > 127;
        if (condition.good())
            condition = identifier.putAndInsertString(tag, value.toUtf8().constData());
    };
    add(DCM_QueryRetrieveLevel, queryLevelName(query.level));
    add(DCM_StudyInstanceUID, query.studyUid);
    if (query.level == QueryLevel::Study) {
        add(DCM_PatientID, query.patientId);
        add(DCM_PatientName, query.patientName);
        add(DCM_AccessionNumber, query.accession);
        add(DCM_StudyDate, query.studyDate);
        add(DCM_StudyDescription, {});
        add(DCM_ModalitiesInStudy, query.modality);
    } else {
        add(DCM_SeriesInstanceUID, query.seriesUid);
        if (query.level == QueryLevel::Series) {
            add(DCM_Modality, query.modality);
            add(DCM_SeriesNumber, {});
            add(DCM_SeriesDescription, {});
        } else {
            add(DCM_SOPInstanceUID, query.instanceUid);
            add(DCM_InstanceNumber, {});
            add(DCM_SOPClassUID, {});
        }
    }
    if (unicode)
        add(DCM_SpecificCharacterSet, "ISO_IR 192");
    return condition;
}

}  // namespace

QString queryLevelName(QueryLevel level)
{
    switch (level) {
    case QueryLevel::Study: return "STUDY";
    case QueryLevel::Series: return "SERIES";
    case QueryLevel::Image: return "IMAGE";
    }
    return {};
}

QString validateQuery(const QueryRequest &query)
{
    if (queryLevelName(query.level).isEmpty())
        return "Choose STUDY, SERIES, or IMAGE.";
    if (query.level != QueryLevel::Study) {
        if (query.studyUid.isEmpty())
            return "Study Instance UID is required at SERIES and IMAGE levels.";
        if (!query.patientId.isEmpty() || !query.patientName.isEmpty() ||
            !query.accession.isEmpty() || !query.studyDate.isEmpty())
            return "Patient, accession, and study date filters apply only at STUDY level.";
    }
    if (query.level == QueryLevel::Image) {
        if (query.seriesUid.isEmpty())
            return "Series Instance UID is required at IMAGE level.";
        if (!query.modality.isEmpty())
            return "Modality cannot be used at IMAGE level.";
    } else if (!query.instanceUid.isEmpty()) {
        return "SOP Instance UID applies only at IMAGE level.";
    }
    if (query.level == QueryLevel::Study && !query.seriesUid.isEmpty())
        return "Series Instance UID cannot be used at STUDY level.";
    if ((!query.studyUid.isEmpty() && !validUid(query.studyUid)) ||
        (!query.seriesUid.isEmpty() && !validUid(query.seriesUid)) ||
        (!query.instanceUid.isEmpty() && !validUid(query.instanceUid)))
        return "Each UID must be a single exact DICOM UID of at most 64 characters.";
    if (!query.studyDate.isEmpty() && !validFindDate(query.studyDate))
        return "Enter a valid study date as YYYYMMDD or an ascending date range, such as 20260101-20261231.";
    if (!validFindText(query.patientId, 64) || !validFindText(query.accession, 16))
        return "Enter a single Patient ID (up to 64 characters) and accession number (up to 16 characters).";
    if (const auto error = findPersonNameError(query.patientName); !error.isEmpty())
        return error;
    if (!query.modality.isEmpty() && !validFindModality(query.modality))
        return "Enter a single modality code in uppercase, such as CT or MR.";
    return {};
}

Operation findOperation(profiles::Profile source, profiles::Profile destination, QueryRequest query,
                        ClientTimeouts timeouts, bool detailedCapture, std::function<void(QueryMatch)> onMatch)
{
    return [source = std::move(source), destination = std::move(destination), query = std::move(query),
            timeouts, detailedCapture, onMatch = std::move(onMatch)](std::stop_token stop,
                                                                    const ReportDiagnostic &report) {
        if (const auto error = validateQuery(query); !error.isEmpty())
            return OperationResult::failed({ErrorCode::InvalidInput, error});
        DcmDataset identifier;
        if (buildIdentifier(query, identifier).bad())
            return OperationResult::failed({ErrorCode::Internal, "Could not construct the C-FIND identifier."});
        return executeFind(source, destination, UID_FINDStudyRootQueryRetrieveInformationModel,
                           "Study Root · " + queryLevelName(query.level), identifier,
                           timeouts, detailedCapture, stop, report, onMatch);
    };
}

}  // namespace dingobench::dicom
