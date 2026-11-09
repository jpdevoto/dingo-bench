// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/OperationRunner.h"
#include "dingobench/infrastructure/ProfileStore.h"

namespace dingobench::dicom {

inline constexpr int maximumVerificationTimeout = maximumClientTimeout;
using VerificationTimeouts = ClientTimeouts;

Operation verificationOperation(profiles::Profile source, profiles::Profile destination,
                                VerificationTimeouts timeouts = {}, bool detailedCapture = false);
QString formatDicomStatus(quint16 status);

}  // namespace dingobench::dicom
