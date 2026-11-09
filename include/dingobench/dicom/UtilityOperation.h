// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QStringList>

#include <chrono>

#include "dingobench/dicom/OperationRunner.h"

namespace dingobench::dicom {

// Pass a trusted absolute executable path and discrete arguments. The child
// receives no input. Its output and command line are excluded from diagnostics.
Operation utilityOperation(QString executable, QStringList arguments,
                           std::chrono::milliseconds timeout);

}  // namespace dingobench::dicom
