// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/FindResult.h"
#include "dingobench/dicom/OperationRunner.h"
#include "dingobench/infrastructure/ProfileStore.h"

namespace dingobench::dicom {

enum class QueryLevel { Study, Series, Image };

struct QueryRequest {
    QueryLevel level = QueryLevel::Study;
    QString patientId, patientName, accession, studyDate, studyUid, seriesUid, instanceUid, modality;
};

QString queryLevelName(QueryLevel level);
QString validateQuery(const QueryRequest &query);

// Delivers decoded matches on the operation worker. The caller can queue them to the UI.
Operation findOperation(profiles::Profile source, profiles::Profile destination, QueryRequest query,
                        ClientTimeouts timeouts, bool detailedCapture,
                        std::function<void(QueryMatch)> onMatch);

}  // namespace dingobench::dicom
