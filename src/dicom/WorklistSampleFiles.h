// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/OperationRunner.h"

class QDir;

namespace dingobench::dicom::detail {

// Move a validated batch from a private staging directory on the same volume.
OperationResult publishWorklistSampleFiles(const QDir &staged, const QDir &destination,
                                           const QStringList &names, const QString &batch,
                                           std::stop_token stop);

}  // namespace dingobench::dicom::detail
