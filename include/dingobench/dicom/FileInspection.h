// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/OperationRunner.h"

namespace dingobench::dicom {

struct FileInspection {
    DatasetSnapshot meta, dataset;
    QString syntaxUid, syntaxName, sopClass, sopInstance, characterSet, pixelData;
    QStringList issues;
    qint64 size = 0, physicalBytesRead = 0;
    bool datasetRead = false;
};

// Delivers a read-only snapshot on the operation worker. Partial results can
// accompany a parse error, and never contain toolkit objects or pixel values.
Operation inspectDicomFile(QString path, std::function<void(FileInspection)> deliver);

}  // namespace dingobench::dicom
