// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"
#include "dingobench/dicom/Verification.h"

#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <tuple>

namespace dingobench {
using ui::Symbol;

namespace {
QLineEdit *field(const QString &placeholder = {}, const char *name = nullptr)
{
    auto *input = new QLineEdit;
    input->setPlaceholderText(placeholder);
    if (name)
        input->setObjectName(QLatin1String(name));
    return input;
}

QSpinBox *number(int value, int lower, int upper)
{
    auto *widget = new QSpinBox;
    widget->setRange(lower, upper);
    widget->setValue(value);
    return widget;
}

QString endpoint(const QString &address, int port)
{
    const auto host = address.contains(':') ? "[" + address + "]" : address;
    return QString("%1:%2").arg(host).arg(port);
}

QHBoxLayout *line(QVBoxLayout *parent)
{
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    parent->addLayout(row);
    return row;
}

QLabel *value(QFormLayout *form, const QString &name)
{
    auto *label = new QLabel;
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    form->addRow(name, label);
    return label;
}

QLabel *disclosure(QVBoxLayout *layout, const QString &name)
{
    auto *toggle = new QToolButton;
    toggle->setText(name);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setIcon(ui::icon(Symbol::Right));
    toggle->setCheckable(true);
    layout->addWidget(toggle, 0, Qt::AlignLeft);
    auto *details = ui::note({});
    details->setTextFormat(Qt::PlainText);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->hide();
    layout->addWidget(details);
    QObject::connect(toggle, &QToolButton::toggled, details, [toggle, details](bool open) {
        toggle->setIcon(ui::icon(open ? Symbol::Down : Symbol::Right));
        details->setVisible(open);
    });
    return details;
}

QWidget *directoryPicker(QWidget *owner, QLineEdit *input, const char *name)
{
    auto *widget = new QWidget;
    auto *row = new QHBoxLayout(widget);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(input, 1);
    auto *browse = ui::button("Browse", Symbol::Folder, name);
    row->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, owner, [owner, input] {
        const auto chosen = QFileDialog::getExistingDirectory(owner, "Select local directory",
            input->text(), QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
        if (!chosen.isEmpty())
            input->setText(QDir::toNativeSeparators(chosen));
    });
    return widget;
}

}  // namespace

QWidget *MainWindow::dashboard()
{
    QVBoxLayout *body;
    auto *widget = page("Dashboard", "Open a diagnostic tool, check your connection profiles, or review this session’s operations.", &body);
    auto *columns = line(body);
    auto *actions = ui::section("Quick start", nullptr);
    auto *connection = ui::section("Current connection", nullptr);
    columns->addWidget(actions->parentWidget(), 3);
    columns->addWidget(connection->parentWidget(), 4);
    for (const auto &[target, label, icon] : {
             std::tuple{Verification, "Verify an endpoint", Symbol::Verification},
             std::tuple{Send, "Prepare a storage send", Symbol::Send},
             std::tuple{Query, "Query a DICOM archive", Symbol::Query}}) {
        auto *shortcut = ui::button(label, icon, nullptr, target == Verification);
        shortcut->setProperty("quickAction", true);
        actions->addWidget(shortcut);
        connect(shortcut, &QPushButton::clicked, this, [this, target] { selectPage(target); });
    }
    actions->addStretch();
    auto *form = new QFormLayout;
    connection->addLayout(form);
    auto *source = value(form, "Source");
    auto *destination = value(form, "Destination");
    auto *addressLabel = value(form, "Endpoint");
    auto *create = createDestinationButton();
    create->setObjectName("dashboardCreateDestination");
    connection->addWidget(create);
    connection->addStretch();
    auto *recent = ui::section("Recent DICOM Operations", body);
    auto *table = ui::table({"Time", "Service", "Peer", "Outcome", "Duration"});
    m_recentOperations = table;
    m_recentEmpty = ui::note("No DICOM operations in this session");
    m_recentEmpty->setObjectName("recentOperationsEmpty");
    recent->addWidget(m_recentEmpty);
    table->setObjectName("recentOperations");
    table->setColumnWidth(1, 150);
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table->setColumnWidth(4, 85);
    recent->addWidget(table);
    auto *logs = ui::button("Open diagnostic sessions", Symbol::Logs, "dashboardDiagnosticSessions");
    recent->addWidget(logs, 0, Qt::AlignLeft);
    connect(logs, &QPushButton::clicked, this, [this] { selectPage(Logs); });
    refreshDashboardHistory();
    watch([this, source, destination, addressLabel] {
        const auto &calling = selectedProfile(true);
        const auto &called = selectedProfile(false);
        source->setText(QString("%1  /  %2").arg(calling.name, calling.ae));
        destination->setText(called.id ? QString("%1  /  %2").arg(called.name, called.ae) : "No destination profile");
        addressLabel->setText(called.id ? endpoint(called.address, called.port) : QString("—"));
    });
    return widget;
}

void MainWindow::refreshDashboardHistory()
{
    if (!m_recentOperations)
        return;
    QList<const dicom::DiagnosticSession *> completed;
    for (const auto &session : std::as_const(m_protocolHistory))
        if (!session.synthetic && session.ended.isValid())
            completed.append(&session);
    std::ranges::stable_sort(completed, std::greater{}, &dicom::DiagnosticSession::ended);
    QList<QStringList> rows;
    for (const auto *entry : completed) {
        const auto &session = *entry;
        const auto &association = session.association;
        const QString peerAe = session.inbound ? association.callingAe : association.calledAe;
        const QString peer = !session.inbound && session.identities.destination
            ? QString("%1 / %2").arg(session.identities.destination->name, session.identities.destination->ae)
            : peerAe.isEmpty() ? QString("Unavailable") : peerAe;
        rows.append({session.ended.toLocalTime().toString("HH:mm:ss.zzz"), session.operation, peer,
                     session.result, QString("%1 ms").arg(session.durationMs)});
    }
    ui::rows(m_recentOperations, rows);
    m_recentOperations->setVisible(!rows.isEmpty());
    m_recentEmpty->setVisible(rows.isEmpty());
}

QWidget *MainWindow::verification()
{
    QVBoxLayout *body;
    auto *widget = page("Verification", "Send a live DICOM C-ECHO and inspect the peer's response.", &body);
    auto *configuration = ui::section("Association configuration", body);
    configuration->addWidget(connectionRow());
    auto *fields = line(configuration);
    auto *from = new QFormLayout;
    auto *to = new QFormLayout;
    fields->addLayout(from, 1);
    fields->addLayout(to, 1);
    auto *calling = value(from, "Calling AE");
    auto *called = value(to, "Called AE");
    auto *target = value(to, "Host / port");
    const auto addTimeout = [](QFormLayout *form, const QString &name, const QString &id) {
        auto *input = number(5, 1, dicom::maximumVerificationTimeout);
        input->setSuffix(" s");
        input->setObjectName(id);
        form->addRow(name, input);
        return input;
    };
    auto *connectionTimeout = addTimeout(from, "Connection timeout", "echoConnectionTimeout");
    auto *associationTimeout = addTimeout(to, "Association timeout", "echoAssociationTimeout");
    auto *dimseTimeout = addTimeout(from, "DIMSE timeout", "echoDimseTimeout");
    configuration->addWidget(ui::note("Outgoing connections use the system-selected local interface."));
    auto *controls = line(configuration);
    auto *send = ui::button("Send C-ECHO", Symbol::Run, "sendEcho", true);
    auto *cancel = ui::button("Cancel", Symbol::Close, "cancelEcho");
    cancel->setEnabled(false);
    controls->addWidget(send);
    controls->addWidget(cancel);
    controls->addStretch();
    auto *response = ui::section("Response", body);
    auto *status = new QLabel;
    status->setObjectName("echoResult");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    ui::status(status, "No verification result in this session.", "info");
    response->addWidget(status);
    auto *metrics = new QFormLayout;
    auto *code = value(metrics, "DICOM status");
    auto *elapsed = value(metrics, "Elapsed");
    code->setObjectName("echoDicomStatus");
    elapsed->setObjectName("echoElapsed");
    code->setText("—");
    elapsed->setText("—");
    response->addLayout(metrics);
    auto *details = disclosure(response, "Association details");
    details->setObjectName("echoDetails");
    details->setText("Association details appear after a verification attempt.");
    auto *logsButton = ui::button("Open related diagnostics", Symbol::Logs, "echoOpenLogs");
    logsButton->setEnabled(false);
    auto *diagnostics = line(response);
    diagnostics->addWidget(logsButton);
    addProtocolControls(diagnostics, "C-ECHO", "verificationProtocolSessions", "verificationProtocolDetails");
    connect(logsButton, &QPushButton::clicked, this, [this, logsButton] {
        selectPage(Logs);
        findChild<QPushButton *>("logReset")->click();
        findChild<QLineEdit *>("logOperation")->setText(logsButton->property("operationId").toString());
    });
    watch([this, calling, called, target, send] {
        const auto &source = selectedProfile(true);
        const auto &destination = selectedProfile(false);
        calling->setText(source.ae);
        called->setText(destination.id ? destination.ae : QString("—"));
        target->setText(destination.id ? endpoint(destination.address, destination.port) : QString("—"));
        send->setEnabled(m_verificationId.isNull() && destination.id);
    });
    connect(send, &QPushButton::clicked, this, [=, this] {
        if (!selectedProfile(false).id)
            return;
        m_verificationId = m_verificationRunner.start("C-ECHO", dicom::verificationOperation(
            selectedProfile(true), selectedProfile(false),
            {connectionTimeout->value(), associationTimeout->value(), dimseTimeout->value()},
            QSettings().value("protocolInspector/detailedCapture", false).toBool()),
            false, "Verification", dicom::DiagnosticSession::Identities{selectedProfile(true), selectedProfile(false)});
        if (m_verificationId.isNull())
            return;
        send->setEnabled(false);
        cancel->setEnabled(true);
        logsButton->setEnabled(true);
        logsButton->setProperty("operationId", m_verificationId.toString(QUuid::WithoutBraces));
        ui::status(status, "Verification in progress…", "info");
        code->setText("—");
        elapsed->setText("—");
        details->setText("Negotiating with the selected destination.");
        static_cast<QScrollArea *>(widget)->ensureWidgetVisible(response->parentWidget(), 10, 10);
    });
    connect(cancel, &QPushButton::clicked, this, [this, cancel, status] {
        if (m_verificationRunner.cancel(m_verificationId)) {
            cancel->setEnabled(false);
            ui::status(status, "Cancelling verification…", "warning");
        }
    });
    connect(&m_verificationRunner, &dicom::OperationRunner::finished, widget,
            [=, this](const QUuid &id, const dicom::OperationResult &result) {
        if (id != m_verificationId)
            return;
        m_verificationId = {};
        send->setEnabled(selectedProfile(false).id != 0);
        cancel->setEnabled(false);
        using Result = dicom::OperationResult::Status;
        if (result.status == Result::Succeeded)
            ui::status(status, "C-ECHO succeeded.", "success");
        else if (result.status == Result::Cancelled)
            ui::status(status, "Verification cancelled.", "warning");
        else
            ui::status(status, result.error ? result.error->message : QString("Verification failed."), "error");
        elapsed->setText(QString("%1 ms").arg(result.elapsedMs));
        code->setText(result.dicomStatus ? dicom::formatDicomStatus(*result.dicomStatus) : QString("—"));
        details->setText(result.details.isEmpty() ? "Association details are unavailable." : result.details);
        if (result.protocol)
            retainProtocol(*result.protocol);
        closeWhenOperationsFinish();
    });
    body->addStretch();
    return widget;
}

QWidget *MainWindow::worklist()
{
    QVBoxLayout *body;
    auto *widget = page("Worklist", "Query scheduled procedures or run a diagnostic Worklist Server.", &body);
    auto *tabs = new QTabWidget;
    tabs->setObjectName("worklistTabs");
    body->addWidget(tabs, 1);
    auto *queryTab = new QWidget;
    auto *serverTab = new QWidget;
    tabs->addTab(queryTab, ui::icon(Symbol::Search), "Query");
    tabs->addTab(serverTab, ui::icon(Symbol::Receive), "Server");
    auto *query = new QVBoxLayout(queryTab);
    auto *server = new QVBoxLayout(serverTab);
    buildWorklistQuery(query, static_cast<QScrollArea *>(widget));

    buildWorklistServer(server, serverTab);
    return widget;
}

QWidget *MainWindow::profiles()
{
    QVBoxLayout *body;
    auto *widget = page("Profiles", "Manage local connection profiles. Changes update every connection selector.", &body);
    auto *warning = ui::note({});
    warning->setTextFormat(Qt::PlainText);
    body->addWidget(warning);
    watch([this, warning] {
        ui::status(warning, m_profiles.loadWarning(), "warning");
        warning->setVisible(!m_profiles.loadWarning().isEmpty());
    });
    auto *tabs = new QTabWidget;
    tabs->setObjectName("profileTabs");
    body->addWidget(tabs, 1);
    for (const bool source : {true, false}) {
        auto *tab = new QWidget;
        tabs->addTab(tab, ui::icon(source ? Symbol::Send : Symbol::Receive),
                     source ? "Source profiles" : "Destination profiles");
        auto *layout = new QVBoxLayout(tab);
        auto *controls = line(layout);
        auto *add = ui::button("New", Symbol::Add, source ? "newSource" : "newDestination", true);
        auto *edit = ui::button("Edit", Symbol::Edit, source ? "editSource" : "editDestination");
        auto *duplicate = ui::button("Duplicate", Symbol::Copy, source ? "copySource" : "copyDestination");
        auto *remove = ui::button("Delete", Symbol::Delete, source ? "deleteSource" : "deleteDestination");
        auto *use = ui::button("Use profile", Symbol::Check);
        for (auto *control : {add, edit, duplicate, remove, use})
            controls->addWidget(control);
        controls->addStretch();
        auto *table = ui::table(source
            ? QStringList{"Profile", "Calling AE", "Local interface", "Port", "Received-file directory"}
            : QStringList{"Profile", "Called AE", "Hostname / IP", "Port"});
        table->setObjectName(source ? "sourceProfilesTable" : "destinationProfilesTable");
        table->setColumnWidth(0, 175);
        table->setColumnWidth(2, 140);
        table->setColumnWidth(3, 68);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        layout->addWidget(table, 1);
        auto *detail = ui::section("Selected endpoint", layout);
        auto *selected = ui::note({});
        selected->setTextFormat(Qt::PlainText);
        detail->addWidget(selected);
        detail->addWidget(ui::note(source ? "Source profiles provide the calling AE and local listener defaults."
                                        : "Destination profiles identify the called AE and remote connection address."));
        const auto id = [table] {
            const auto *item = table->currentRow() < 0 ? nullptr : table->item(table->currentRow(), 0);
            return item ? item->data(Qt::UserRole).toInt() : 0;
        };
        const auto refreshSelection = [this, source, id, selected, edit, duplicate, remove, use] {
            const auto &profiles = m_profiles.entries(profiles::kind(source));
            const int current = id();
            const auto entry = std::ranges::find(profiles, current, &profiles::Profile::id);
            const bool valid = entry != profiles.end();
            selected->setText(valid ? QString("%1  ·  %2  ·  %3%4").arg(entry->name, entry->ae,
                endpoint(entry->address.isEmpty() ? "automatic interface" : entry->address, entry->port),
                current == selectedProfile(source).id ? "  ·  Active" : "")
                : !source && profiles.isEmpty() ? "Create a destination profile to connect to a remote DICOM node."
                                                : "Select an endpoint profile.");
            edit->setEnabled(valid);
            duplicate->setEnabled(valid);
            remove->setEnabled(valid && (!source || profiles.size() > 1));
            use->setEnabled(valid);
        };
        watch([this, source, table, refreshSelection, id] {
            const int previous = id() ? id() : selectedProfile(source).id;
            QList<QStringList> data;
            const auto &profiles = m_profiles.entries(profiles::kind(source));
            for (const auto &entry : profiles) {
                QStringList columns{entry.name, entry.ae, entry.address.isEmpty() ? "Automatic" : entry.address,
                                    QString::number(entry.port)};
                if (source)
                    columns.append(entry.directory.isEmpty() ? "Not set" : entry.directory);
                data.append(columns);
            }
            ui::rows(table, data);
            int selectedRow = 0;
            for (int index = 0; index < profiles.size(); ++index) {
                table->item(index, 0)->setData(Qt::UserRole, profiles[index].id);
                if (profiles[index].id == previous)
                    selectedRow = index;
            }
            table->selectRow(selectedRow);
            refreshSelection();
        });
        connect(table, &QTableWidget::itemSelectionChanged, this, refreshSelection);
        connect(add, &QPushButton::clicked, this, [this, source] { editProfile(source, 0); });
        connect(edit, &QPushButton::clicked, this, [this, source, id] { editProfile(source, id()); });
        connect(duplicate, &QPushButton::clicked, this, [this, source, id] { editProfile(source, id(), true); });
        connect(table, &QTableWidget::cellDoubleClicked, this, [edit] { edit->click(); });
        connect(use, &QPushButton::clicked, this, [this, source, id] { selectProfile(source, id()); });
        connect(remove, &QPushButton::clicked, this, [this, source, id] {
            const auto kind = profiles::kind(source);
            const auto &entries = m_profiles.entries(kind);
            const int removed = id();
            if (!removed || (source && entries.size() <= 1))
                return;
            QMessageBox confirm(QMessageBox::Question, "Delete profile",
                removed == selectedProfile(source).id
                    ? entries.size() == 1 ? "Delete the last destination profile?"
                                          : "Delete the active profile? DingoBench will select the first remaining profile."
                    : "Delete this profile from the local configuration?",
                QMessageBox::Yes | QMessageBox::Cancel, this);
            confirm.setObjectName("confirmProfileDelete");
            confirm.setDefaultButton(QMessageBox::Cancel);
            if (confirm.exec() != QMessageBox::Yes)
                return;
            const auto error = m_profiles.remove(kind, removed);
            if (!error.isEmpty()) {
                reportProfileError(error);
                return;
            }
            m_logger.log(logging::Severity::Info, "Profiles", source ? "Source profile deleted."
                                                                    : "Destination profile deleted.");
            refreshProfiles();
        });
    }
    return widget;
}

void MainWindow::editProfile(bool source, int id, bool duplicate)
{
    const auto kind = profiles::kind(source);
    const auto &entries = m_profiles.entries(kind);
    const auto existing = std::ranges::find(entries, id, &profiles::Profile::id);
    if (id && existing == entries.end())
        return;
    const bool update = !duplicate && id != 0;
    const profiles::Profile seed = existing == entries.end()
        ? profiles::Profile{0, {}, {}, {}, source ? 11112 : 104, {}} : *existing;
    QDialog dialog(this);
    dialog.setWindowTitle(update ? "Edit profile" : duplicate ? "Duplicate profile" : "New profile");
    dialog.setMinimumWidth(405);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(ui::note("Enter connection settings. Keep credentials and patient data out of profiles."));
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *name = field("Profile name", "profileName");
    name->setMaxLength(48);
    name->setText(duplicate ? seed.name + " copy" : seed.name);
    auto *ae = field("AE title", "profileAe");
    ae->setText(seed.ae);
    auto *address = field(source ? "Interface name or IP (optional)" : "Hostname or IP address", "profileAddress");
    address->setText(seed.address);
    auto *port = field("1–65535", "profilePort");
    port->setText(QString::number(seed.port));
    form->addRow("Profile name", name);
    form->addRow(source ? "Calling AE title" : "Called AE title", ae);
    form->addRow(source ? "Local bind interface" : "Hostname / IP", address);
    form->addRow(source ? "Local port" : "Port", port);
    auto *directory = field("Optional", "profileDirectory");
    directory->setText(seed.directory);
    if (source)
        form->addRow("Received files", directoryPicker(&dialog, directory, "profileBrowse"));
    else
        directory->setParent(&dialog);
    directory->setVisible(source);
    auto *message = ui::note({});
    message->setObjectName("profileError");
    message->setTextFormat(Qt::PlainText);
    ui::status(message, {}, "error");
    layout->addWidget(message);
    auto *controls = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    controls->button(QDialogButtonBox::Save)->setIcon(ui::icon(Symbol::Save));
    controls->button(QDialogButtonBox::Cancel)->setIcon(ui::icon(Symbol::Close));
    controls->button(QDialogButtonBox::Save)->setObjectName("saveProfile");
    layout->addWidget(controls);
    connect(controls, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(controls, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto error = m_profiles.save(kind, {update ? id : 0, name->text(), ae->text(),
            address->text(), port->text().toInt(), directory->text()});
        if (!error.isEmpty()) {
            message->setText(error);
            return;
        }
        m_logger.log(logging::Severity::Info, "Profiles",
                     update ? "Connection profile edited." : duplicate ? "Connection profile duplicated."
                                                                       : "Connection profile created.",
                     {}, source ? "Source profile" : "Destination profile");
        dialog.accept();
        refreshProfiles();
    });
    name->setFocus();
    dialog.exec();
}

}  // namespace dingobench
