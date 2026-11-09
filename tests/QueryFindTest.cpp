// SPDX-License-Identifier: GPL-3.0-only
#include "DiagnosticReportAssertions.h"
#include "MainWindow.h"
#include "dingobench/dicom/QueryFind.h"

#include <dcmtk/config/osconfig.h>
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/assoc.h>
#include <dcmtk/dcmnet/dimse.h>

#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeView>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <memory>

using namespace dingobench;
using namespace dingobench::dicom;
using Status = OperationResult::Status;

namespace {

constexpr auto studyOne = "1.2.826.0.1.3680043.10.999.13.1";
constexpr auto seriesOne = "1.2.826.0.1.3680043.10.999.13.1.1";
constexpr auto instanceOne = "1.2.826.0.1.3680043.10.999.13.1.1.1";

profiles::Profile source()
{
    return {1, "Query SCU fixture", "TEST_SCU", {}, 11112, {}};
}

profiles::Profile destination(int port)
{
    return {2, "Query SCP fixture", "TEST_SCP", "127.0.0.1", port, {}};
}

QString value(const QueryMatch &match, const QString &tag)
{
    for (const auto &attribute : match.dataset.attributes)
        if (attribute.tag == tag)
            return attribute.value;
    return {};
}

OperationResult run(int port, const QueryRequest &query, QList<QueryMatch> &matches, bool wrongAe = false)
{
    auto target = destination(port);
    if (wrongAe)
        target.ae = "WRONG_AE";
    return findOperation(source(), target, query, {2, 2, 2}, true,
        [&](QueryMatch match) { matches.append(std::move(match)); })({}, [](DiagnosticEvent) {});
}

bool generate(const QString &path, int index)
{
    DcmFileFormat format;
    auto &d = *format.getDataset();
    const QByteArray study = QByteArray("1.2.826.0.1.3680043.10.999.13.") + QByteArray::number(index);
    const QByteArray series = study + ".1", instance = series + ".1";
    const Uint16 pixel = 1;
    if (d.putAndInsertString(DCM_SpecificCharacterSet, "ISO_IR 100").bad() ||
        d.putAndInsertString(DCM_SOPClassUID, UID_CTImageStorage).bad() ||
        d.putAndInsertString(DCM_SOPInstanceUID, instance.constData()).bad() ||
        d.putAndInsertString(DCM_StudyInstanceUID, study.constData()).bad() ||
        d.putAndInsertString(DCM_SeriesInstanceUID, series.constData()).bad() ||
        d.putAndInsertString(DCM_PatientID, index == 1 ? "SYNTH-A" : "SYNTH-B").bad() ||
        d.putAndInsertString(DCM_PatientName, "M\xFCller^Phantom").bad() ||
        d.putAndInsertString(DCM_StudyDate, "20260115").bad() ||
        d.putAndInsertString(DCM_AccessionNumber, "SYNTHETIC").bad() ||
        d.putAndInsertString(DCM_StudyID, "1").bad() ||
        d.putAndInsertString(DCM_Modality, "CT").bad() ||
        d.putAndInsertString(DCM_SeriesNumber, "1").bad() ||
        d.putAndInsertString(DCM_InstanceNumber, "1").bad() ||
        d.putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2").bad() ||
        d.putAndInsertUint16(DCM_Rows, 1).bad() || d.putAndInsertUint16(DCM_Columns, 1).bad() ||
        d.putAndInsertUint16(DCM_SamplesPerPixel, 1).bad() || d.putAndInsertUint16(DCM_BitsAllocated, 16).bad() ||
        d.putAndInsertUint16(DCM_BitsStored, 16).bad() || d.putAndInsertUint16(DCM_HighBit, 15).bad() ||
        d.putAndInsertUint16(DCM_PixelRepresentation, 0).bad() ||
        d.putAndInsertUint16Array(DCM_PixelData, &pixel, 1).bad())
        return false;
    return format.saveFile(OFFilename(QFile::encodeName(path).constData()), EXS_LittleEndianImplicit).good();
}

class ScriptedPeer final : public QTcpServer {
public:
    enum Mode { WarningThenFailure, Malformed, Timeout, Cancel, ManyMatches, TwoMatches };
    explicit ScriptedPeer(Mode mode) : m_mode(mode) {}
    ~ScriptedPeer() override
    {
        close();
        m_worker.waitForDone();
    }

    std::atomic<bool> received = false;
    std::atomic<bool> gotCancel = false;

protected:
    void incomingConnection(qintptr socket) override
    {
        m_worker.start([this, socket] { serve(static_cast<DcmNativeSocketType>(socket)); });
    }

private:
    void serve(DcmNativeSocketType socket)
    {
        dcmExternalSocketHandle.set(socket);
        T_ASC_Network *rawNetwork = nullptr;
        auto condition = ASC_initializeNetwork(NET_ACCEPTOR, 0, 2, &rawNetwork);
        const auto drop = [](T_ASC_Network *ptr) { ASC_dropNetwork(&ptr); };
        const std::unique_ptr<T_ASC_Network, decltype(drop)> network(rawNetwork, drop);
        T_ASC_Association *rawAssociation = nullptr;
        if (condition.good())
            condition = ASC_receiveAssociation(network.get(), &rawAssociation, ASC_DEFAULTMAXPDU,
                                               nullptr, nullptr, OFFalse, DUL_NOBLOCK, 2);
        dcmExternalSocketHandle.set(DCMNET_INVALID_SOCKET);
        const auto destroy = [](T_ASC_Association *ptr) { ASC_destroyAssociation(&ptr); };
        const std::unique_ptr<T_ASC_Association, decltype(destroy)> association(rawAssociation, destroy);
        if (condition.bad())
            return;
        T_ASC_PresentationContext proposed{};
        if (ASC_getPresentationContext(association->params, 0, &proposed).bad() ||
            OFString(proposed.abstractSyntax) != UID_FINDStudyRootQueryRetrieveInformationModel ||
            ASC_acceptPresentationContext(association.get()->params, proposed.presentationContextID,
                                          UID_LittleEndianImplicitTransferSyntax).bad() ||
            ASC_acknowledgeAssociation(association.get()).bad())
            return;

        T_ASC_PresentationContextID context = 0;
        T_DIMSE_Message message{};
        if (DIMSE_receiveCommand(association.get(), DIMSE_NONBLOCKING, 2, &context, &message, nullptr).bad() ||
            message.CommandField != DIMSE_C_FIND_RQ)
            return;
        DcmDataset *rawIdentifier = nullptr;
        condition = DIMSE_receiveDataSetInMemory(association.get(), DIMSE_NONBLOCKING, 2, &context,
                                                 &rawIdentifier, nullptr, nullptr);
        const std::unique_ptr<DcmDataset> identifier(rawIdentifier);
        OFString level, charset, name;
        if (condition.bad() || identifier->findAndGetOFString(DCM_QueryRetrieveLevel, level).bad() ||
            level != "STUDY" || !identifier->tagExists(DCM_StudyInstanceUID))
            return;
        if (m_mode == WarningThenFailure &&
            (identifier->findAndGetOFString(DCM_SpecificCharacterSet, charset).bad() || charset != "ISO_IR 192" ||
             identifier->findAndGetOFString(DCM_PatientName, name).bad() || name != "M\xC3\xBCller*"))
            return;
        received = true;
        const auto &request = message.msg.CFindRQ;
        const auto reply = [&](Uint16 status, DcmDataset *dataset = nullptr) {
            T_DIMSE_C_FindRSP response{};
            response.DimseStatus = status;
            return DIMSE_sendFindResponse(association.get(), context, &request, &response, dataset, nullptr).good();
        };

        if (m_mode == Malformed) {
            T_DIMSE_Message wrong{};
            wrong.CommandField = DIMSE_C_FIND_RSP;
            wrong.msg.CFindRSP.MessageIDBeingRespondedTo = request.MessageID + 1;
            wrong.msg.CFindRSP.DimseStatus = STATUS_FIND_Success;
            wrong.msg.CFindRSP.DataSetType = DIMSE_DATASET_NULL;
            DIMSE_sendMessageUsingMemoryData(association.get(), context, &wrong, nullptr, nullptr, nullptr, nullptr);
        } else if (m_mode == TwoMatches) {
            DcmDataset data;
            data.putAndInsertString(DCM_PatientID, "Z-FIRST");
            data.putAndInsertString(DCM_StudyInstanceUID, studyOne);
            if (!reply(STATUS_FIND_Pending_MatchesAreContinuing, &data))
                return;
            data.putAndInsertString(DCM_PatientID, "A-SECOND");
            if (!reply(STATUS_FIND_Pending_MatchesAreContinuing, &data))
                return;
            reply(STATUS_FIND_Success);
        } else if (m_mode == WarningThenFailure || m_mode == ManyMatches) {
            DcmDataset data;
            data.putAndInsertString(DCM_SpecificCharacterSet, "ISO_IR 100");
            data.putAndInsertString(DCM_PatientName, "M\xFCller^Phantom");
            data.putAndInsertString(DCM_StudyInstanceUID, studyOne);
            const int responses = m_mode == ManyMatches ? maximumQueryMatches + 1 : 1;
            for (int index = 0; index < responses; ++index)
                if (!reply(STATUS_FIND_Pending_WarningUnsupportedOptionalKeys, &data))
                    return;
            if (m_mode == WarningThenFailure)
                reply(STATUS_FIND_Error_DataSetDoesNotMatchSOPClass);
        }
        T_DIMSE_Message next{};
        condition = DIMSE_receiveCommand(association.get(), DIMSE_NONBLOCKING, 3, &context, &next, nullptr);
        if (condition.good() && next.CommandField == DIMSE_C_CANCEL_RQ &&
            next.msg.CCancelRQ.MessageIDBeingRespondedTo == request.MessageID) {
            gotCancel = true;
            reply(STATUS_FIND_Cancel);
            condition = DIMSE_receiveCommand(association.get(), DIMSE_NONBLOCKING, 2, &context, &next, nullptr);
        }
        if (condition == DUL_PEERREQUESTEDRELEASE)
            ASC_acknowledgeRelease(association.get());
    }

    Mode m_mode;
    QThreadPool m_worker;
};

}  // namespace

class QueryFindTest final : public QObject {
    Q_OBJECT

private slots:
    void validatesLevelAndVrConstraints()
    {
        QueryRequest query;
        QVERIFY(validateQuery(query).isEmpty());
        query.studyDate = "20260229";
        QVERIFY(!validateQuery(query).isEmpty());
        query.studyDate = "20261231-20260101";
        QVERIFY(!validateQuery(query).isEmpty());
        query.studyDate = "-20260101";
        query.patientName = QString::fromUtf8("M\xC3\xBCller*");
        query.modality = "CT";
        QVERIFY(validateQuery(query).isEmpty());
        query.patientId = "ONE\\TWO";
        QVERIFY(!validateQuery(query).isEmpty());
        query = {};
        query.level = QueryLevel::Series;
        QVERIFY(!validateQuery(query).isEmpty());
        query.studyUid = "1.02.3";
        QVERIFY(!validateQuery(query).isEmpty());
        query.studyUid = "1.2.*";
        QVERIFY(!validateQuery(query).isEmpty());
        query.studyUid = studyOne;
        QVERIFY(validateQuery(query).isEmpty());
        query.accession = "DISALLOWED";
        QVERIFY(!validateQuery(query).isEmpty());
        query.accession.clear();
        query.level = QueryLevel::Image;
        QVERIFY(!validateQuery(query).isEmpty());
        query.seriesUid = seriesOne;
        QVERIFY(validateQuery(query).isEmpty());
        query.instanceUid = instanceOne;
        QVERIFY(validateQuery(query).isEmpty());
        query.modality = "CT";
        QVERIFY(!validateQuery(query).isEmpty());
        QList<QueryMatch> matches;
        const auto invalid = run(1, query, matches);
        QCOMPARE(invalid.error->code, ErrorCode::InvalidInput);
        QVERIFY(matches.isEmpty());
    }

    void protocolFaults_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("malformed-response") << int(ScriptedPeer::Malformed);
        QTest::newRow("dimse-timeout") << int(ScriptedPeer::Timeout);
        QTest::newRow("cancel-acknowledged") << int(ScriptedPeer::Cancel);
        QTest::newRow("optional-key-warning-and-final-failure") << int(ScriptedPeer::WarningThenFailure);
        QTest::newRow("bounded-results") << int(ScriptedPeer::ManyMatches);
    }

    void protocolFaults()
    {
        QFETCH(int, mode);
        ScriptedPeer peer{ScriptedPeer::Mode(mode)};
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        logging::Logger logger;
        QList<QueryMatch> matches;
        QObject delivery;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        QueryRequest query;
        if (mode == ScriptedPeer::WarningThenFailure)
            query.patientName = QString::fromUtf8("M\xC3\xBCller*");
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start(10);
        const auto id = runner.start("C-FIND", findOperation(source(), destination(peer.serverPort()), query,
            {2, 2, 1}, false, [&delivery, &matches](QueryMatch match) {
                QMetaObject::invokeMethod(&delivery, [&matches, match = std::move(match)] { matches.append(match); },
                                          Qt::QueuedConnection);
            }));
        QTRY_VERIFY(peer.received.load());
        if (mode == ScriptedPeer::Cancel)
            QVERIFY(runner.cancel(id));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        const auto result = finished.first().at(1).value<OperationResult>();
        if (mode == ScriptedPeer::Timeout) {
            QVERIFY(result.error);
            QCOMPARE(result.error->code, ErrorCode::DimseTimeout);
            QVERIFY(ticks >= 5);
            QVERIFY(result.elapsedMs < 2300);
        } else if (mode == ScriptedPeer::Malformed) {
            QVERIFY(result.error);
            QCOMPARE(result.error->code, ErrorCode::ProtocolFailure);
        } else if (mode == ScriptedPeer::WarningThenFailure) {
            QCOMPARE(result.status, Status::Failed);
            QVERIFY(result.error);
            QCOMPARE(result.error->code, ErrorCode::FindFailure);
            QCOMPARE(result.dicomStatus.value_or(0), 0xa900);
            QCOMPARE(matches.size(), 1);
            QCOMPARE(matches.first().status, 0xff01);
            QCOMPARE(value(matches.first(), "(0010,0010)"), QString::fromUtf8("M\xC3\xBCller^Phantom"));
            QVERIFY(result.details.contains("1 optional-key warning"));
            QVERIFY(!sessionText(*result.protocol).contains("Phantom"));
            QVERIFY(!result.protocol->messages.first().dataset);
        } else {
            QCOMPARE(result.status, Status::Cancelled);
            QVERIFY(peer.gotCancel.load());
            QCOMPARE(result.dicomStatus.value_or(0), 0xfe00);
            if (mode == ScriptedPeer::ManyMatches) {
                QCOMPARE(matches.size(), maximumQueryMatches);
                QVERIFY(result.protocol);
                QVERIFY(result.protocol->messagesOmitted > 0);
                QVERIFY(std::ranges::count_if(result.protocol->messages, [](const auto &message) {
                    return message.status == 0xff01;
                }) > 1);
                QCOMPARE(result.protocol->messages.last().status.value_or(0), quint16(0xfe00));
                verifyDiagnosticReport(*result.protocol, "find-multiple-responses");
                QVERIFY(result.details.contains("Result limit reached"));
            }
        }
    }

    void liveWorkspaceSortAndInspect()
    {
        ScriptedPeer peer{ScriptedPeer::TwoMatches};
        QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir dir(QDir(QCoreApplication::applicationDirPath()).filePath("query-ui-XXXXXX"));
        QVERIFY(dir.isValid());
        const auto profilePath = dir.filePath("profiles.json");
        profiles::Store profiles(profilePath);
        auto target = destination(peer.serverPort());
        target.id = 0;
        QVERIFY(profiles.save(profiles::Kind::Destination, target).isEmpty());

        logging::Logger logger;
        MainWindow window(logger, false, profilePath);
        window.show();
        window.findChild<QPushButton *>("nav4")->click();
        auto *summary = window.findChild<QLabel *>("querySummary");
        auto *table = window.findChild<QTableWidget *>("queryResults");
        window.findChild<QPushButton *>("sendFind")->click();
        QTRY_VERIFY_WITH_TIMEOUT(summary->text().contains("Query complete"), 5000);
        QVERIFY(summary->text().contains("0x0000"));
        QCOMPARE(table->rowCount(), 2);
        table->sortItems(0, Qt::AscendingOrder);
        QCOMPARE(table->item(0, 0)->text(), QString("A-SECOND"));
        table->selectRow(0);

        QString inspectedValue;
        QTimer::singleShot(0, &window, [&] {
            if (auto *dialog = window.findChild<QDialog *>("dicomDatasetInspector")) {
                const auto *tree = dialog->findChild<QTreeView *>("datasetTree");
                const auto *model = tree->model();
                for (int row = 0; row < model->rowCount(); ++row)
                    for (int column = 0; column < model->columnCount(); ++column)
                        if (model->index(row, column).data().toString() == "A-SECOND")
                            inspectedValue = "A-SECOND";
                dialog->accept();
            }
        });
        window.findChild<QPushButton *>("inspectQueryDataset")->click();
        QCOMPARE(inspectedValue, QString("A-SECOND"));
        QVERIFY(window.findChild<QPushButton *>("queryProtocolDetails")->isEnabled());
        bool logged = false;
        for (const auto &event : logger.snapshot()) {
            if (event.component != "Query / Retrieve")
                continue;
            QVERIFY(!event.message.contains("A-SECOND") && !event.details.contains("A-SECOND"));
            logged |= !event.synthetic && event.message.contains("Operation completed");
        }
        QVERIFY(logged);
        QVERIFY(window.close());
    }

    void dcmqrscpStudySeriesAndImage()
    {
        const auto serverExe = qEnvironmentVariable("DINGOBENCH_DCMQRSCP");
        const auto storescu = qEnvironmentVariable("DINGOBENCH_STORESCU");
        int port = qEnvironmentVariableIntValue("DINGOBENCH_QUERY_TEST_PORT");
        if (storescu.isEmpty() || (serverExe.isEmpty() && !port))
            QSKIP("Set DINGOBENCH_STORESCU and either DINGOBENCH_DCMQRSCP or DINGOBENCH_QUERY_TEST_PORT.");
        QTemporaryDir dir(QDir(QCoreApplication::applicationDirPath()).filePath("query-synthetic-XXXXXX"));
        QVERIFY(dir.isValid());
        QProcess server;
        if (!port) {
            QTcpServer reserve;
            QVERIFY(reserve.listen(QHostAddress::LocalHost, 0));
            port = reserve.serverPort();
            reserve.close();
            QVERIFY(QDir(dir.path()).mkdir("archive"));
            QFile config(dir.filePath("dcmqrscp.cfg"));
            QVERIFY(config.open(QIODevice::WriteOnly));
            const auto contents = QString(
                "NetworkTCPPort = %1\nMaxPDUSize = 16384\nMaxAssociations = 4\n"
                "SpecificCharacterSet = \"ISO_IR 192\", override\n"
                "HostTable BEGIN\nfixture = (TEST_SCU, 127.0.0.1, 11112)\nHostTable END\n"
                "VendorTable BEGIN\nVendorTable END\n"
                "AETable BEGIN\nTEST_SCP \"%2\" RW (20, 64mb) fixture\nAETable END\n")
                .arg(port).arg(dir.filePath("archive")).toUtf8();
            QCOMPARE(config.write(contents), contents.size());
            config.close();
            server.setProcessChannelMode(QProcess::MergedChannels);
            server.start(serverExe, {"--check-find", "--disable-host-lookup", "--config", config.fileName()});
            QVERIFY2(server.waitForStarted(), qPrintable(server.errorString()));
            QTRY_VERIFY_WITH_TIMEOUT([port] {
                QTcpSocket probe;
                probe.connectToHost(QHostAddress::LocalHost, quint16(port));
                return probe.waitForConnected(75);
            }(), 4000);
        }
        const auto first = dir.filePath("one.dcm"), second = dir.filePath("two.dcm");
        QVERIFY(generate(first, 1));
        QVERIFY(generate(second, 2));
        QProcess seed;
        seed.setProcessChannelMode(QProcess::MergedChannels);
        seed.start(storescu, {"-aet", "TEST_SCU", "-aec", "TEST_SCP", "-to", "3", "-ta", "3", "-td", "3",
                              "127.0.0.1", QString::number(port), first, second});
        QVERIFY(seed.waitForFinished(8000));
        QCOMPARE(seed.exitStatus(), QProcess::NormalExit);
        QVERIFY2(seed.exitCode() == 0, seed.readAll().constData());

        QList<QueryMatch> matches;
        QueryRequest query;
        query.studyDate = "20260101-20260131";
        auto result = run(port, query, matches);
        QVERIFY2(result.status == Status::Succeeded, qPrintable(result.error ? result.error->message : result.details));
        QCOMPARE(result.dicomStatus.value_or(1), 0);
        QCOMPARE(matches.size(), 2);
        QCOMPARE(value(matches.first(), "(0010,0010)"), QString::fromUtf8("M\xC3\xBCller^Phantom"));
        QVERIFY(result.protocol->messages.size() >= 4);
        QVERIFY(result.protocol->messages.first().dataset);

        matches.clear();
        query = {};
        query.level = QueryLevel::Series;
        query.studyUid = studyOne;
        query.modality = "CT";
        result = run(port, query, matches);
        QVERIFY2(result.status == Status::Succeeded, qPrintable(result.error ? result.error->message : result.details));
        QCOMPARE(matches.size(), 1);
        QCOMPARE(value(matches.first(), "(0020,000E)"), QString(seriesOne));

        matches.clear();
        query.level = QueryLevel::Image;
        query.seriesUid = seriesOne;
        query.modality.clear();
        result = run(port, query, matches);
        QVERIFY2(result.status == Status::Succeeded, qPrintable(result.error ? result.error->message : result.details));
        QCOMPARE(matches.size(), 1);
        QCOMPARE(value(matches.first(), "(0008,0018)"), QString(instanceOne));

        matches.clear();
        query.instanceUid = "1.2.826.0.1.3680043.10.999.13.99";
        result = run(port, query, matches);
        QCOMPARE(result.status, Status::Succeeded);
        QCOMPARE(result.dicomStatus.value_or(1), 0);
        QVERIFY(matches.isEmpty());

        result = run(port, query, matches, true);
        QVERIFY(result.error);
        QCOMPARE(result.error->code, ErrorCode::AssociationRejected);
        QVERIFY(matches.isEmpty());
        if (server.state() != QProcess::NotRunning) {
            server.kill();
            QVERIFY(server.waitForFinished(2000));
        }
    }
};

QTEST_MAIN(QueryFindTest)
#include "QueryFindTest.moc"
