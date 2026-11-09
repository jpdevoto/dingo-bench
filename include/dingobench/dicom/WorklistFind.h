// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/FindResult.h"
#include "dingobench/dicom/OperationRunner.h"
#include "dingobench/infrastructure/ProfileStore.h"

namespace dingobench::dicom {

struct WorklistRequest {
    QString scheduledDate, modality, stationAe, patientId, patientName;
};

QString validateWorklist(const WorklistRequest &query);

// Delivers decoded matches on the operation worker.
Operation worklistOperation(profiles::Profile source, profiles::Profile destination, WorklistRequest query,
                            ClientTimeouts timeouts, bool detailedCapture,
                            std::function<void(QueryMatch)> onMatch);

}  // namespace dingobench::dicom
