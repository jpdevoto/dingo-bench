// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/OperationRunner.h"
#include "dingobench/infrastructure/ProfileStore.h"

namespace dingobench::dicom {

struct MoveRequest {
    QString studyUid, seriesUid, destinationAe;
    quint16 messageId = 0;
};

struct MoveProgress {
    quint16 status;
    std::optional<quint16> remaining, completed, failed, warnings;
};

quint16 nextMoveMessageId();
QString validateMove(const MoveRequest &request);
QString moveStatusText(quint16 status);

// Progress is delivered on the operation worker and represents the archive's
// reported C-STORE sub-operations. Local receipts are tracked separately.
Operation moveOperation(profiles::Profile source, profiles::Profile archive, MoveRequest request,
                        ClientTimeouts timeouts, bool detailedCapture,
                        std::function<void(MoveProgress)> onProgress);

}  // namespace dingobench::dicom
