// SPDX-License-Identifier: GPL-3.0-only
#include "ProtocolInspector.h"
#include "Ui.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace dingobench::ui {
namespace {

using namespace dicom;

void copy(const QString &text)
{
    QApplication::clipboard()->setText(text);
}

QPlainTextEdit *textView(const QString &text, const char *name = nullptr)
{
    auto *view = new QPlainTextEdit(text);
    if (name)
        view->setObjectName(QLatin1String(name));
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    return view;
}

QString available(const QString &text)
{
    return text.isEmpty() ? QString("Unavailable") : text;
}

QString number(const std::optional<quint16> &value)
{
    return value ? QString::number(*value) : QString("—");
}

QString timestamp(const QDateTime &time)
{
    return time.isValid() ? time.toUTC().toString(Qt::ISODateWithMs) : QString("Unavailable");
}

class AttributeFilter final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void search(QString value)
    {
        m_search = std::move(value);
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        for (int column : {0, 1, 4}) {
            const auto value = sourceModel()->index(row, column, parent).data(Qt::DisplayRole).toString();
            if (value.contains(m_search, Qt::CaseInsensitive))
                return true;
        }
        return false;
    }

private:
    QString m_search;
};

void appendAttributes(QStandardItem *parent, const QList<Attribute> &entries)
{
    for (const auto &entry : entries) {
        QList<QStandardItem *> row;
        const QStringList cells{entry.tag, entry.name, entry.vr,
            entry.kind == Attribute::Item ? QString() : entry.vm < 0 ? QString("—") : QString::number(entry.vm),
            entry.value};
        for (const auto &text : cells) {
            auto *cell = new QStandardItem(text);
            cell->setToolTip("<qt>" + text.toHtmlEscaped() + "</qt>");
            row.append(cell);
        }
        parent->appendRow(row);
        appendAttributes(row.first(), entry.children);
    }
}

class DatasetInspector final : public QWidget {
public:
    explicit DatasetInspector(QWidget *parent = nullptr) : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        auto *tools = new QHBoxLayout;
        auto *search = new QLineEdit;
        search->setObjectName("datasetSearch");
        search->setPlaceholderText("Search tag, keyword, or value");
        search->setAccessibleName("Search attributes");
        search->addAction(icon(Symbol::Search), QLineEdit::LeadingPosition);
        tools->addWidget(search, 1);
        auto *expand = button("Expand", Symbol::Expand);
        auto *collapse = button("Collapse", Symbol::Collapse);
        tools->addWidget(expand);
        tools->addWidget(collapse);
        layout->addLayout(tools);

        m_model = new QStandardItemModel(this);
        m_filter = new AttributeFilter(this);
        m_filter->setRecursiveFilteringEnabled(true);
        m_filter->setSourceModel(m_model);
        m_tree = new QTreeView;
        m_tree->setObjectName("datasetTree");
        m_tree->setModel(m_filter);
        m_tree->setUniformRowHeights(true);
        m_tree->setIndentation(14);
        m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        layout->addWidget(m_tree, 1);
        m_note = note({});
        layout->addWidget(m_note);
        m_value = textView({}, "datasetFullValue");
        m_value->setMaximumHeight(82);
        m_value->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        layout->addWidget(m_value);
        m_value->hide();

        auto *actions = new QHBoxLayout;
        auto *copyTag = button("Copy tag", Symbol::Copy, "copyDatasetTag");
        auto *copyAttributes = button("Copy selected attributes", Symbol::Copy, "copyDatasetSelection");
        auto *copyDump = button("Copy dataset dump", Symbol::Copy, "copyDatasetDump");
        copyAttributes->setToolTip("Copies displayed attribute values. They may contain patient information.");
        copyDump->setToolTip(copyAttributes->toolTip());
        for (auto *action : {copyTag, copyAttributes, copyDump})
            actions->addWidget(action);
        actions->addStretch();
        layout->addLayout(actions);
        connect(search, &QLineEdit::textChanged, this, [this](const QString &text) {
            m_filter->search(text.trimmed());
            if (!text.trimmed().isEmpty())
                m_tree->expandAll();
        });
        connect(expand, &QPushButton::clicked, m_tree, &QTreeView::expandAll);
        connect(collapse, &QPushButton::clicked, m_tree, &QTreeView::collapseAll);
        connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &current) {
            const auto value = current.siblingAtColumn(4).data().toString();
            m_value->setPlainText(value);
            m_value->setVisible(!value.isEmpty());
        });
        connect(copyTag, &QPushButton::clicked, this, [this] {
            const auto tag = m_tree->currentIndex().siblingAtColumn(0).data().toString();
            if (!tag.isEmpty())
                copy(tag);
        });
        connect(copyAttributes, &QPushButton::clicked, this, [this] {
            QStringList rows;
            for (const auto &index : m_tree->selectionModel()->selectedRows()) {
                QStringList columns;
                for (int column = 0; column < 5; ++column)
                    columns.append(index.siblingAtColumn(column).data().toString());
                rows.append(columns.join(u'\t'));
            }
            if (!rows.isEmpty())
                copy(rows.join(u'\n'));
        });
        connect(copyDump, &QPushButton::clicked, this, [this] {
            if (m_snapshot)
                copy(datasetText(*m_snapshot, true));
        });
        m_copyDump = copyDump;
    }

    void display(const DatasetSnapshot *snapshot, const QString &unavailable)
    {
        m_snapshot = snapshot ? std::optional<DatasetSnapshot>(*snapshot) : std::nullopt;
        m_model->clear();
        m_model->setHorizontalHeaderLabels({"Tag", "Name / Keyword", "VR", "VM", "Value"});
        if (snapshot)
            appendAttributes(m_model->invisibleRootItem(), snapshot->attributes);
        m_tree->setColumnWidth(0, 175);
        m_tree->setColumnWidth(1, 215);
        m_tree->setColumnWidth(2, 48);
        m_tree->setColumnWidth(3, 48);
        m_note->setText(snapshot ? (snapshot->truncated ? "Capture limit reached. Some attributes or values were omitted."
                                                       : "Select an attribute to see its full captured value.") : unavailable);
        m_value->clear();
        m_value->hide();
        m_copyDump->setEnabled(snapshot != nullptr);
    }

private:
    std::optional<DatasetSnapshot> m_snapshot;
    QStandardItemModel *m_model;
    AttributeFilter *m_filter;
    QTreeView *m_tree;
    QLabel *m_note;
    QPlainTextEdit *m_value;
    QPushButton *m_copyDump;
};

QWidget *overview(const DiagnosticSession &session)
{
    auto *widget = new QWidget;
    auto *form = new QFormLayout(widget);
    const auto add = [&](const QString &label, const QString &value) {
        auto *text = new QLabel(available(value));
        text->setTextFormat(Qt::PlainText);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        text->setWordWrap(true);
        form->addRow(label, text);
    };
    add("Operation", session.operation);
    add("Session ID", session.id.toString(QUuid::WithoutBraces));
    add("Direction", session.inbound ? "Inbound" : "Outbound");
    const auto identity = [](const std::optional<DiagnosticSession::Identity> &profile) -> QString {
        if (!profile)
            return {};
        return QString("%1 (ID %2) · AE %3 · %4 · port %5").arg(available(profile->name))
            .arg(profile->id).arg(available(profile->ae), available(profile->address)).arg(profile->port);
    };
    add(session.inbound ? "Selected local receiving profile" : "Selected source profile", identity(session.identities.source));
    if (!session.inbound)
        add("Selected destination profile", identity(session.identities.destination));
    add("Connection", session.association.connection);
    add("Calling AE", session.association.callingAe);
    add("Called AE", session.association.calledAe);
    add("Configured destination", session.association.requestedEndpoint);
    add("Local endpoint", session.association.localEndpoint);
    add("Connected peer", session.association.remoteEndpoint);
    add("Start (UTC)", timestamp(session.started));
    add("End (UTC)", timestamp(session.ended));
    add("Duration", QString("%1 ms").arg(session.durationMs));
    add("Result", session.result);
    add("DICOM status", session.status ? statusDescription(*session.status) : QString());
    add("Failure category", session.errorCategory);
    add("Error", session.error);
    add("Technical error", session.technicalError);
    add("Capture source", session.captureSource);
    for (auto it = session.counts.cbegin(); it != session.counts.cend(); ++it)
        add(it.key(), QString::number(it.value()));
    add("Capture mode", session.detailedCapture ? "Detailed dataset capture enabled" : "Protocol metadata only");
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(widget);
    return scroll;
}

QWidget *associationPage(const AssociationDetails &a, QCheckBox *includeValues)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *tree = new QTreeWidget;
    tree->setObjectName("associationTree");
    tree->setHeaderLabels({"Negotiation", "Captured value"});
    tree->setColumnWidth(0, 250);
    const auto row = [](QTreeWidgetItem *parent, const QString &label, const QString &value) {
        return new QTreeWidgetItem(parent, {label, available(value)});
    };
    const auto pdu = [](const std::optional<quint32> &value) { return value ? QString::number(*value) : QString(); };
    auto *request = new QTreeWidgetItem(tree, {"Request", !a.requestSent ? "Unavailable" : *a.requestSent ? "Sent" : "Send not confirmed"});
    row(request, "Calling AE", a.callingAe);
    row(request, "Called AE", a.calledAe);
    row(request, "Application Context UID", a.applicationContext);
    row(request, "Local max receive PDU", pdu(a.localMaxPdu));
    auto *response = new QTreeWidgetItem(tree, {"Peer response", available(a.outcome)});
    row(response, "Peer max receive PDU", pdu(a.peerMaxPdu));
    row(response, "Rejection", a.rejection);
    row(response, "Closure", a.closure);
    auto *contexts = new QTreeWidgetItem(tree, {"Presentation contexts", QString::number(a.contexts.size())});
    for (const auto &context : a.contexts) {
        auto *entry = row(contexts, QString("Context %1").arg(context.id), context.outcome);
        row(entry, "SOP Class", context.sopClass);
        row(entry, "Abstract Syntax UID", context.abstractSyntax);
        for (const auto &syntax : context.proposedSyntaxes)
            row(entry, "Proposed Transfer Syntax UID", syntax);
        row(entry, "Accepted Transfer Syntax UID", context.acceptedSyntax);
        row(entry, "Proposed role", context.proposedRole);
        row(entry, "Accepted role", context.acceptedRole);
    }
    tree->expandToDepth(1);
    layout->addWidget(tree);
    auto *copyButton = button("Copy association details", Symbol::Copy);
    layout->addWidget(copyButton, 0, Qt::AlignLeft);
    QObject::connect(copyButton, &QPushButton::clicked, page, [a, includeValues] { copy(associationText(a, includeValues->isChecked())); });
    return page;
}

QWidget *messagesPage(const DiagnosticSession &session, QCheckBox *includeValues)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *messages = table({"Direction", "Time (UTC)", "Command", "ID", "Response to", "Context", "Status", "Dataset", "SOP Class UID"});
    messages->setObjectName("protocolMessages");
    messages->setMaximumHeight(155);
    messages->setSelectionMode(QAbstractItemView::SingleSelection);
    messages->setSelectionBehavior(QAbstractItemView::SelectRows);
    messages->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    QList<QStringList> data;
    for (const auto &message : session.messages) {
        const QString presence = !message.datasetPresent ? "—" : *message.datasetPresent ? "Present" : "Absent";
        data.append({directionName(message.direction), timestamp(message.timestamp), message.command,
            number(message.id), number(message.responseTo), number(message.context),
            message.status ? statusDescription(*message.status) : QString("—"), presence, message.sopClass});
    }
    rows(messages, data);
    for (int index = 0; index < session.messages.size(); ++index) {
        const auto direction = session.messages.at(index).direction;
        messages->item(index, 0)->setIcon(icon(direction == Direction::Outgoing ? Symbol::Send : Symbol::Receive));
    }
    layout->addWidget(messages);
    auto *label = note({});
    if (session.messagesOmitted)
        layout->addWidget(note(QString("%1 older DIMSE messages omitted by the capture limit.").arg(session.messagesOmitted)));
    layout->addWidget(label);
    auto *details = new QTabWidget;
    auto *command = new DatasetInspector;
    auto *dataset = new DatasetInspector;
    auto *status = new DatasetInspector;
    details->addTab(command, "Command set");
    details->addTab(dataset, "Dataset");
    details->addTab(status, "Status details");
    layout->addWidget(details, 1);
    auto *copyButton = button("Copy selected DIMSE message", Symbol::Copy);
    layout->addWidget(copyButton, 0, Qt::AlignLeft);
    const auto update = [=] {
        const int index = messages->currentRow();
        const auto *entry = index < 0 ? nullptr : &session.messages.at(index);
        copyButton->setEnabled(entry);
        command->display(entry ? &entry->commandSet : nullptr, "No DIMSE message captured.");
        dataset->display(entry && entry->dataset ? &*entry->dataset : nullptr,
            entry && entry->datasetPresent == false ? "This DIMSE message carries no dataset."
                                                     : "Dataset values were not captured.");
        status->display(entry && entry->statusDetail ? &*entry->statusDetail : nullptr, "No status details captured.");
        if (const auto request = session.requestIndex(index))
            label->setText(QString("Response to row %1.").arg(*request + 1));
        else
            label->setText(session.messages.isEmpty() ? "No DIMSE messages captured." : QString());
    };
    QObject::connect(messages, &QTableWidget::currentCellChanged, page, update);
    QObject::connect(copyButton, &QPushButton::clicked, page, [=] {
        if (const auto index = messages->currentRow(); index >= 0)
            copy(messageText(session.messages.at(index), includeValues->isChecked()));
    });
    if (!data.isEmpty())
        messages->selectRow(0);
    update();
    return page;
}

QWidget *logsPage(const DiagnosticSession &session)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *tree = new QTreeWidget;
    tree->setObjectName("diagnosticSessionLogs");
    tree->setHeaderLabels({"Time (UTC)", "Severity", "Component", "Event"});
    tree->setColumnWidth(0, 200);
    tree->setColumnWidth(2, 150);
    for (const auto &event : session.logs) {
        auto *row = new QTreeWidgetItem(tree, {timestamp(event.timestampUtc), logging::severityName(event.severity),
                                               event.component, event.message});
        if (!event.details.isEmpty())
            new QTreeWidgetItem(row, {{}, {}, "Details", event.details.simplified()});
    }
    layout->addWidget(tree);
    if (session.logs.isEmpty())
        layout->addWidget(note("No correlated log events were retained."));
    if (session.logsOmitted)
        layout->addWidget(note(QString("%1 older log events omitted by the capture limit.").arg(session.logsOmitted)));
    return page;
}

}  // namespace

ProtocolInspector::ProtocolInspector(const DiagnosticSession &session, QWidget *parent) : QDialog(parent)
{
    setObjectName("protocolInspector");
    setWindowTitle("DICOM Protocol Inspector");
    setModal(true);
    setSizeGripEnabled(true);
    resize(900, 660);
    restoreGeometry(QSettings().value("protocolInspector/geometry").toByteArray());
    auto *layout = new QVBoxLayout(this);
    auto *heading = new QLabel("DICOM Protocol Inspector");
    heading->setProperty("sectionHeading", true);
    layout->addWidget(heading);
    layout->addWidget(note(session.synthetic ? "Synthetic test fixture" : "Application-level protocol diagnostics · In-memory session"));
    auto *includeValues = new QCheckBox("Include sensitive values in reports");
    includeValues->setObjectName("includeProtocolValues");
    auto *tabs = new QTabWidget;
    tabs->setObjectName("protocolTabs");
    tabs->addTab(overview(session), icon(Symbol::Info), "Overview");
    tabs->addTab(associationPage(session.association, includeValues), icon(Symbol::Link), "Association");
    tabs->addTab(messagesPage(session, includeValues), icon(Symbol::Terminal), "DIMSE Messages");
    auto *raw = textView({}, "protocolRaw");
    tabs->addTab(raw, icon(Symbol::Files), "Raw Dump");
    tabs->addTab(logsPage(session), icon(Symbol::Logs), "Related Logs");
    const auto updateRaw = [raw, tabs, includeValues, session] {
        if (tabs->currentIndex() == 3)
            raw->setPlainText(sessionText(session, includeValues->isChecked()));
    };
    connect(tabs, &QTabWidget::currentChanged, raw, updateRaw);
    connect(includeValues, &QCheckBox::toggled, raw, updateRaw);
    layout->addWidget(tabs, 1);
    layout->addWidget(includeValues);
    layout->addWidget(note("Report copies and exports omit datasets, identities, and free-text log details by default. "
                          "This option includes profiles, endpoints, and captured attributes that may expose patient information."));
    auto *actions = new QHBoxLayout;
    auto *copyReport = button("Copy report", Symbol::Copy, "copyProtocolReport");
    auto *exportReport = button("Export…", Symbol::Export, "exportProtocolReport");
    auto *close = button("Close", Symbol::Close);
    actions->addWidget(copyReport);
    actions->addWidget(exportReport);
    actions->addStretch();
    actions->addWidget(close);
    layout->addLayout(actions);
    auto *exportStatus = note({});
    layout->addWidget(exportStatus);
    connect(copyReport, &QPushButton::clicked, this, [session, includeValues] {
        copy(sessionText(session, includeValues->isChecked()));
    });
    connect(exportReport, &QPushButton::clicked, this, [=, this] {
        const QString textFilter = "Text report (*.txt)";
        const QString jsonFilter = "JSON session (*.json)";
        QString format;
        const auto path = QFileDialog::getSaveFileName(this, "Export diagnostic session", {},
            textFilter + ";;" + jsonFilter, &format, QFileDialog::DontUseNativeDialog);
        if (path.isEmpty())
            return;
        const bool json = format == jsonFilter || path.endsWith(".json", Qt::CaseInsensitive);
        const auto extension = json ? QString(".json") : QString(".txt");
        const auto target = path.endsWith(extension, Qt::CaseInsensitive) ? path : path + extension;
        const auto error = writeReport(target, session, json, includeValues->isChecked());
        status(exportStatus, error.isEmpty() ? QString("Diagnostic report exported.") : error,
               error.isEmpty() ? "success" : "error");
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(this, &QDialog::finished, this, [this] {
        QSettings().setValue("protocolInspector/geometry", saveGeometry());
    });
}

void openDiagnosticReport(QWidget *parent, QString path)
{
    if (path.isEmpty())
        path = QFileDialog::getOpenFileName(parent, "Open diagnostic report", {},
            "DingoBench diagnostic reports (*.txt *.json)", nullptr, QFileDialog::DontUseNativeDialog);
    if (path.isEmpty())
        return;
    QString error;
    const auto report = readReport(path, error);
    QDialog dialog(parent);
    dialog.setObjectName("savedDiagnosticReportDialog");
    dialog.setWindowTitle("Saved diagnostic report");
    dialog.resize(860, 620);
    auto *layout = new QVBoxLayout(&dialog);
    if (error.isEmpty())
        layout->addWidget(textView(report, "savedDiagnosticReport"));
    else
        layout->addWidget(note(error));
    auto *close = button("Close", Symbol::Close);
    layout->addWidget(close, 0, Qt::AlignRight);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

void showDatasetInspector(const dicom::DatasetSnapshot &dataset, const QString &heading, QWidget *parent)
{
    QDialog dialog(parent);
    dialog.setObjectName("dicomDatasetInspector");
    dialog.setWindowTitle("DICOM Dataset Inspector");
    dialog.resize(820, 560);
    dialog.setSizeGripEnabled(true);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = note(heading);
    label->setTextFormat(Qt::PlainText);
    layout->addWidget(label);
    auto *inspector = new DatasetInspector;
    inspector->display(&dataset, {});
    layout->addWidget(inspector, 1);
    auto *close = button("Close", Symbol::Close);
    layout->addWidget(close, 0, Qt::AlignRight);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

}  // namespace dingobench::ui
