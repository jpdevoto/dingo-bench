// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/FindResult.h"
#include "dingobench/dicom/OperationRunner.h"
#include "dingobench/infrastructure/ProfileStore.h"

class DcmDataset;

namespace dingobench::dicom {

bool validFindDate(const QString &value);
bool validFindText(const QString &value, int max);
QString findPersonNameError(const QString &value);
bool validFindModality(const QString &value);

// Runs a C-FIND exchange using the service's own SOP Class and identifier.
OperationResult executeFind(const profiles::Profile &source, const profiles::Profile &destination,
    const char *sopClass, const QString &model, DcmDataset &identifier, ClientTimeouts timeouts,
    bool detailedCapture, std::stop_token stop, const ReportDiagnostic &report,
    const std::function<void(QueryMatch)> &onMatch);

}  // namespace dingobench::dicom
