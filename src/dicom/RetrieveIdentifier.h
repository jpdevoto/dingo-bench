// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcvrui.h>

#include <QString>

namespace dingobench::dicom {

inline bool validRetrieveKeys(const QString &study, const QString &series)
{
    const auto valid = [](const QString &uid) {
        const auto bytes = uid.toUtf8();
        return !uid.isEmpty() && uid == uid.trimmed() &&
            DcmUniqueIdentifier::checkStringValue(OFString(bytes.constData(), bytes.size()), "1").good();
    };
    return valid(study) && (series.isEmpty() || valid(series));
}

inline OFCondition buildRetrieveIdentifier(DcmDataset &identifier, const QString &study, const QString &series)
{
    auto condition = identifier.putAndInsertString(DCM_QueryRetrieveLevel, series.isEmpty() ? "STUDY" : "SERIES");
    if (condition.good())
        condition = identifier.putAndInsertString(DCM_StudyInstanceUID, study.toLatin1().constData());
    if (condition.good() && !series.isEmpty())
        condition = identifier.putAndInsertString(DCM_SeriesInstanceUID, series.toLatin1().constData());
    return condition;
}

}  // namespace dingobench::dicom
