// SPDX-License-Identifier: GPL-3.0-only
#include "DiagnosticReportAssertions.h"
#include "ProtocolInspector.h"
#include "Ui.h"
#include "dingobench/dicom/OperationRunner.h"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcsequen.h>
#include <dcmtk/dcmdata/dcvrobow.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QtTest>

#include <algorithm>

using namespace dingobench;
using namespace dingobench::dicom;

namespace {

bool populate(DcmDataset &dataset)
{
    if (dataset.putAndInsertString(DCM_SpecificCharacterSet, "ISO_IR 100").bad() ||
        dataset.putAndInsertString(DCM_PatientName, "M\xFCller^Synthetic").bad() ||
        dataset.putAndInsertString(DCM_PatientID, "").bad() ||
        dataset.putAndInsertString(DCM_ImageType, "ORIGINAL\\PRIMARY").bad())
        return false;
    auto sequence = std::make_unique<DcmSequenceOfItems>(DCM_ScheduledProcedureStepSequence);
    for (const char *description : {"Synthetic step one", "Synthetic step two"}) {
        auto item = std::make_unique<DcmItem>();
        if (item->putAndInsertString(DCM_ScheduledProcedureStepDescription, description).bad())
            return false;
        auto nested = std::make_unique<DcmSequenceOfItems>(DCM_ScheduledProtocolCodeSequence);
        auto code = std::make_unique<DcmItem>();
        if (code->putAndInsertString(DCM_CodeValue, "SYNTHETIC").bad() ||
            nested->append(code.get()).bad())
            return false;
        code.release();
        if (item->insert(nested.get()).bad())
            return false;
        nested.release();
        if (sequence->append(item.get()).bad())
            return false;
        item.release();
    }
    if (dataset.insert(sequence.get()).bad())
        return false;
    sequence.release();
    auto privateTag = std::make_unique<DcmOtherByteOtherWord>(DcmTag(DcmTagKey(0x0011, 0x1010), EVR_OB));
    const Uint8 bytes[]{1, 2};
    if (privateTag->putUint8Array(bytes, 2).bad() || dataset.insert(privateTag.get()).bad())
        return false;
    privateTag.release();
    auto pixel = std::make_unique<DcmOtherByteOtherWord>(DcmTag(DCM_PixelData, EVR_OB));
    const QByteArray image(4 * 1024 * 1024, 'x');
    if (pixel->putUint8Array(reinterpret_cast<const Uint8 *>(image.constData()), image.size()).bad() ||
        dataset.insert(pixel.get()).bad())
        return false;
    pixel.release();
    return true;
}

const Attribute &attribute(const DatasetSnapshot &snapshot, const QString &tag)
{
    const auto it = std::ranges::find(snapshot.attributes, tag, &Attribute::tag);
    if (it != snapshot.attributes.end())
        return *it;
    static const Attribute missing;
    return missing;
}

DiagnosticSession fixture(const DatasetSnapshot &dataset)
{
    DiagnosticSession session;
    session.id = QUuid::createUuid();
    session.operation = "C-FIND model fixture";
    session.result = "Succeeded";
    session.started = QDateTime::currentDateTimeUtc();
    session.ended = session.started.addMSecs(42);
    session.durationMs = 42;
    session.synthetic = true;
    session.detailedCapture = true;
    session.association.callingAe = "FIXTURE_SCU";
    session.association.calledAe = "FIXTURE_SCP";
    DimseRecord query;
    query.timestamp = session.started;
    query.direction = Direction::Outgoing;
    query.command = "C-FIND-RQ";
    query.id = 1;
    query.context = 1;
    query.datasetPresent = true;
    session.record(query);
    DimseRecord response;
    response.timestamp = session.ended;
    response.direction = Direction::Incoming;
    response.command = "C-FIND-RSP";
    response.responseTo = 1;
    response.context = 1;
    response.status = 0xff00;
    response.datasetPresent = true;
    response.dataset = dataset;
    session.record(response);
    session.record(response);
    response.status = 0;
    response.datasetPresent = false;
    response.dataset.reset();
    session.record(response);
    return session;
}

}  // namespace

class ProtocolInspectorTest final : public QObject {
    Q_OBJECT
    QTemporaryDir m_temporary;

    static QByteArray exportReport(ui::ProtocolInspector &inspector, const QString &path)
    {
        QTimer chooser;
        connect(&chooser, &QTimer::timeout, &inspector, [&] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            chooser.stop();
            dialog->setDirectory(QFileInfo(path).absolutePath());
            dialog->findChild<QLineEdit *>("fileNameEdit")->setText(QDir::toNativeSeparators(path));
            static_cast<QDialog *>(dialog)->accept();
            if (dialog->isVisible())
                dialog->reject();
        });
        chooser.start(25);
        inspector.findChild<QPushButton *>("exportProtocolReport")->click();
        QFile exported(path);
        return exported.open(QIODevice::ReadOnly) ? exported.readAll() : QByteArray();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_temporary.isValid());
        QCoreApplication::setOrganizationName("DingoBenchTest");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_temporary.path());
        QVERIFY(ui::installTheme(*qApp));
    }

    void datasetStructure()
    {
        DcmDataset original;
        QVERIFY(populate(original));
        const auto snapshot = snapshotDataset(original);
        QVERIFY(!snapshot.truncated);
        const auto &name = attribute(snapshot, "(0010,0010)");
        QCOMPARE(name.vr, QString("PN"));
        QCOMPARE(name.vm, 1);
        QCOMPARE(name.value, QString::fromUtf8("Müller^Synthetic"));
        QCOMPARE(attribute(snapshot, "(0010,0020)").kind, Attribute::Empty);
        QVERIFY(attribute(snapshot, "(0010,0030)").tag.isEmpty());
        QCOMPARE(attribute(snapshot, "(0008,0008)").vm, 2);
        const auto &steps = attribute(snapshot, "(0040,0100)");
        QCOMPARE(steps.kind, Attribute::Sequence);
        QCOMPARE(steps.children.size(), 2);
        QCOMPARE(steps.children.first().children.last().children.first().children.first().value,
                 QString("SYNTHETIC"));
        QCOMPARE(attribute(snapshot, "(0011,1010)").kind, Attribute::Binary);
        QCOMPARE(attribute(snapshot, "(7FE0,0010)").kind, Attribute::Binary);
        QVERIFY(attribute(snapshot, "(7FE0,0010)").value.contains("4194304"));
        QVERIFY(datasetText(snapshot, true).size() < 4000);

        DcmDataset command;
        QVERIFY(command.putAndInsertUint16(DCM_CommandField, 0x0020).good());
        QVERIFY(command.putAndInsertUint16(DCM_CommandDataSetType, 0).good());
        DiagnosticSession metadataOnly;
        metadataOnly.captureCommand(command, Direction::Outgoing, {}, 1, &original);
        QVERIFY(metadataOnly.messages.first().datasetPresent == true);
        QVERIFY(!metadataOnly.messages.first().dataset);
        metadataOnly.detailedCapture = true;
        metadataOnly.captureCommand(command, Direction::Incoming, {}, 1, &original);
        QVERIFY(metadataOnly.messages.last().dataset);
    }

    void messageCorrelationAndHistoryLimit()
    {
        DiagnosticSession echo;
        DimseRecord echoRequest, echoResponse;
        echoRequest.command = "C-ECHO-RQ";
        echoRequest.direction = Direction::Incoming;
        echoRequest.id = 11;
        echoRequest.context = 1;
        echoRequest.datasetPresent = false;
        echoResponse.command = "C-ECHO-RSP";
        echoResponse.direction = Direction::Outgoing;
        echoResponse.responseTo = 11;
        echoResponse.context = 1;
        echoResponse.status = 0;
        echoResponse.datasetPresent = false;
        echo.record(echoRequest);
        echo.record(echoResponse);
        QCOMPARE(echo.requestIndex(1).value_or(-1), 0);
        QVERIFY(echo.messages.last().datasetPresent == false);

        auto session = fixture({});
        for (int row = 1; row < 4; ++row)
            QCOMPARE(session.requestIndex(row).value_or(-1), 0);
        auto storeRequest = session.messages.first();
        storeRequest.command = "C-STORE-RQ";
        storeRequest.direction = Direction::Incoming;
        storeRequest.context = 3;
        session.record(storeRequest);
        auto response = session.messages.last();
        response.command = "C-STORE-RSP";
        response.direction = Direction::Outgoing;
        response.responseTo = 1;
        response.context = 1;
        session.record(response);
        QVERIFY(!session.requestIndex(5));
        response.context = 3;
        session.record(response);
        QCOMPARE(session.requestIndex(6).value_or(-1), 4);
        for (int i = 0; i < 25; ++i)
            session.record(response);
        QCOMPARE(session.messages.size(), 24);
        QVERIFY(session.messagesTruncated);
    }

    void detailedCaptureOnWorker()
    {
        logging::Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        const auto *guiThread = QThread::currentThread();
        const auto id = runner.start("Synthetic capture", [guiThread](auto, const auto &) {
            DcmDataset dataset, command;
            if (QThread::currentThread() == guiThread || !populate(dataset) ||
                command.putAndInsertUint16(DCM_CommandField, 0x0020).bad())
                return OperationResult::failed({ErrorCode::Internal, "Synthetic capture fixture failed"});
            OperationResult result;
            result.protocol.emplace();
            result.protocol->detailedCapture = true;
            result.protocol->captureCommand(command, Direction::Outgoing, QDateTime::currentDateTimeUtc(), 1, &dataset);
            return result;
        }, true);
        QVERIFY(!id.isNull());
        QTRY_COMPARE(finished.size(), 1);
        const auto result = finished.first().at(1).value<OperationResult>();
        QVERIFY(result.protocol);
        QCOMPARE(result.protocol->id, id);
        QVERIFY(result.protocol->synthetic);
        QVERIFY(result.protocol->messages.first().dataset);
        QCOMPARE(attribute(*result.protocol->messages.first().dataset, "(7FE0,0010)").kind, Attribute::Binary);
    }

    void longSessionExportAndRedaction()
    {
        DcmDataset original;
        QVERIFY(populate(original));
        auto session = fixture(snapshotDataset(original));
        session.identities.source.emplace(profiles::Profile{5, "Sensitive profile name", "PRIVATE_AE",
            "private.invalid", 104, "unused-directory"});
        session.association.requestedEndpoint = "private.invalid:104";
        session.error = "password=very-secret";
        session.technicalError = "Patient Müller, token=very-secret";
        session.status = 0;
        auto pending = session.messages.at(1);
        pending.sopClass = "not-a-uid-very-secret";
        const auto final = session.messages.takeLast();
        for (int i = 0; i < 10'000; ++i) {
            session.record(pending);
            session.addLog({quint64(i + 1), session.started, logging::Severity::Info, "Query / Retrieve",
                i == 0 ? "Operation started." : "password=very-secret",
                session.id.toString(QUuid::WithoutBraces), "Müller^Synthetic", true});
        }
        session.record(final);
        QCOMPARE(session.messages.size(), 24);
        QCOMPARE(session.messagesOmitted, 9980);
        QCOMPARE(session.messages.last().status.value_or(1), quint16(0));
        QCOMPARE(session.logs.size(), 256);
        QCOMPARE(session.logsOmitted, 9744);
        QCOMPARE(session.logs.last().sequence, quint64(10'000));
        verifyDiagnosticReport(session, "long-session");

        const auto report = sessionJson(session);
        const auto messages = report.value("messages").toArray();
        QVERIFY(messages.at(1).toObject().value("datasetCaptured").toBool());
        QVERIFY(messages.at(1).toObject().value("dataset").isNull());
        QVERIFY(messages.at(9).toObject().value("sopClassUid").isNull());
        const auto json = QString::fromUtf8(QJsonDocument(report).toJson());
        const auto text = sessionText(session);
        for (const auto &value : {"very-secret", "private.invalid", "Sensitive profile name", "Müller", "(0010,0010)"}) {
            QVERIFY(!json.contains(QString::fromUtf8(value)));
            QVERIFY(!text.contains(QString::fromUtf8(value)));
        }
        const auto sensitive = QString::fromUtf8(QJsonDocument(sessionJson(session, true)).toJson());
        QVERIFY(sensitive.contains("private.invalid"));
        QVERIFY(sensitive.contains("Müller"));
        QVERIFY(!sensitive.contains("very-secret"));
        QVERIFY(!sessionText(session, true).contains("very-secret"));
        if (const auto directory = qEnvironmentVariable("DINGOBENCH_DIAGNOSTIC_REPORT_TEST_DIR"); !directory.isEmpty())
            QVERIFY(writeReport(QDir(directory).filePath("long-session-sensitive.json"), session, true, true).isEmpty());
        QVERIFY(QDir(m_temporary.path()).mkdir("GitHub"));
        const auto blocked = m_temporary.filePath("GitHub/blocked.json");
        QVERIFY(!writeReport(blocked, session, true).isEmpty());
        QVERIFY(!QFileInfo::exists(blocked));
        QFile invalid(m_temporary.filePath("invalid.json"));
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write(R"({"schemaVersion":2,"logs":[]})");
        invalid.close();
        QString error;
        QVERIFY(readReport(invalid.fileName(), error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void preflightFailureRetainsIdentityAndLogs()
    {
        logging::Logger logger(1);
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        profiles::Profile source{17, "Local", "BENCH", {}, 104, "not-captured"};
        profiles::Profile destination{18, "Peer", "TEST_SCP", "::1", 11112, {}};
        const auto id = runner.start("C-ECHO", [](std::stop_token, const ReportDiagnostic &) {
            return OperationResult::failed({ErrorCode::InvalidInput, "Profile validation failed."});
        }, true, "Verification", DiagnosticSession::Identities{source, destination});
        source.name = "Edited";
        QTRY_COMPARE(finished.size(), 1);
        const auto result = finished.first().at(1).value<OperationResult>();
        QVERIFY(result.protocol);
        const auto &session = *result.protocol;
        QCOMPARE(session.id, id);
        QCOMPARE(session.identities.source->name, QString("Local"));
        QCOMPARE(session.association.requestedEndpoint, QString("[::1]:11112"));
        QCOMPARE(session.association.connection, QString("Not attempted"));
        QVERIFY(!session.association.requestSent);
        QCOMPARE(session.logs.size(), 2);
        QCOMPARE(logger.snapshot().size(), 1);
        verifyDiagnosticReport(session, "preflight-failure");
    }

    void dialogTreeCopyAndExports()
    {
        DcmDataset original;
        QVERIFY(populate(original));
        const auto session = fixture(snapshotDataset(original));
        ui::ProtocolInspector inspector(session, nullptr);
        QVERIFY(inspector.isModal());
        inspector.resize(1080, 730);
        inspector.show();
        auto *tabs = inspector.findChild<QTabWidget *>("protocolTabs");
        QCOMPARE(tabs->count(), 5);
        tabs->setCurrentIndex(2);
        inspector.findChild<QTableWidget *>("protocolMessages")->selectRow(1);
        auto *messageTabs = tabs->currentWidget()->findChild<QTabWidget *>();
        messageTabs->setCurrentIndex(1);
        auto *dataset = messageTabs->currentWidget();
        auto *tree = dataset->findChild<QTreeView *>("datasetTree");
        auto *search = dataset->findChild<QLineEdit *>("datasetSearch");
        auto *fullValue = dataset->findChild<QPlainTextEdit *>("datasetFullValue");
        QVERIFY(fullValue->isHidden());
        search->setText("SYNTHETIC");
        QVERIFY(tree->model()->rowCount() > 1);
        tree->collapseAll();
        tree->expandAll();
        search->setText("(0010,0010)");
        QCOMPARE(tree->model()->rowCount(), 1);
        const auto row = tree->model()->index(0, 0);
        tree->setCurrentIndex(row);
        tree->selectionModel()->select(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QVERIFY(!fullValue->isHidden());
        QCOMPARE(fullValue->toPlainText(), QString("Müller^Synthetic"));
        dataset->findChild<QPushButton *>("copyDatasetTag")->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("(0010,0010)"));
        dataset->findChild<QPushButton *>("copyDatasetSelection")->click();
        QVERIFY(QApplication::clipboard()->text().contains("Müller^Synthetic"));
        dataset->findChild<QPushButton *>("copyDatasetDump")->click();
        QVERIFY(QApplication::clipboard()->text().contains("(0040,0100)"));

        auto *values = inspector.findChild<QCheckBox *>("includeProtocolValues");
        QVERIFY(!values->isChecked());
        inspector.findChild<QPushButton *>("copyProtocolReport")->click();
        QVERIFY(!QApplication::clipboard()->text().contains("Müller"));
        const auto text = exportReport(inspector, m_temporary.filePath("redacted.txt"));
        QVERIFY(text.contains("<redacted>"));
        QVERIFY(!QString::fromUtf8(text).contains("Müller"));
        const auto json = exportReport(inspector, m_temporary.filePath("redacted.json"));
        const auto redacted = QJsonDocument::fromJson(json).object();
        QCOMPARE(redacted.value("sessionId").toString(), session.id.toString(QUuid::WithoutBraces));
        QVERIFY(!QString::fromUtf8(json).contains("Müller"));
        bool reopenedRedacted = false;
        QTimer::singleShot(0, &inspector, [&] {
            auto *dialog = inspector.findChild<QDialog *>("savedDiagnosticReportDialog");
            if (!dialog)
                return;
            if (auto *report = dialog->findChild<QPlainTextEdit *>("savedDiagnosticReport")) {
                reopenedRedacted = report->isReadOnly() &&
                    report->toPlainText().contains(session.id.toString(QUuid::WithoutBraces)) &&
                    !report->toPlainText().contains("Müller");
            }
            dialog->accept();
        });
        ui::openDiagnosticReport(&inspector, m_temporary.filePath("redacted.json"));
        QVERIFY(reopenedRedacted);
        values->setChecked(true);
        QVERIFY(QString::fromUtf8(exportReport(inspector, m_temporary.filePath("unredacted.json"))).contains("Müller"));
        tabs->setCurrentIndex(3);
        QVERIFY(inspector.findChild<QPlainTextEdit *>("protocolRaw")->toPlainText().contains("DIMSE", Qt::CaseInsensitive));
        tabs->setCurrentIndex(2);
        inspector.findChild<QTableWidget *>("protocolMessages")->selectRow(0);
        QVERIFY(fullValue->isHidden());
        inspector.findChild<QTableWidget *>("protocolMessages")->selectRow(1);
        search->clear();
        tree->expandAll();
        values->setChecked(false);
        const auto screenshots = qEnvironmentVariable("DINGOBENCH_PROTOCOL_CAPTURE_DIR");
        if (!screenshots.isEmpty()) {
            inspector.resize(1100, 900);
            const auto firstStep = tree->model()->match(tree->model()->index(0, 1), Qt::DisplayRole,
                "ScheduledProcedureStepSequence", 1, Qt::MatchExactly | Qt::MatchRecursive);
            if (!firstStep.isEmpty())
                tree->scrollTo(firstStep.first(), QAbstractItemView::PositionAtTop);
            QCoreApplication::processEvents();
            QVERIFY(inspector.grab().save(QDir(screenshots).filePath("synthetic-worklist-dataset.png")));
        }
        const auto desktop = inspector.screen()->availableGeometry().size();
        inspector.resize(qMin(760, desktop.width() - 32), qMin(670, desktop.height() - 32));
        inspector.move(14, 20);
        inspector.accept();
        ui::ProtocolInspector reopened(session, nullptr);
        QCOMPARE(reopened.size(), inspector.size());
        QTimer::singleShot(0, &reopened, &QDialog::reject);
        QCOMPARE(reopened.exec(), int(QDialog::Rejected));
    }
};

QTEST_MAIN(ProtocolInspectorTest)
#include "ProtocolInspectorTest.moc"
