// SPDX-License-Identifier: GPL-3.0-only

#include "dingobench/infrastructure/AppPaths.h"

#include <QStandardPaths>

namespace dingobench::paths {

QString configDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString localDataDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QString cacheDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

}  // namespace dingobench::paths
