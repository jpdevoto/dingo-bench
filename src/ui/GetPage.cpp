// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"
#include "dingobench/dicom/GetRetrieve.h"
#include "dingobench/dicom/Verification.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMutex>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace dingobench {
namespace {

struct ProgressBuffer {
    QMutex mutex;
    std::optional<dicom::GetProgress> update;
};

QString countText(std::optional<quint16> count)
{
    return count ? QString::number(*count) : QString("—");
}

}  // namespace

void MainWindow::buildGetPanel(QVBoxLayout *body, QScrollArea *scroll, QTableWidget *results,
                               std::function<std::optional<RetrievalSelection>()> selected)
{
    using ui::Symbol;
    using Status = dicom::OperationResult::Status;
    auto *panel = ui::section("Retrieve on the same association", body);
    auto *selection = ui::note({});
    selection->setTextFormat(Qt::PlainText);
    panel->addWidget(selection);
    panel->addWidget(ui::note("C-GET receives C-STORE objects on its own association. No Storage SCP listener is required."));
    auto *form = new QFormLayout;
    panel->addLayout(form);
    auto *folderRow = new QHBoxLayout;
    auto *directory = new QLineEdit(selectedProfile(true).directory);
    directory->setObjectName("getDirectory");
    directory->setPlaceholderText("Choose an existing directory");
    folderRow->addWidget(directory, 1);
    auto *browse = ui::button("Browse", Symbol::Folder, "getBrowse");
    folderRow->addWidget(browse);
    form->addRow("Save received files", folderRow);
    connect(browse, &QPushButton::clicked, scroll, [=] {
        const auto chosen = QFileDialog::getExistingDirectory(scroll, "Choose C-GET output directory", directory->text(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
        if (!chosen.isEmpty())
            directory->setText(chosen);
    });

    auto *timeouts = new QHBoxLayout;
    panel->addLayout(timeouts);
    const auto timeout = [timeouts](const QString &label, const char *name, int initial) {
        auto *field = new QFormLayout;
        auto *spin = new QSpinBox;
        spin->setObjectName(QLatin1String(name));
        spin->setRange(1, dicom::maximumClientTimeout);
        spin->setValue(initial);
        spin->setSuffix(" s");
        field->addRow(label, spin);
        timeouts->addLayout(field);
        return spin;
    };
    auto *connection = timeout("Connection", "getConnectionTimeout", 5);
    auto *association = timeout("Association", "getAssociationTimeout", 5);
    auto *dimse = timeout("DIMSE", "getDimseTimeout", 30);

    auto *actions = new QHBoxLayout;
    panel->addLayout(actions);
    auto *send = ui::button("Send C-GET", Symbol::Send, "sendGet", true);
    auto *cancel = ui::button("Cancel", Symbol::Close, "cancelGet");
    actions->addWidget(send);
    actions->addWidget(cancel);
    actions->addStretch();
    auto *summary = ui::note("Ready to retrieve.");
    summary->setObjectName("getSummary");
    summary->setTextFormat(Qt::PlainText);
    panel->addWidget(summary);
    auto *counts = ui::note({});
    counts->setObjectName("getCounts");
    panel->addWidget(counts);
    auto *local = ui::note({});
    local->setObjectName("getReceipts");
    panel->addWidget(local);
    auto *protocol = new QHBoxLayout;
    panel->addLayout(protocol);
    addProtocolControls(protocol, "C-GET", "getProtocolSessions", "getProtocolDetails");

    const auto refresh = [=, this] {
        const bool idle = m_getId.isNull();
        const auto item = selected();
        if (item) {
            const auto keys = item->seriesUid.isEmpty() ? "Study " + item->studyUid
                : "Study " + item->studyUid + " · Series " + item->seriesUid;
            selection->setText(keys + "\nArchive from C-FIND · " + item->archive.name + " · " + item->archive.ae);
        } else {
            selection->setText("Select a STUDY or SERIES result. Run C-FIND again to use a different archive.");
        }
        directory->setEnabled(idle);
        browse->setEnabled(idle);
        for (auto *spin : {connection, association, dimse})
            spin->setEnabled(idle);
        send->setEnabled(idle && item.has_value());
        cancel->setEnabled(!idle);
    };
    const auto showProgress = [=](const dicom::GetProgress &progress) {
        counts->setText(QString("Archive sub-operations · Remaining %1 · Completed %2 · Failed %3 · Warning %4")
            .arg(countText(progress.remaining), countText(progress.completed), countText(progress.failed),
                 countText(progress.warnings)));
        local->setText(QString("Local C-STORE · %1 files saved · %2 failed").arg(progress.filesSaved).arg(progress.storesFailed));
        if (progress.status && cancel->isEnabled())
            ui::status(summary, dicom::getStatusText(*progress.status) + " · " +
                       dicom::formatDicomStatus(*progress.status), "info");
    };

    auto progress = std::make_shared<ProgressBuffer>();
    auto *timer = new QTimer(scroll);
    timer->setInterval(50);
    const auto showLatest = [progress, showProgress] {
        std::optional<dicom::GetProgress> latest;
        {
            QMutexLocker guard(&progress->mutex);
            latest.swap(progress->update);
        }
        if (latest)
            showProgress(*latest);
    };
    connect(timer, &QTimer::timeout, scroll, showLatest);
    connect(results, &QTableWidget::itemSelectionChanged, scroll, refresh);
    connect(send, &QPushButton::clicked, scroll, [=, this] {
        const auto item = selected();
        if (!m_getId.isNull() || !item)
            return;
        const dicom::GetRequest request{item->studyUid, item->seriesUid, directory->text().trimmed()};
        const auto report = [progress](dicom::GetProgress update) {
            QMutexLocker guard(&progress->mutex);
            progress->update = update;
        };
        m_getId = m_getRunner.start("C-GET", dicom::getOperation(selectedProfile(true), item->archive, request,
            {connection->value(), association->value(), dimse->value()},
            QSettings().value("protocolInspector/detailedCapture", false).toBool(), report), false, "Query / Retrieve", dicom::DiagnosticSession::Identities{selectedProfile(true), item->archive});
        showProgress({});
        timer->start();
        ui::status(summary, "C-GET running…", "info");
        refresh();
    });
    connect(cancel, &QPushButton::clicked, scroll, [=, this] {
        if (m_getRunner.cancel(m_getId)) {
            cancel->setEnabled(false);
            ui::status(summary, "Cancelling C-GET…", "warning");
        }
    });
    connect(&m_getRunner, &dicom::OperationRunner::finished, scroll,
        [=, this](const QUuid &id, const dicom::OperationResult &result) {
            if (id != m_getId)
                return;
            m_getId = {};
            timer->stop();
            showLatest();
            if (result.protocol)
                retainProtocol(*result.protocol);
            const bool cancelled = result.status == Status::Cancelled;
            const bool warning = result.status == Status::Warning;
            QString text = result.error ? result.error->message
                : cancelled ? QString("C-GET stopped")
                : warning && result.dicomStatus == 0 ? QString("Archive completion reported with local or peer failures")
                : result.dicomStatus ? dicom::getStatusText(*result.dicomStatus) : QString("C-GET finished");
            if (cancelled && !result.dicomStatus)
                text += " · Archive acknowledgement unavailable";
            text += QString(" · %1 ms").arg(result.elapsedMs);
            if (result.dicomStatus)
                text += " · " + dicom::formatDicomStatus(*result.dicomStatus);
            ui::status(summary, text, result.error ? "error" : cancelled || warning ? "warning" : "success");
            refresh();
            closeWhenOperationsFinish();
        });
    showProgress({});
    watch(refresh);
    refresh();
}

}  // namespace dingobench
