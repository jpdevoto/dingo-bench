// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/DiagnosticSession.h"

#include <stop_token>

namespace dingobench::dicom {

DatasetSnapshot snapshotFileMetadata(DcmItem &dataset, std::stop_token stop);

}  // namespace dingobench::dicom
