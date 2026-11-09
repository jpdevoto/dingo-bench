// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"
#include "dingobench/dicom/MoveRetrieve.h"
#include "dingobench/dicom/Verification.h"

#include <QComboBox>
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

#include <algorithm>
#include <memory>

namespace dingobench {
namespace {

struct MoveReceipts {
    QUuid operation;
    QString originatorAe, targetAe;
    quint16 messageId = 0;
    quint64 files = 0, failed = 0;
    dicom::DiagnosticSession activity;
};

struct LatestProgress {
    QMutex mutex;
    std::optional<dicom::MoveProgress> value;
};

QString archiveName(const profiles::Profile &archive)
{
    auto host = archive.address;
    if (host.contains(u':'))
        host = u'[' + host + u']';
    return QString("%1 · %2 at %3:%4").arg(archive.name, archive.ae, host).arg(archive.port);
}

QString displayCount(std::optional<quint16> count)
{
    return count ? QString::number(*count) : QString("—");
}

}  // namespace

void MainWindow::buildMovePanel(QVBoxLayout *body, QScrollArea *scroll, QTableWidget *results,
                               std::function<std::optional<RetrievalSelection>()> selected)
{
    using dicom::OperationResult;
    using ui::Symbol;
    auto *panel = ui::section("Retrieve selected study or series", body);
    auto *selection = ui::note({});
    selection->setTextFormat(Qt::PlainText);
    panel->addWidget(selection);
    panel->addWidget(ui::note("The archive must map the Move Destination AE to a reachable host and port. "
                              "It opens a separate C-STORE association to that target."));
    auto *form = new QFormLayout;
    panel->addLayout(form);
    auto *target = new QComboBox;
    target->setObjectName("moveTarget");
    target->addItems({"Local Storage SCP", "Other move destination"});
    form->addRow("Move target", target);
    auto *ae = new QLineEdit;
    ae->setObjectName("moveDestinationAe");
    ae->setPlaceholderText("AE Title configured on the archive");
    form->addRow("Move Destination AE", ae);
    auto *listenerRow = new QHBoxLayout;
    panel->addLayout(listenerRow);
    auto *listener = ui::note({});
    listener->setObjectName("moveListenerStatus");
    listener->setTextFormat(Qt::PlainText);
    listenerRow->addWidget(listener, 1);
    auto *openReceiver = ui::button("Open Storage Receive", Symbol::Receive, "moveOpenReceiver");
    listenerRow->addWidget(openReceiver);
    connect(openReceiver, &QPushButton::clicked, scroll, [this] { selectPage(Receive); });

    auto *timeouts = new QHBoxLayout;
    panel->addLayout(timeouts);
    const auto timeout = [timeouts](const QString &label, const char *name) {
        auto *field = new QFormLayout;
        auto *spin = new QSpinBox;
        spin->setObjectName(QLatin1String(name));
        spin->setRange(1, dicom::maximumClientTimeout);
        spin->setValue(5);
        spin->setSuffix(" s");
        field->addRow(label, spin);
        timeouts->addLayout(field);
        return spin;
    };
    auto *connection = timeout("Connection", "moveConnectionTimeout");
    auto *association = timeout("Association", "moveAssociationTimeout");
    auto *dimse = timeout("DIMSE", "moveDimseTimeout");
    dimse->setValue(30);

    auto *actions = new QHBoxLayout;
    panel->addLayout(actions);
    auto *send = ui::button("Send C-MOVE", Symbol::Send, "sendMove", true);
    auto *cancel = ui::button("Cancel", Symbol::Close, "cancelMove");
    actions->addWidget(send);
    actions->addWidget(cancel);
    actions->addStretch();
    auto *summary = ui::note("Ready to retrieve.");
    summary->setObjectName("moveSummary");
    summary->setTextFormat(Qt::PlainText);
    panel->addWidget(summary);
    auto *counts = ui::note("Archive sub-operations · Remaining — · Completed — · Failed — · Warning —");
    counts->setObjectName("moveCounts");
    panel->addWidget(counts);
    auto *local = ui::note("Local receipts · —");
    local->setObjectName("moveReceipts");
    panel->addWidget(local);
    auto *protocol = new QHBoxLayout;
    panel->addLayout(protocol);
    addProtocolControls(protocol, "C-MOVE", "moveProtocolSessions", "moveProtocolDetails");

    auto receipts = std::make_shared<MoveReceipts>();
    const auto showReceipts = [=] {
        local->setText(QString("Correlated local receipts · %1 files saved · %2 failed or interrupted")
            .arg(receipts->files).arg(receipts->failed));
    };
    const auto refresh = [=, this] {
        const bool idle = m_moveId.isNull();
        const bool localTarget = target->currentIndex() == 0;
        const auto listeningAe = m_receiver.listeningAe();
        const auto item = selected();
        if (item) {
            const auto keys = item->seriesUid.isEmpty() ? "Study " + item->studyUid
                : "Study " + item->studyUid + " · Series " + item->seriesUid;
            selection->setText(keys + "\nArchive from C-FIND · " + archiveName(item->archive));
        } else {
            selection->setText("Select a STUDY or SERIES result with valid UIDs. Run C-FIND again to use a different archive.");
        }
        listener->setText(listeningAe.isEmpty() ? m_receiver.message() + " Start the local listener in Storage Receive."
            : "Local Storage SCP listening · " + listeningAe + " · " + m_receiver.listeningEndpoint());
        ae->setEnabled(idle);
        ae->setVisible(!localTarget);
        form->labelForField(ae)->setVisible(!localTarget);
        target->setEnabled(idle);
        for (auto *spin : {connection, association, dimse})
            spin->setEnabled(idle);
        send->setEnabled(idle && item.has_value() && (!localTarget || !listeningAe.isEmpty()));
        cancel->setEnabled(!idle);
    };
    const auto showCounts = [=](dicom::MoveProgress progress) {
        counts->setText(QString("Archive sub-operations · Remaining %1 · Completed %2 · Failed %3 · Warning %4")
            .arg(displayCount(progress.remaining), displayCount(progress.completed),
                 displayCount(progress.failed), displayCount(progress.warnings)));
        if (cancel->isEnabled())
            ui::status(summary, dicom::moveStatusText(progress.status) + " · " +
                       dicom::formatDicomStatus(progress.status), "info");
    };

    auto latest = std::make_shared<LatestProgress>();
    auto *progressTimer = new QTimer(scroll);
    progressTimer->setInterval(50);
    const auto showLatest = [latest, showCounts] {
        std::optional<dicom::MoveProgress> progress;
        {
            QMutexLocker guard(&latest->mutex);
            progress.swap(latest->value);
        }
        if (progress)
            showCounts(*progress);
    };
    connect(progressTimer, &QTimer::timeout, scroll, showLatest);

    connect(target, &QComboBox::currentIndexChanged, scroll, refresh);
    connect(results, &QTableWidget::itemSelectionChanged, scroll, refresh);
    connect(&m_receiver, &dicom::StorageReceiver::stateChanged, scroll, refresh);
    connect(&m_receiver, &dicom::StorageReceiver::eventReceived, scroll,
        [=, this](const dicom::ReceiveEvent &event) {
            if (receipts->operation.isNull() || event.moveOriginatorId != receipts->messageId ||
                event.moveOriginatorAe != receipts->originatorAe || event.calledAe != receipts->targetAe)
                return;
            const bool saved = !event.path.isEmpty() && event.status == 0;
            if (saved) {
                ++receipts->files;
            } else if (event.action == "Failed" || event.action == "Interrupted") {
                ++receipts->failed;
            } else {
                return;
            }
            showReceipts();
            const auto retained = std::ranges::find(m_protocolHistory, receipts->operation, &dicom::DiagnosticSession::id);
            if (retained != m_protocolHistory.end()) {
                retained->counts["Correlated local objects saved"] = qint64(receipts->files);
                retained->counts["Correlated local failures"] = qint64(receipts->failed);
            }
            if (auto logged = m_logger.log(saved ? logging::Severity::Info : logging::Severity::Warning, "Query / Retrieve",
                    saved ? "Correlated C-STORE object saved locally." : "Correlated incoming C-STORE failed or was interrupted.",
                    receipts->operation.toString(QUuid::WithoutBraces),
                    QString("Move Originator %1 · Message ID %2").arg(receipts->originatorAe).arg(receipts->messageId))) {
                if (retained != m_protocolHistory.end())
                    retained->addLog(std::move(*logged));
                else if (m_moveId == receipts->operation)
                    receipts->activity.addLog(std::move(*logged));
            }
        });
    connect(send, &QPushButton::clicked, scroll, [=, this] {
        const auto item = selected();
        const bool localTarget = target->currentIndex() == 0;
        const auto destinationAe = localTarget ? m_receiver.listeningAe() : ae->text().trimmed();
        if (!m_moveId.isNull() || !item || (localTarget && destinationAe.isEmpty()))
            return;
        const dicom::MoveRequest request{item->studyUid, item->seriesUid, destinationAe, dicom::nextMoveMessageId()};
        if (const auto error = dicom::validateMove(request); !error.isEmpty()) {
            ui::status(summary, error, "error");
            return;
        }
        const auto source = selectedProfile(true);
        const auto progress = [latest](dicom::MoveProgress update) {
            QMutexLocker guard(&latest->mutex);
            latest->value = update;
        };
        m_moveId = m_moveRunner.start("C-MOVE", dicom::moveOperation(source, item->archive, request,
            {connection->value(), association->value(), dimse->value()},
            QSettings().value("protocolInspector/detailedCapture", false).toBool(), progress),
            false, "Query / Retrieve", dicom::DiagnosticSession::Identities{source, item->archive});
        *receipts = {m_moveId, source.ae.trimmed(), destinationAe, request.messageId};
        receipts->activity.id = m_moveId;
        progressTimer->start();
        counts->setText("Archive sub-operations · Remaining — · Completed — · Failed — · Warning —");
        showReceipts();
        ui::status(summary, "C-MOVE running…", "info");
        refresh();
    });
    connect(cancel, &QPushButton::clicked, scroll, [=, this] {
        if (m_moveRunner.cancel(m_moveId)) {
            cancel->setEnabled(false);
            ui::status(summary, "Cancelling C-MOVE…", "warning");
        }
    });
    connect(&m_moveRunner, &dicom::OperationRunner::finished, scroll,
        [=, this](const QUuid &id, const OperationResult &result) {
            if (id != m_moveId)
                return;
            m_moveId = {};
            progressTimer->stop();
            showLatest();
            if (result.protocol) {
                auto session = *result.protocol;
                for (const auto &event : receipts->activity.logs)
                    session.addLog(event);
                session.logsOmitted += receipts->activity.logsOmitted;
                session.counts["Correlated local objects saved"] = qint64(receipts->files);
                session.counts["Correlated local failures"] = qint64(receipts->failed);
                retainProtocol(session);
            }
            const bool cancelled = result.status == OperationResult::Status::Cancelled;
            const bool warning = result.status == OperationResult::Status::Warning;
            QString text = result.error ? result.error->message
                : cancelled ? QString("C-MOVE stopped")
                : result.dicomStatus ? dicom::moveStatusText(*result.dicomStatus)
                                     : QString("C-MOVE finished");
            if (cancelled && !result.dicomStatus)
                text += " · Archive cancellation unconfirmed; C-STORE may continue";
            else if (cancelled && result.dicomStatus != 0xfe00)
                text += " · Archive sent a final status during cancellation";
            text += QString(" · %1 ms").arg(result.elapsedMs);
            if (result.dicomStatus)
                text += " · " + dicom::formatDicomStatus(*result.dicomStatus);
            ui::status(summary, text, result.error ? "error" : cancelled || warning ? "warning" : "success");
            refresh();
            closeWhenOperationsFinish();
        });
    watch(refresh);
    refresh();
}

}  // namespace dingobench
