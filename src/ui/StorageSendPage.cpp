// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"
#include "dingobench/dicom/StorageSend.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace dingobench {
namespace {

using dicom::StorageFile;
using ui::Symbol;

QString transferProgress(const StorageFile &file)
{
    switch (file.state) {
    case StorageFile::Ready: return "Pending";
    case StorageFile::Sending: return file.bytesSent ? QString("%1 B sent").arg(file.bytesSent) : QString("Sending…");
    case StorageFile::Stored:
    case StorageFile::Warning: return "Confirmed";
    case StorageFile::Unconfirmed: return "Unconfirmed";
    default: return "—";
    }
}

struct Queue {
    QList<StorageFile> files;
    QHash<QString, int> rows;
    bool sending = false;
    int total = 0;
    int completed = 0;
};

}  // namespace

QWidget *MainWindow::storageSend()
{
    QVBoxLayout *body;
    auto *widget = page("Storage Send", "Validate local DICOM files and send them sequentially using C-STORE.", &body);
    body->addWidget(connectionRow());
    auto *queue = ui::section("Transfer queue", body);
    auto *controls = new QHBoxLayout;
    queue->addLayout(controls);
    auto *add = ui::button("Add files", Symbol::Add, "sendAddFiles");
    auto *directory = ui::button("Add directory", Symbol::Folder, "sendAddDirectory");
    auto *remove = ui::button("Remove", Symbol::Delete, "sendRemove");
    auto *clear = ui::button("Clear", Symbol::Clear, "sendClear");
    for (auto *control : {add, directory, remove, clear})
        controls->addWidget(control);
    controls->addStretch();
    auto *files = ui::table({"File", "SOP Class UID", "SOP Instance UID", "Transfer Syntax UID", "Size (B)", "Progress", "Result"});
    files->setObjectName("sendFiles");
    files->setMinimumHeight(175);
    files->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    files->horizontalHeader()->setStretchLastSection(true);
    const int widths[]{160, 152, 164, 144, 84, 95};
    for (int column = 0; column < 6; ++column)
        files->setColumnWidth(column, widths[column]);
    queue->addWidget(files);

    auto *transfer = ui::section("Transfer progress", body);
    auto *timeouts = new QHBoxLayout;
    transfer->addLayout(timeouts);
    const auto timeout = [&](const QString &label, const char *name) {
        auto *form = new QFormLayout;
        auto *spin = new QSpinBox;
        spin->setObjectName(QLatin1String(name));
        spin->setRange(1, dicom::maximumClientTimeout);
        spin->setValue(5);
        spin->setSuffix(" s");
        form->addRow(label, spin);
        timeouts->addLayout(form);
        return spin;
    };
    auto *connection = timeout("Connection", "storageConnectionTimeout");
    auto *association = timeout("Association", "storageAssociationTimeout");
    auto *dimse = timeout("DIMSE", "storageDimseTimeout");
    auto *bar = new QProgressBar;
    bar->setObjectName("sendProgress");
    transfer->addWidget(bar);
    auto *summary = ui::note("Add DICOM Part 10 files to validate them.");
    summary->setObjectName("storageSummary");
    summary->setTextFormat(Qt::PlainText);
    transfer->addWidget(summary);
    auto *actions = new QHBoxLayout;
    transfer->addLayout(actions);
    auto *send = ui::button("Send files", Symbol::Send, "sendStorage", true);
    auto *cancel = ui::button("Cancel", Symbol::Close, "cancelStorage");
    actions->addWidget(send);
    actions->addWidget(cancel);
    actions->addStretch();
    auto *protocol = new QHBoxLayout;
    transfer->addLayout(protocol);
    addProtocolControls(protocol, "C-STORE", "storageProtocolSessions", "storageProtocolDetails");

    auto state = std::make_shared<Queue>();
    const auto refreshControls = [=, this] {
        const bool idle = m_storageId.isNull();
        const bool ready = std::ranges::any_of(state->files, [](const StorageFile &file) {
            return file.state == StorageFile::Ready;
        });
        send->setEnabled(idle && ready && selectedProfile(false).id);
        cancel->setEnabled(!idle);
        clear->setEnabled(idle && !state->files.isEmpty());
        remove->setEnabled(idle && !files->selectedItems().isEmpty());
        add->setEnabled(idle && state->files.size() < dicom::maximumStorageQueue);
        directory->setEnabled(add->isEnabled());
    };
    const auto updateTable = [=](StorageFile file) {
        int row = state->rows.value(file.path, -1);
        if (row < 0) {
            row = files->rowCount();
            files->insertRow(row);
            state->rows.insert(file.path, row);
            state->files.append(file);
        } else {
            const auto previous = state->files.at(row).state;
            if (state->sending && (previous == StorageFile::Ready || previous == StorageFile::Sending) &&
                file.state != StorageFile::Ready && file.state != StorageFile::Sending)
                bar->setValue(++state->completed);
            state->files[row] = file;
        }
        const QStringList values{QFileInfo(file.path).fileName(), file.sopClass, file.sopInstance, file.transferSyntax,
                                 QString::number(file.size), transferProgress(file), file.message};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = files->item(row, column);
            if (!item) {
                item = new QTableWidgetItem;
                files->setItem(row, column, item);
            }
            item->setText(values.at(column));
            const auto tooltip = column == 0 ? file.path : values.at(column);
            item->setToolTip("<qt>" + tooltip.toHtmlEscaped() + "</qt>");
        }
    };
    const auto reportFile = [this, updateTable](StorageFile file) {
        QMetaObject::invokeMethod(this, [updateTable, file = std::move(file)] { updateTable(file); },
                                  Qt::QueuedConnection);
    };
    const auto inspect = [=, this](QStringList paths, QString folder) {
        if ((paths.isEmpty() && folder.isEmpty()) || !m_storageId.isNull())
            return;
        QStringList existing;
        for (const auto &file : state->files)
            existing.append(file.path);
        m_storageId = m_storageRunner.start("Validate storage files",
            dicom::inspectStorageFiles(std::move(paths), std::move(folder), std::move(existing), reportFile),
            false, "Storage Send");
        bar->setRange(0, 0);
        ui::status(summary, "Validating files…", "info");
        refreshControls();
    };
    connect(add, &QPushButton::clicked, this, [this, inspect] {
        inspect(QFileDialog::getOpenFileNames(this, "Add DICOM files", {},
            "DICOM files (*.dcm *.dicom);;All files (*)", nullptr, QFileDialog::DontUseNativeDialog), {});
    });
    connect(directory, &QPushButton::clicked, this, [this, inspect] {
        inspect({}, QFileDialog::getExistingDirectory(this, "Add files from directory", {},
            QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog));
    });
    const auto queueSummary = [=] {
        const auto ready = std::ranges::count(state->files, StorageFile::Ready, &StorageFile::state);
        const auto invalid = std::ranges::count(state->files, StorageFile::Invalid, &StorageFile::state);
        return QString("%1 files · %2 ready · %3 invalid").arg(state->files.size()).arg(ready).arg(invalid);
    };
    const auto resetEmptyQueue = [=] {
        bar->setRange(0, 1);
        bar->setValue(0);
        ui::status(summary, "Add DICOM Part 10 files to validate them.", "info");
    };
    connect(remove, &QPushButton::clicked, this, [=] {
        auto selection = files->selectionModel()->selectedRows();
        std::ranges::sort(selection, std::greater{}, &QModelIndex::row);
        for (const auto &index : selection) {
            state->files.removeAt(index.row());
            files->removeRow(index.row());
        }
        state->rows.clear();
        for (int index = 0; index < state->files.size(); ++index)
            state->rows.insert(state->files.at(index).path, index);
        if (state->files.isEmpty())
            resetEmptyQueue();
        else
            ui::status(summary, queueSummary(), "info");
        refreshControls();
    });
    connect(clear, &QPushButton::clicked, this, [=] {
        state->files.clear();
        state->rows.clear();
        files->setRowCount(0);
        resetEmptyQueue();
        refreshControls();
    });
    connect(files, &QTableWidget::itemSelectionChanged, this, refreshControls);
    files->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(files, &QTableWidget::customContextMenuRequested, this, [files, remove, clear](QPoint point) {
        QMenu menu;
        auto *deleteRows = menu.addAction(ui::icon(Symbol::Delete), "Remove selected files", remove, &QPushButton::click);
        auto *clearRows = menu.addAction(ui::icon(Symbol::Clear), "Clear queue", clear, &QPushButton::click);
        deleteRows->setEnabled(remove->isEnabled());
        clearRows->setEnabled(clear->isEnabled());
        menu.exec(files->viewport()->mapToGlobal(point));
    });
    connect(send, &QPushButton::clicked, this, [=, this] {
        QStringList pending;
        QList<StorageFile> validated;
        for (const auto &file : state->files) {
            if (file.state == StorageFile::Ready) {
                pending.append(file.path);
                validated.append(file);
            }
        }
        if (pending.isEmpty() || !m_storageId.isNull() || !selectedProfile(false).id)
            return;
        state->sending = true;
        state->total = int(pending.size());
        state->completed = 0;
        bar->setRange(0, state->total);
        bar->setValue(0);
        m_storageId = m_storageRunner.start("C-STORE", dicom::sendStorageFiles(selectedProfile(true),
            selectedProfile(false), std::move(pending), {connection->value(), association->value(), dimse->value()},
            QSettings().value("protocolInspector/detailedCapture", false).toBool(), reportFile,
            std::move(validated)), false, "Storage Send",
            dicom::DiagnosticSession::Identities{selectedProfile(true), selectedProfile(false)});
        ui::status(summary, "Sending files…", "info");
        refreshControls();
    });
    connect(cancel, &QPushButton::clicked, this, [this, cancel, summary] {
        if (m_storageRunner.cancel(m_storageId)) {
            ui::status(summary, "Cancelling…", "warning");
            cancel->setEnabled(false);
        }
    });
    connect(&m_storageRunner, &dicom::OperationRunner::finished, widget,
        [=, this](const QUuid &id, const dicom::OperationResult &result) {
            if (id != m_storageId)
                return;
            m_storageId = {};
            if (result.protocol)
                retainProtocol(*result.protocol);
            const auto cancelled = result.status == dicom::OperationResult::Status::Cancelled;
            if (state->sending) {
                auto text = result.details.isEmpty() ? QString("No file was confirmed.") : result.details;
                if (result.error)
                    text += " · " + result.error->message;
                else if (cancelled)
                    text = "Cancelled · " + text;
                ui::status(summary, text, result.error ? "error" : cancelled ? "warning" : "info");
            } else {
                bar->setRange(0, 1);
                bar->setValue(0);
                ui::status(summary, (cancelled ? "Validation cancelled · " : "") + queueSummary(),
                           cancelled ? "warning" : "info");
            }
            state->sending = false;
            refreshControls();
            closeWhenOperationsFinish();
        });
    bar->setRange(0, 1);
    bar->setValue(0);
    watch(refreshControls);
    refreshControls();
    return widget;
}

}  // namespace dingobench
