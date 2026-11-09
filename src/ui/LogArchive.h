// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/infrastructure/Logger.h"

namespace dingobench::archive {

enum class Format { Text, JsonLines };
QString describe(const logging::Event &event, bool includeDetails);
bool save(const QString &path, const QList<logging::Event> &events, Format format,
          bool includeDetails, QString &error);
bool load(const QString &path, QList<logging::Event> &events, QString &error);

}  // namespace dingobench::archive
