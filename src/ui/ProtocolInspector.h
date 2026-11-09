// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDialog>
#include "dingobench/dicom/DiagnosticSession.h"

namespace dingobench::ui {

class ProtocolInspector final : public QDialog {
public:
    ProtocolInspector(const dicom::DiagnosticSession &session, QWidget *parent);
};

void openDiagnosticReport(QWidget *parent, QString path = {});
void showDatasetInspector(const dicom::DatasetSnapshot &dataset, const QString &heading, QWidget *parent);

}  // namespace dingobench::ui
