// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTime>
#include <QVBoxLayout>

namespace dingobench {

void MainWindow::buildWorklistServer(QVBoxLayout *body, QWidget *page)
{
    using State = dicom::WorklistServer::State;
    using ui::Symbol;
    auto *configuration = ui::section("Worklist listener", body);
    auto *form = new QFormLayout;
    configuration->addLayout(form);
    auto *source = profileSelector(true, "worklistServerSource");
    form->addRow("Source Profile", source);
    auto *ae = ui::note({});
    ae->setTextFormat(Qt::PlainText);
    form->addRow("Called AE Title", ae);
    auto *port = new QSpinBox;
    port->setObjectName("worklistServerPort");
    port->setRange(1, 65535);
    form->addRow("Listening port", port);
    auto *family = new QComboBox;
    family->setObjectName("worklistServerFamily");
    family->addItems({"IPv4", "IPv6"});
    form->addRow("Address family", family);
    auto *bind = ui::note({});
    bind->setTextFormat(Qt::PlainText);
    configuration->addWidget(bind);
    auto *allow = new QCheckBox("Allow listening on all interfaces of this address family");
    allow->setObjectName("worklistServerAllowWildcard");
    configuration->addWidget(allow);

    const auto picker = [&](const QString &label, const char *name, bool executable) {
        auto *row = new QHBoxLayout;
        auto *input = new QLineEdit;
        input->setObjectName(QLatin1String(name));
        row->addWidget(input, 1);
        auto *browse = ui::button("Browse", Symbol::Folder);
        row->addWidget(browse);
        form->addRow(label, row);
        connect(browse, &QPushButton::clicked, page, [=] {
            const auto path = executable
                ? QFileDialog::getOpenFileName(page, "Choose the DCMTK wlmscpfs executable", input->text(),
                                                {}, nullptr, QFileDialog::DontUseNativeDialog)
                : QFileDialog::getExistingDirectory(page, "Choose the worklist record directory", input->text(),
                    QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
            if (!path.isEmpty())
                input->setText(QDir::toNativeSeparators(path));
        });
        return std::pair(input, browse);
    };
    const auto [directory, browseDirectory] = picker("Worklist files", "worklistServerDirectory", false);
    directory->setPlaceholderText("Directory containing .wl records");
    const auto [utility, browseUtility] = picker("wlmscpfs executable", "worklistServerExecutable", true);
    utility->setText(QDir::toNativeSeparators(dicom::findWorklistServerUtility()));
    utility->setPlaceholderText("Choose an installed DCMTK executable");

    auto *actions = new QHBoxLayout;
    configuration->addLayout(actions);
    auto *start = ui::button("Start server", Symbol::Run, "worklistServerStart", true);
    auto *stop = ui::button("Stop server", Symbol::Stop, "worklistServerStop");
    auto *rescan = ui::button("Refresh records", Symbol::Refresh, "worklistServerRefresh");
    auto *generate = ui::button("Generate 50 samples", Symbol::Add, "worklistServerGenerate");
    for (auto *button : {start, stop, rescan, generate})
        actions->addWidget(button);
    actions->addStretch();
    auto *status = ui::note({});
    status->setObjectName("worklistServerStatus");
    status->setTextFormat(Qt::PlainText);
    configuration->addWidget(status);
    configuration->addWidget(ui::note("Each start loads a fresh snapshot. Stop the server before refreshing or changing records."));

    auto *inventory = ui::section("Worklist records", body);
    auto *recordCount = ui::note("Choose a directory and refresh to check its records.");
    recordCount->setObjectName("worklistServerRecordCount");
    inventory->addWidget(recordCount);
    auto *records = ui::table({"Record", "Station AE", "Modality", "Start date", "Validation"});
    records->setObjectName("worklistServerRecords");
    records->setMinimumHeight(144);
    records->horizontalHeader()->setStretchLastSection(true);
    inventory->addWidget(records);

    auto *events = ui::section("Process diagnostics and inbound activity", body);
    auto *table = ui::table({"Time", "Event", "Detail"});
    table->setObjectName("worklistServerActivity");
    table->setMinimumHeight(145);
    table->setColumnWidth(0, 90);
    table->setColumnWidth(1, 195);
    table->horizontalHeader()->setStretchLastSection(true);
    events->addWidget(table);
    auto *logs = ui::button("Open server logs", Symbol::Logs, "worklistServerLogs");
    events->addWidget(logs, 0, Qt::AlignLeft);

    const auto displayed = std::make_shared<profiles::Profile>();
    const auto updateSource = [=, this] {
        if (m_worklistServer.busy())
            return;
        const auto &profile = selectedProfile(true);
        if (displayed->id == profile.id && displayed->port == profile.port &&
            displayed->ae == profile.ae && displayed->address == profile.address)
            return;
        *displayed = profile;
        ae->setText(profile.ae);
        port->setValue(profile.port);
        family->setCurrentIndex(profile.address.contains(u':') ? 1 : 0);
        const auto requested = profile.address.isEmpty() ? QString("unspecified") : profile.address;
        bind->setText(QString("Source bind setting: %1. wlmscpfs supports address family selection and listens on all interfaces of that family.")
            .arg(requested));
        allow->setChecked(false);
    };
    const auto refresh = [=, this] {
        const auto state = m_worklistServer.state();
        const bool idle = !m_worklistServer.busy();
        for (auto *input : QList<QWidget *>{source, port, family, allow, directory, utility, browseDirectory, browseUtility})
            input->setEnabled(idle);
        start->setEnabled(idle && allow->isChecked());
        stop->setEnabled(!idle && state != State::Stopping);
        rescan->setEnabled(idle);
        generate->setEnabled(idle);
        ui::status(status, m_worklistServer.message(), state == State::Error ? "error" :
            state == State::Listening ? "success" : "info");
    };

    watch(updateSource);
    connect(family, &QComboBox::currentIndexChanged, page, [allow] { allow->setChecked(false); });
    connect(allow, &QCheckBox::toggled, page, refresh);
    connect(rescan, &QPushButton::clicked, page, [=, this] {
        m_worklistServer.refreshRecords(directory->text().trimmed());
    });
    connect(generate, &QPushButton::clicked, page, [=, this] {
        if (directory->text().trimmed().isEmpty())
            browseDirectory->click();
        const auto path = directory->text().trimmed();
        if (!path.isEmpty())
            m_worklistServer.generateSamples(path);
    });
    connect(start, &QPushButton::clicked, page, [=, this] {
        table->setRowCount(0);
        m_worklistServer.start({selectedProfile(true), directory->text().trimmed(), utility->text().trimmed(),
                                port->value(), family->currentIndex() == 1, allow->isChecked()});
    });
    connect(stop, &QPushButton::clicked, &m_worklistServer, &dicom::WorklistServer::stop);
    connect(logs, &QPushButton::clicked, page, [this] {
        selectPage(Logs);
        findChild<QPushButton *>("logReset")->click();
        findChild<QLineEdit *>("logComponent")->setText("Worklist Server");
    });
    connect(&m_worklistServer, &dicom::WorklistServer::stateChanged, page, [=, this] {
        updateSource();
        refresh();
        closeWhenOperationsFinish();
    });
    connect(&m_worklistServer, &dicom::WorklistServer::recordsChanged, page, [=, this] {
        int valid = 0;
        QList<QStringList> rows;
        for (const auto &record : m_worklistServer.records()) {
            valid += record.issue.isEmpty();
            rows.append({record.file, record.station, record.modality, record.date,
                         record.issue.isEmpty() ? QString("Ready") : record.issue});
        }
        ui::rows(records, rows);
        recordCount->setText(QString("%1 available · %2 rejected").arg(valid).arg(rows.size() - valid));
    });
    connect(&m_worklistServer, &dicom::WorklistServer::activity, page, [table](const QString &event, const QString &detail) {
        if (table->rowCount() == 200)
            table->removeRow(0);
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(QTime::currentTime().toString("HH:mm:ss")));
        table->setItem(row, 1, new QTableWidgetItem(event));
        table->setItem(row, 2, new QTableWidgetItem(detail));
        table->scrollToBottom();
    });
    updateSource();
    refresh();
}

}  // namespace dingobench
