// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"
#include "ProtocolInspector.h"
#include "dingobench/dicom/QueryFind.h"
#include "dingobench/dicom/Verification.h"

#include <QComboBox>
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
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace dingobench {

QWidget *MainWindow::queryRetrieve()
{
    using dicom::QueryLevel;
    using ui::Symbol;

    QVBoxLayout *body;
    auto *widget = page("Query / Retrieve", "Find studies and series, inspect results, and retrieve them with C-MOVE or C-GET.", &body);
    body->addWidget(connectionRow());
    auto *criteria = ui::section("Study Root query", body);
    auto *columns = new QHBoxLayout;
    criteria->addLayout(columns);
    auto *left = new QFormLayout;
    auto *right = new QFormLayout;
    columns->addLayout(left, 1);
    columns->addLayout(right, 1);
    auto *level = new QComboBox;
    level->setObjectName("queryLevel");
    level->addItems({"STUDY", "SERIES", "IMAGE"});
    left->addRow("Query level", level);
    const auto field = [](QFormLayout *form, const char *label, const char *name, const char *hint) {
        auto *edit = new QLineEdit;
        edit->setObjectName(QLatin1String(name));
        edit->setPlaceholderText(QString::fromLatin1(hint));
        form->addRow(QString::fromLatin1(label), edit);
        return edit;
    };
    auto *patientId = field(left, "Patient ID", "queryPatientId", "Patient identifier");
    auto *patientName = field(left, "Patient name", "queryPatientName", "e.g. PHANTOM^ALPHA");
    auto *accession = field(left, "Accession", "queryAccession", "e.g. QA-2048");
    auto *date = field(left, "Study date", "queryStudyDate", "YYYYMMDD or YYYYMMDD-YYYYMMDD");
    auto *study = field(right, "Study Instance UID", "queryStudyUid", "Exact UID");
    auto *series = field(right, "Series Instance UID", "querySeriesUid", "Exact UID");
    auto *instance = field(right, "SOP Instance UID", "queryInstanceUid", "Exact UID");
    auto *modality = field(right, "Modality", "queryModality", "e.g. CT");
    const QList<QLineEdit *> fields{patientId, patientName, accession, date, study, series, instance, modality};
    auto *hint = ui::note({});
    criteria->addWidget(hint);
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
    auto *connection = timeout("Connection", "queryConnectionTimeout");
    auto *association = timeout("Association", "queryAssociationTimeout");
    auto *dimse = timeout("DIMSE", "queryDimseTimeout");
    auto *actions = new QHBoxLayout;
    criteria->addLayout(actions);
    auto *search = ui::button("Send C-FIND", Symbol::Search, "sendFind", true);
    auto *cancel = ui::button("Cancel", Symbol::Close, "cancelFind");
    auto *clear = ui::button("Clear filters", Symbol::Clear);
    for (auto *action : {search, cancel, clear})
        actions->addWidget(action);
    actions->addStretch();

    auto *results = ui::section("Query results", body);
    auto *summary = ui::note("No query result in this session.");
    summary->setObjectName("querySummary");
    summary->setTextFormat(Qt::PlainText);
    results->addWidget(summary);
    auto *table = ui::table({"Patient ID", "Patient name", "Accession", "Study date", "Modality",
                             "Study Instance UID", "Series Instance UID", "SOP Instance UID", "DIMSE status"});
    table->setObjectName("queryResults");
    table->setMinimumHeight(180);
    table->setWordWrap(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    const auto showColumns = [table](QueryLevel queryLevel) {
        for (int column = 0; column <= 3; ++column)
            table->setColumnHidden(column, queryLevel != QueryLevel::Study);
        table->setColumnHidden(4, queryLevel == QueryLevel::Image);
        table->setColumnHidden(6, queryLevel == QueryLevel::Study);
        table->setColumnHidden(7, queryLevel != QueryLevel::Image);
        table->horizontalHeader()->setSortIndicator(queryLevel == QueryLevel::Study ? 0 :
            queryLevel == QueryLevel::Series ? 6 : 7, Qt::AscendingOrder);
    };
    table->setColumnWidth(1, 150);
    for (int column = 5; column <= 7; ++column)
        table->setColumnWidth(column, 165);
    showColumns(QueryLevel::Study);
    results->addWidget(table);
    auto *inspect = ui::button("Inspect dataset", Symbol::Search, "inspectQueryDataset");
    auto *technical = ui::button("Technical summary", Symbol::Query, "queryTechnical");
    technical->setCheckable(true);
    auto *tools = new QHBoxLayout;
    results->addLayout(tools);
    tools->addWidget(inspect);
    tools->addWidget(technical);
    tools->addStretch();
    auto *details = ui::note({});
    details->setTextFormat(Qt::PlainText);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->hide();
    results->addWidget(details);
    auto *protocol = new QHBoxLayout;
    results->addLayout(protocol);
    addProtocolControls(protocol, "C-FIND", "queryProtocolSessions", "queryProtocolDetails");

    struct QueryResults {
        QList<dicom::QueryMatch> matches;
        QueryLevel level = QueryLevel::Study;
        QString requestedStudy;
        profiles::Profile archive;
    };
    auto found = std::make_shared<QueryResults>();
    const auto selected = [this, table, found]() -> std::optional<RetrievalSelection> {
        const auto &destinations = m_profiles.entries(profiles::Kind::Destination);
        if (found->level == QueryLevel::Image || table->selectedItems().isEmpty() ||
            std::ranges::find(destinations, found->archive.id, &profiles::Profile::id) == destinations.end())
            return std::nullopt;
        const auto *item = table->item(table->currentRow(), 0);
        if (!item)
            return std::nullopt;
        const auto &dataset = found->matches.at(item->data(Qt::UserRole).toInt()).dataset;
        const auto value = [&](const char *tag) {
            for (const auto &attribute : dataset.attributes)
                if (attribute.tag == QLatin1String(tag) && attribute.kind == dicom::Attribute::Value)
                    return attribute.value;
            return QString{};
        };
        auto studyUid = value("(0020,000D)");
        if (studyUid.isEmpty())
            studyUid = found->requestedStudy;
        auto seriesUid = found->level == QueryLevel::Series ? value("(0020,000E)") : QString{};
        if (studyUid.isEmpty() || (found->level == QueryLevel::Series && seriesUid.isEmpty()))
            return std::nullopt;
        return RetrievalSelection{std::move(studyUid), std::move(seriesUid), found->archive};
    };
    auto *retrieval = new QTabWidget;
    retrieval->setObjectName("retrieveMethodTabs");
    body->addWidget(retrieval);
    auto *move = new QWidget;
    auto *get = new QWidget;
    retrieval->addTab(move, "C-MOVE");
    retrieval->addTab(get, "C-GET");
    auto *scroll = static_cast<QScrollArea *>(widget);
    buildMovePanel(new QVBoxLayout(move), scroll, table, selected);
    buildGetPanel(new QVBoxLayout(get), scroll, table, selected);
    const auto refresh = [=, this] {
        const bool idle = m_queryId.isNull();
        const auto current = QueryLevel(level->currentIndex());
        const bool isStudy = current == QueryLevel::Study;
        const bool isImage = current == QueryLevel::Image;
        for (auto *edit : {patientId, patientName, accession, date})
            edit->setEnabled(idle && isStudy);
        study->setEnabled(idle);
        series->setEnabled(idle && !isStudy);
        instance->setEnabled(idle && isImage);
        modality->setEnabled(idle && !isImage);
        hint->setText(isStudy ? "Study UID is optional. Blank filters request all studies."
            : isImage ? "Enter exact Study and Series UIDs. SOP Instance UID is optional."
                      : "Enter an exact Study UID. Series UID and modality are optional.");
        for (auto *spin : {connection, association, dimse})
            spin->setEnabled(idle);
        level->setEnabled(idle);
        search->setEnabled(idle && selectedProfile(false).id);
        cancel->setEnabled(!idle);
        clear->setEnabled(idle);
    };
    const auto append = [=](dicom::QueryMatch match) {
        const int row = table->rowCount();
        table->insertRow(row);
        const auto value = [&](const char *tag) {
            for (const auto &attribute : match.dataset.attributes)
                if (attribute.tag == QLatin1String(tag))
                    return attribute.value;
            return QString{};
        };
        const auto modalities = value("(0008,0061)");
        const QStringList cells{value("(0010,0020)"), value("(0010,0010)"), value("(0008,0050)"),
            value("(0008,0020)"), modalities.isEmpty() ? value("(0008,0060)") : modalities,
            value("(0020,000D)"), value("(0020,000E)"), value("(0008,0018)"), dicom::formatDicomStatus(match.status)};
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells.at(column).isEmpty() ? QString("—") : cells.at(column));
            item->setData(Qt::UserRole, row);
            table->setItem(row, column, item);
        }
        found->matches.append(std::move(match));
        if (cancel->isEnabled())
            ui::status(summary, QString("Query running · %1 matching response(s)").arg(found->matches.size()), "info");
    };
    const auto openMatch = [=] {
        const auto *item = table->item(table->currentRow(), 0);
        if (item) {
            const auto match = found->matches.at(item->data(Qt::UserRole).toInt());
            ui::showDatasetInspector(match.dataset, "C-FIND result · " + dicom::formatDicomStatus(match.status), widget);
        }
    };
    inspect->setEnabled(false);
    technical->setEnabled(false);
    connect(technical, &QPushButton::toggled, details, &QWidget::setVisible);
    connect(inspect, &QPushButton::clicked, widget, openMatch);
    connect(table, &QTableWidget::cellDoubleClicked, widget, openMatch);
    connect(table, &QTableWidget::itemSelectionChanged, widget, [=] {
        inspect->setEnabled(!table->selectedItems().isEmpty());
    });
    connect(level, &QComboBox::currentIndexChanged, widget, refresh);
    connect(clear, &QPushButton::clicked, widget, [fields] {
        for (auto *edit : fields)
            edit->clear();
    });
    connect(search, &QPushButton::clicked, widget, [=, this] {
        if (!m_queryId.isNull() || !selectedProfile(false).id)
            return;
        dicom::QueryRequest query;
        query.level = QueryLevel(level->currentIndex());
        const auto text = [](const QLineEdit *edit) {
            return edit->isEnabled() ? edit->text().trimmed() : QString{};
        };
        query.patientId = text(patientId);
        query.patientName = text(patientName);
        query.accession = text(accession);
        query.studyDate = text(date);
        query.studyUid = text(study);
        query.seriesUid = text(series);
        query.instanceUid = text(instance);
        query.modality = text(modality);
        if (const auto error = dicom::validateQuery(query); !error.isEmpty()) {
            ui::status(summary, error, "error");
            static_cast<QScrollArea *>(widget)->ensureWidgetVisible(summary, 8, 12);
            return;
        }
        table->setSortingEnabled(false);
        table->setRowCount(0);
        showColumns(query.level);
        found->matches.clear();
        found->level = query.level;
        found->requestedStudy = query.studyUid;
        found->archive = selectedProfile(false);
        technical->setChecked(false);
        technical->setEnabled(false);
        details->clear();
        const auto reportMatch = [this, append](dicom::QueryMatch match) {
            QMetaObject::invokeMethod(this, [append, match = std::move(match)] { append(match); },
                                      Qt::QueuedConnection);
        };
        m_queryId = m_queryRunner.start("C-FIND", dicom::findOperation(selectedProfile(true), selectedProfile(false),
            std::move(query), {connection->value(), association->value(), dimse->value()},
            QSettings().value("protocolInspector/detailedCapture", false).toBool(), reportMatch),
            false, "Query / Retrieve", dicom::DiagnosticSession::Identities{selectedProfile(true), selectedProfile(false)});
        ui::status(summary, "Query running…", "info");
        refresh();
        static_cast<QScrollArea *>(widget)->ensureWidgetVisible(summary, 8, 12);
    });
    connect(cancel, &QPushButton::clicked, widget, [=, this] {
        if (m_queryRunner.cancel(m_queryId)) {
            cancel->setEnabled(false);
            ui::status(summary, "Cancelling C-FIND…", "warning");
        }
    });
    connect(&m_queryRunner, &dicom::OperationRunner::finished, widget,
        [=, this](const QUuid &id, const dicom::OperationResult &result) {
            if (id != m_queryId)
                return;
            m_queryId = {};
            if (result.protocol)
                retainProtocol(*result.protocol);
            const bool cancelled = result.status == dicom::OperationResult::Status::Cancelled;
            auto outcome = result.error ? result.error->message
                : cancelled ? QString("Query stopped")
                : found->matches.isEmpty() ? QString("No matches") : QString("Query complete");
            if (result.details.contains("Result limit reached"))
                outcome = "Result limit reached";
            outcome += QString(" · %1 match(es) · %2 ms").arg(found->matches.size()).arg(result.elapsedMs);
            if (result.dicomStatus)
                outcome += " · " + dicom::formatDicomStatus(*result.dicomStatus);
            ui::status(summary, outcome, result.error ? "error" : cancelled ? "warning" : "success");
            details->setText(result.details);
            technical->setEnabled(!result.details.isEmpty());
            table->setSortingEnabled(true);
            refresh();
            closeWhenOperationsFinish();
        });
    watch(refresh);
    refresh();
    return widget;
}

}  // namespace dingobench
