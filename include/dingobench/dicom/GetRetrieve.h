// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/OperationRunner.h"
#include "dingobench/infrastructure/ProfileStore.h"

namespace dingobench::dicom {

struct GetRequest {
    QString studyUid, seriesUid, directory;
};

struct GetProgress {
    std::optional<quint16> status, remaining, completed, failed, warnings;
    quint64 filesSaved = 0, storesFailed = 0;
};

QString getStatusText(quint16 status);

// Reports archive counters and confirmed local writes from the operation worker.
Operation getOperation(profiles::Profile source, profiles::Profile archive, GetRequest request,
                       ClientTimeouts timeouts, bool detailedCapture,
                       std::function<void(GetProgress)> onProgress);

}  // namespace dingobench::dicom
