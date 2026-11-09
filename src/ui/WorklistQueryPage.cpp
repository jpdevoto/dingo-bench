// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"
#include "ProtocolInspector.h"
#include "dingobench/dicom/Verification.h"
#include "dingobench/dicom/WorklistFind.h"

#include <QDate>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace dingobench {
namespace {

const dicom::Attribute *attribute(const QList<dicom::Attribute> &attributes, const char *tag)
{
    const auto found = std::ranges::find_if(attributes, [tag](const dicom::Attribute &entry) {
        return entry.tag == QLatin1String(tag);
    });
    return found == attributes.end() ? nullptr : &*found;
}

QString value(const QList<dicom::Attribute> &attributes, const char *tag)
{
    const auto *entry = attribute(attributes, tag);
    return entry ? entry->value : QString{};
}

QString scheduledAt(const QList<dicom::Attribute> &step)
{
    const auto rawDate = value(step, "(0040,0002)");
    const auto date = QDate::fromString(rawDate, "yyyyMMdd");
    auto time = value(step, "(0040,0003)");
    if (time.size() >= 6 && time.at(4).isDigit())
        time.insert(4, u':');
    if (time.size() >= 4 && time.at(2).isDigit())
        time.insert(2, u':');
    return ((date.isValid() ? date.toString(Qt::ISODate) : rawDate) + u' ' + time).trimmed();
}

QStringList displayWorklistItem(const dicom::QueryMatch &match)
{
    const auto &root = match.dataset.attributes;
    const auto *sequence = attribute(root, "(0040,0100)");
    const auto step = sequence && !sequence->children.isEmpty()
        ? sequence->children.first().children : QList<dicom::Attribute>{};
    auto procedure = value(step, "(0040,0007)");
    if (procedure.isEmpty()) {
        const auto *codes = attribute(step, "(0040,0008)");
        if (codes && !codes->children.isEmpty())
            procedure = value(codes->children.first().children, "(0008,0104)");
    }
    if (procedure.isEmpty())
        procedure = value(root, "(0032,1060)");
    auto stepId = value(step, "(0040,0009)");
    if (sequence && sequence->children.size() > 1)
        stepId = QString("%1 (+%2)").arg(stepId.isEmpty() ? QString("—") : stepId)
            .arg(sequence->children.size() - 1);
    return {scheduledAt(step), value(step, "(0008,0060)"), value(step, "(0040,0001)"),
            stepId, value(root, "(0010,0020)"), value(root, "(0010,0010)"), procedure,
            dicom::formatDicomStatus(match.status)};
}

}  // namespace

void MainWindow::buildWorklistQuery(QVBoxLayout *body, QScrollArea *scroll)
{
    using ui::Symbol;

    body->addWidget(connectionRow());
    auto *criteria = ui::section("Modality Worklist query", body);
    auto *columns = new QHBoxLayout;
    criteria->addLayout(columns);
    auto *left = new QFormLayout;
    auto *right = new QFormLayout;
    columns->addLayout(left, 1);
    columns->addLayout(right, 1);
    const auto field = [](QFormLayout *form, const QString &label, const char *name, const QString &hint) {
        auto *edit = new QLineEdit;
        edit->setObjectName(QLatin1String(name));
        edit->setPlaceholderText(hint);
        form->addRow(label, edit);
        return edit;
    };
    auto *date = field(left, "Scheduled date", "worklistDate", "YYYYMMDD or YYYYMMDD-YYYYMMDD");
    auto *modality = field(left, "Modality", "worklistModality", "e.g. CT");
    auto *station = field(left, "Scheduled Station AE", "worklistStation", "Exact AE Title");
    auto *patientId = field(right, "Patient ID", "worklistPatientId", "Optional");
    auto *patientName = field(right, "Patient name", "worklistPatientName", "e.g. PHANTOM^ALPHA");
    const QList<QLineEdit *> fields{date, modality, station, patientId, patientName};
    criteria->addWidget(ui::note("Blank filters match any value. The station AE filter is independent of the Calling AE."));
    auto *timeouts = new QHBoxLayout;
    criteria->addLayout(timeouts);
    const auto timeout = [timeouts](const QString &label, const char *name) {
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
    auto *connection = timeout("Connection", "worklistConnectionTimeout");
    auto *association = timeout("Association", "worklistAssociationTimeout");
    auto *dimse = timeout("DIMSE", "worklistDimseTimeout");
    auto *actions = new QHBoxLayout;
    criteria->addLayout(actions);
    auto *search = ui::button("Send C-FIND", Symbol::Search, "sendWorklist", true);
    auto *clear = ui::button("Clear filters", Symbol::Clear);
    actions->addWidget(search);
    actions->addWidget(clear);
    actions->addStretch();

    auto *results = ui::section("Scheduled procedure steps", body);
    auto *statusRow = new QHBoxLayout;
    results->addLayout(statusRow);
    auto *summary = ui::note("No query result in this session.");
    summary->setObjectName("worklistSummary");
    summary->setTextFormat(Qt::PlainText);
    statusRow->addWidget(summary, 1);
    auto *cancel = ui::button("Cancel", Symbol::Close, "cancelWorklist");
    statusRow->addWidget(cancel);
    auto *table = ui::table({"Scheduled start", "Modality", "Station AE", "Step ID", "Patient ID", "Patient name",
                            "Procedure", "DIMSE status"});
    table->setObjectName("worklistResults");
    table->setMinimumHeight(180);
    table->setWordWrap(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    const int widths[]{156, 74, 112, 85, 94, 138, 160, 96};
    for (int column = 0; column < table->columnCount(); ++column)
        table->setColumnWidth(column, widths[column]);
    table->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    results->addWidget(table);
    auto *tools = new QHBoxLayout;
    results->addLayout(tools);
    auto *inspect = ui::button("Inspect dataset", Symbol::Search, "inspectWorklistDataset");
    tools->addWidget(inspect);
    tools->addStretch();
    addProtocolControls(tools, "MWL C-FIND", "worklistProtocolSessions", "worklistProtocolDetails");

    auto matches = std::make_shared<QList<dicom::QueryMatch>>();
    const auto refresh = [=, this] {
        const bool idle = m_worklistId.isNull();
        for (auto *edit : fields)
            edit->setEnabled(idle);
        for (auto *spin : {connection, association, dimse})
            spin->setEnabled(idle);
        search->setEnabled(idle && selectedProfile(false).id);
        clear->setEnabled(idle);
        cancel->setEnabled(!idle);
    };
    const auto append = [=](dicom::QueryMatch match) {
        const int row = table->rowCount();
        table->insertRow(row);
        const auto cells = displayWorklistItem(match);
        for (int column = 0; column < cells.size(); ++column) {
            const auto &text = cells.at(column);
            auto *item = new QTableWidgetItem(text.isEmpty() ? QString("—") : text);
            item->setData(Qt::UserRole, row);
            item->setToolTip("<qt>" + text.toHtmlEscaped() + "</qt>");
            table->setItem(row, column, item);
        }
        matches->append(std::move(match));
        if (cancel->isEnabled())
            ui::status(summary, QString("Query running · %1 worklist item(s)").arg(matches->size()), "info");
    };
    const auto openMatch = [=] {
        if (const auto *item = table->item(table->currentRow(), 0)) {
            const auto match = matches->at(item->data(Qt::UserRole).toInt());
            ui::showDatasetInspector(match.dataset, "Modality Worklist result · " +
                                     dicom::formatDicomStatus(match.status), scroll);
        }
    };
    inspect->setEnabled(false);
    connect(inspect, &QPushButton::clicked, scroll, openMatch);
    connect(table, &QTableWidget::cellDoubleClicked, scroll, openMatch);
    connect(table, &QTableWidget::itemSelectionChanged, scroll, [=] {
        inspect->setEnabled(!table->selectedItems().isEmpty());
    });
    connect(clear, &QPushButton::clicked, scroll, [fields] {
        for (auto *edit : fields)
            edit->clear();
    });
    connect(search, &QPushButton::clicked, scroll, [=, this] {
        if (!m_worklistId.isNull() || !selectedProfile(false).id)
            return;
        const dicom::WorklistRequest query{date->text().trimmed(), modality->text().trimmed(),
            station->text().trimmed(), patientId->text().trimmed(), patientName->text().trimmed()};
        scroll->ensureWidgetVisible(summary, 8, 12);
        if (const auto error = dicom::validateWorklist(query); !error.isEmpty()) {
            ui::status(summary, error, "error");
            return;
        }
        table->setSortingEnabled(false);
        table->setRowCount(0);
        matches->clear();
        const auto reportMatch = [this, append](dicom::QueryMatch match) {
            QMetaObject::invokeMethod(this, [append, match = std::move(match)] { append(match); },
                                      Qt::QueuedConnection);
        };
        m_worklistId = m_worklistRunner.start("MWL C-FIND", dicom::worklistOperation(
            selectedProfile(true), selectedProfile(false), query,
            {connection->value(), association->value(), dimse->value()},
            QSettings().value("protocolInspector/detailedCapture", false).toBool(), reportMatch), false, "Worklist", dicom::DiagnosticSession::Identities{selectedProfile(true), selectedProfile(false)});
        ui::status(summary, "Query running…", "info");
        refresh();
    });
    connect(cancel, &QPushButton::clicked, scroll, [=, this] {
        if (m_worklistRunner.cancel(m_worklistId)) {
            cancel->setEnabled(false);
            ui::status(summary, "Cancelling C-FIND…", "warning");
        }
    });
    connect(&m_worklistRunner, &dicom::OperationRunner::finished, scroll,
        [=, this](const QUuid &id, const dicom::OperationResult &result) {
            if (id != m_worklistId)
                return;
            m_worklistId = {};
            if (result.protocol)
                retainProtocol(*result.protocol);
            const bool stopped = result.status == dicom::OperationResult::Status::Cancelled;
            const bool warnings = std::ranges::any_of(*matches, [](const dicom::QueryMatch &match) {
                return match.status == 0xff01;
            });
            auto outcome = result.error ? result.error->message : stopped ? QString("Query stopped")
                : matches->isEmpty() ? QString("No worklist items") : QString("Query complete");
            if (result.details.contains("Result limit reached"))
                outcome = "Result limit reached";
            outcome += QString(" · %1 item(s) · %2 ms").arg(matches->size()).arg(result.elapsedMs);
            if (result.dicomStatus)
                outcome += " · " + dicom::formatDicomStatus(*result.dicomStatus);
            if (warnings)
                outcome += " · Optional-key warning";
            ui::status(summary, outcome, result.error ? "error" : stopped || warnings ? "warning" : "success");
            table->setSortingEnabled(true);
            refresh();
            closeWhenOperationsFinish();
        });
    watch(refresh);
    refresh();
}

}  // namespace dingobench
