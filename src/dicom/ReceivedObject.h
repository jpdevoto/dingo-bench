// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>

#include <memory>
#include <stop_token>

class DcmDataset;
struct T_DIMSE_C_StoreRQ;

namespace dingobench::dicom {

inline constexpr size_t datasetWireLimit = 64 * 1024 * 1024;

struct StoreOutcome {
    QString name;
    QString message;
    qint64 bytes = 0;
    quint16 status = 0xa700;
};

bool stagingAllowed(const QString &output, const QString &stage);
StoreOutcome saveReceived(std::unique_ptr<DcmDataset> dataset, const T_DIMSE_C_StoreRQ &request,
                          const QString &output, const QString &stage, std::stop_token stop);

}  // namespace dingobench::dicom
