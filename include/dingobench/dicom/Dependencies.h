// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>
#include <QStringList>

namespace dingobench::dicom {

struct DependencyInfo {
    QString version;
    QStringList modules;
    bool dictionaryLoaded;
    bool associationParametersAvailable;
};

// Probes the installed toolkit without opening a socket.
DependencyInfo dependencies();

}  // namespace dingobench::dicom
