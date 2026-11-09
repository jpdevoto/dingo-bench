// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "dingobench/dicom/OperationRunner.h"
#include "dingobench/infrastructure/ProfileStore.h"

#include "BoundedConnection.h"

struct T_ASC_Association;
class OFCondition;

namespace dingobench::dicom {

using AssociationWork = std::function<OperationResult(T_ASC_Association &, ClientPhase &)>;

QString conditionText(const OFCondition &condition);

// Owns the association for the duration of work. Contexts carry the proposed UIDs
// on entry and the peer's actual decisions on return.
OperationResult withClientAssociation(const profiles::Profile &source, const profiles::Profile &destination,
    ClientTimeouts timeouts, std::stop_token stop, const ReportDiagnostic &report,
    DiagnosticSession &session, const QString &command, const AssociationWork &work);

}  // namespace dingobench::dicom
