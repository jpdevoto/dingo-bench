// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/DiagnosticSession.h"

namespace dingobench::dicom {

inline constexpr int maximumQueryMatches = 250;

struct QueryMatch {
    DatasetSnapshot dataset;
    quint16 status;
};

}  // namespace dingobench::dicom
