// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/OperationRunner.h"
#include "dingobench/infrastructure/ProfileStore.h"

namespace dingobench::dicom {

inline constexpr int maximumStorageQueue = 1000;

struct StorageFile {
    QString path, sopClass, sopInstance, transferSyntax, message;
    qint64 size = 0;
    QDateTime modified;
    enum State { Ready, Invalid, Sending, Stored, Warning, Rejected, Failed, Unconfirmed, Skipped } state = Invalid;
    std::optional<quint16> status;
    qint64 bytesSent = 0;
};

using ReportStorageFile = std::function<void(StorageFile)>;

Operation inspectStorageFiles(QStringList paths, QString directory, QStringList existing,
                              ReportStorageFile update);
Operation sendStorageFiles(profiles::Profile source, profiles::Profile destination, QStringList paths,
                           ClientTimeouts timeouts, bool detailedCapture, ReportStorageFile update,
                           QList<StorageFile> validatedQueue = {});

}  // namespace dingobench::dicom
