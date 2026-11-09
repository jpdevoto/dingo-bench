// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"
#include "ProtocolInspector.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace dingobench {
namespace {

using State = dicom::StorageReceiver::State;
using ui::Symbol;

struct ReceiveView {
    std::optional<profiles::Profile> runningProfile;
    QString defaultDirectory, inspecting;
    int profileId = 0;
    quint64 files = 0, bytes = 0;
    std::optional<dicom::DatasetSnapshot> snapshot;
};

}  // namespace

QWidget *MainWindow::storageReceive()
{
    QVBoxLayout *body;
    auto *widget = page("Storage Receive", "Run a temporary DICOM Storage SCP and inspect received objects.", &body);
    auto *listener = ui::section("Listener configuration", body);
    auto *form = new QFormLayout;
    listener->addLayout(form);
    auto *selector = profileSelector(true, "receiveSourceProfile");
    form->addRow("Source profile", selector);
    const auto value = [form](const QString &name) {
        auto *label = new QLabel;
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(name, label);
        return label;
    };
    auto *ae = value("Local AE Title");
    auto *port = value("Listening port");
    auto *bind = value("Bind address");
    auto *directory = new QLineEdit;
    directory->setObjectName("receiveDirectory");
    directory->setPlaceholderText("Choose an existing local directory");
    auto *directoryRow = new QHBoxLayout;
    auto *directoryWidget = new QWidget;
    directoryWidget->setLayout(directoryRow);
    directoryRow->setContentsMargins(0, 0, 0, 0);
    directoryRow->addWidget(directory, 1);
    auto *browse = ui::button("Browse", Symbol::Folder, "receiveBrowse");
    directoryRow->addWidget(browse);
    form->addRow("Received files", directoryWidget);
    auto *strict = new QCheckBox("Require the selected Called AE Title");
    strict->setObjectName("receiveStrictAe");
    strict->setChecked(true);
    listener->addWidget(strict);

    auto *buttons = new QHBoxLayout;
    listener->addLayout(buttons);
    auto *start = ui::button("Start listener", Symbol::Run, "receiveStart", true);
    auto *stop = ui::button("Stop listener", Symbol::Stop, "receiveStop");
    buttons->addWidget(start);
    buttons->addWidget(stop);
    buttons->addStretch();
    auto *status = ui::note({});
    status->setObjectName("receiveStatus");
    status->setTextFormat(Qt::PlainText);
    listener->addWidget(status);

    auto *objects = ui::section("Incoming associations and objects", body);
    auto *totals = ui::note("0 files saved · 0 bytes");
    totals->setObjectName("receiveTotals");
    objects->addWidget(totals);
    auto *table = ui::table({"Time", "Calling AE", "Event", "SOP Class UID", "SOP Instance UID", "Bytes", "Status"});
    table->setObjectName("receiveEvents");
    table->setMinimumHeight(180);
    table->setColumnWidth(0, 93);
    table->setColumnWidth(1, 108);
    table->setColumnWidth(2, 105);
    table->setColumnWidth(3, 150);
    table->setColumnWidth(4, 162);
    table->setColumnWidth(5, 78);
    table->horizontalHeader()->setStretchLastSection(true);
    objects->addWidget(table);
    auto *details = ui::note("Select an event to inspect its details.");
    details->setTextFormat(Qt::PlainText);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    objects->addWidget(details);
    auto *actions = new QHBoxLayout;
    objects->addLayout(actions);
    auto *inspect = ui::button("Open saved dataset", Symbol::Search, "receiveOpenDataset");
    inspect->setEnabled(false);
    auto *logs = ui::button("Open related diagnostics", Symbol::Logs, "receiveOpenLogs");
    actions->addWidget(inspect);
    actions->addWidget(logs);
    actions->addStretch();

    auto view = std::make_shared<ReceiveView>();
    auto *inspectRunner = new dicom::OperationRunner(m_logger, widget);
    const auto refresh = [=, this] {
        const auto state = m_receiver.state();
        const bool busy = m_receiver.active() || state == State::Starting;
        if (!busy)
            view->runningProfile.reset();
        const auto &profile = view->runningProfile ? *view->runningProfile : selectedProfile(true);
        ae->setText(profile.ae);
        port->setText(QString::number(profile.port));
        bind->setText(profile.address.trimmed().isEmpty() ? "127.0.0.1 (loopback default)" : profile.address);
        if (!busy && (view->profileId != profile.id || view->defaultDirectory != profile.directory)) {
            directory->setText(profile.directory);
            view->profileId = profile.id;
            view->defaultDirectory = profile.directory;
        }
        for (QWidget *control : {static_cast<QWidget *>(selector), static_cast<QWidget *>(directory),
                                 static_cast<QWidget *>(browse), static_cast<QWidget *>(strict)})
            control->setEnabled(!busy);
        start->setEnabled(!busy);
        stop->setEnabled(busy && state != State::Stopping);
        ui::status(status, m_receiver.message(), state == State::Error ? "error" :
                   state == State::Listening ? "success" : "info");
    };
    watch(refresh);
    connect(browse, &QPushButton::clicked, this, [this, directory] {
        const auto selected = QFileDialog::getExistingDirectory(this, "Choose received-file directory",
            directory->text(), QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
        if (!selected.isEmpty())
            directory->setText(selected);
    });
    connect(start, &QPushButton::clicked, this, [=, this] {
        view->runningProfile = selectedProfile(true);
        m_receiver.start(*view->runningProfile, directory->text(), strict->isChecked());
    });
    connect(stop, &QPushButton::clicked, &m_receiver, &dicom::StorageReceiver::stop);
    connect(&m_receiver, &dicom::StorageReceiver::stateChanged, widget, [=, this] {
        if (m_receiver.state() == State::Starting) {
            table->setRowCount(0);
            view->files = view->bytes = 0;
            totals->setText("0 files saved · 0 bytes");
        }
        refresh();
        closeWhenOperationsFinish();
    });
    connect(&m_receiver, &dicom::StorageReceiver::eventReceived, widget, [=](const dicom::ReceiveEvent &event) {
        if (!event.path.isEmpty() && event.status == 0) {
            ++view->files;
            view->bytes += quint64(event.bytes);
            totals->setText(QString("%1 files saved · %2 bytes").arg(view->files).arg(view->bytes));
        }
        if (table->rowCount() == 300)
            table->removeRow(0);
        const int row = table->rowCount();
        table->insertRow(row);
        const QStringList fields{event.timestamp.toLocalTime().toString("HH:mm:ss"), event.callingAe,
            event.action, event.sopClass, event.sopInstance,
            event.path.isEmpty() ? QString("—") : QString::number(event.bytes),
            event.status ? dicom::statusDescription(*event.status) : event.message};
        for (int column = 0; column < fields.size(); ++column) {
            auto *item = new QTableWidgetItem(fields.at(column));
            item->setToolTip("<qt>" + fields.at(column).toHtmlEscaped() + "</qt>");
            table->setItem(row, column, item);
        }
        auto *item = table->item(row, 0);
        item->setData(Qt::UserRole, event.path);
        item->setData(Qt::UserRole + 1, event.association);
        const auto originator = event.moveOriginatorId && !event.moveOriginatorAe.isEmpty()
            ? QString("\nMove Originator %1 · Message ID %2").arg(event.moveOriginatorAe).arg(*event.moveOriginatorId)
            : QString{};
        item->setData(Qt::UserRole + 2, QString("%1\nCalling AE %2 · Called AE %3 · Peer %4%5%6")
            .arg(event.message, event.callingAe, event.calledAe, event.peer, originator,
                 event.path.isEmpty() ? QString() : u'\n' + event.path));
        if (table->currentRow() < 0)
            table->selectRow(row);
    });
    const auto selection = [table]() -> QTableWidgetItem * {
        return table->item(table->currentRow(), 0);
    };
    const auto refreshSelection = [=] {
        const auto *item = selection();
        inspect->setEnabled(view->inspecting.isEmpty() && item && !item->data(Qt::UserRole).toString().isEmpty());
        details->setText(item ? item->data(Qt::UserRole + 2).toString()
                              : QString("Select an event to inspect its details."));
    };
    connect(table, &QTableWidget::itemSelectionChanged, widget, refreshSelection);
    connect(logs, &QPushButton::clicked, this, [this, selection] {
        const auto *item = selection();
        const auto id = item ? item->data(Qt::UserRole + 1).toString() : QString();
        selectPage(Logs);
        findChild<QPushButton *>("logReset")->click();
        findChild<QLineEdit *>("logComponent")->setText("Storage Receive");
        findChild<QLineEdit *>("logOperation")->setText(id);
    });
    connect(inspect, &QPushButton::clicked, widget, [=] {
        const auto *item = selection();
        if (!item || !view->inspecting.isEmpty())
            return;
        view->inspecting = item->data(Qt::UserRole).toString();
        view->snapshot.reset();
        inspectRunner->start("Inspect received dataset", dicom::inspectReceivedFile(view->inspecting,
            [widget, view](dicom::DatasetSnapshot snapshot) {
                QMetaObject::invokeMethod(widget, [view, snapshot = std::move(snapshot)] {
                    view->snapshot = snapshot;
                }, Qt::QueuedConnection);
            }), false, "Storage Receive");
        refreshSelection();
    });
    connect(inspectRunner, &dicom::OperationRunner::finished, widget,
        [=](const QUuid &, const dicom::OperationResult &result) {
            const auto fileName = QFileInfo(view->inspecting).fileName();
            view->inspecting.clear();
            refreshSelection();
            if (result.error)
                details->setText(result.error->message);
            else if (view->snapshot)
                ui::showDatasetInspector(*view->snapshot, "Saved DICOM dataset · " + fileName, widget);
            view->snapshot.reset();
        });
    refresh();
    return widget;
}

}  // namespace dingobench
