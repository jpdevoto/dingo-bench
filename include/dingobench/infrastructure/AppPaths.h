// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

namespace dingobench::paths {

// Resolve these after setting the Qt application and organization names.
// These functions query locations without creating directories.
QString configDirectory();
QString localDataDirectory();
QString cacheDirectory();

}  // namespace dingobench::paths
