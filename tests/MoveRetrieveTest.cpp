// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"
#include "dingobench/dicom/MoveRetrieve.h"
#include "dingobench/dicom/GetRetrieve.h"

#include <dcmtk/config/osconfig.h>
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/assoc.h>
#include <dcmtk/dcmnet/dimse.h>

#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <memory>

using namespace dingobench;
using namespace dingobench::dicom;
using Status = OperationResult::Status;

namespace {

constexpr auto studyUid = "1.2.826.0.1.3680043.10.999.15.1";
constexpr auto originAe = "TEST_SCU";
constexpr auto receiverAe = "TEST_RECEIVE";

QString seriesUid(int index)
{
    return QString("%1.%2").arg(studyUid).arg(index);
}

QString instanceUid(int index)
{
    return seriesUid(index) + ".1";
}

profiles::Profile source()
{
    return {1, "C-MOVE origin", originAe, {}, 11112, {}};
}

profiles::Profile archive(int port)
{
    return {2, "Isolated C-MOVE archive", "TEST_ARCHIVE", "127.0.0.1", port, {}};
}

bool writeImage(const QString &path, int index)
{
    DcmFileFormat format;
    auto &d = *format.getDataset();
    const auto series = seriesUid(index).toLatin1();
    const auto instance = instanceUid(index).toLatin1();
    const auto number = QByteArray::number(index);
    const Uint16 pixel = 1;
    if (d.putAndInsertString(DCM_SOPClassUID, UID_CTImageStorage).bad() ||
        d.putAndInsertString(DCM_SOPInstanceUID, instance.constData()).bad() ||
        d.putAndInsertString(DCM_StudyInstanceUID, studyUid).bad() ||
        d.putAndInsertString(DCM_SeriesInstanceUID, series.constData()).bad() ||
        d.putAndInsertString(DCM_PatientID, "SYNTH-MOVE").bad() ||
        d.putAndInsertString(DCM_PatientName, "Phantom^Retrieve").bad() ||
        d.putAndInsertString(DCM_StudyDate, "20261108").bad() ||
        d.putAndInsertString(DCM_StudyID, "15").bad() ||
        d.putAndInsertString(DCM_Modality, "CT").bad() ||
        d.putAndInsertString(DCM_SeriesNumber, number.constData()).bad() ||
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

QSet<QString> readInstances(const QDir &dir)
{
    QSet<QString> uids;
    for (const auto &name : dir.entryList({"*.dcm"}, QDir::Files)) {
        DcmFileFormat file;
        OFString uid;
        if (file.loadFile(QFile::encodeName(dir.filePath(name)).constData()).good() &&
            file.getDataset()->findAndGetOFString(DCM_SOPInstanceUID, uid).good())
            uids.insert(QString::fromLatin1(uid.c_str()));
    }
    return uids;
}

class ArchiveFixture {
public:
    QTemporaryDir files{QDir(QCoreApplication::applicationDirPath()).filePath("move-synthetic-XXXXXX")};
    QProcess process;
    int port = 0, receivePort = 0, offlinePort = 0;
    QString error;

    ~ArchiveFixture()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(2500);
        }
    }

    bool start(const QString &serverExe, const QString &storescu, int partialPort)
    {
        QTcpServer reserveArchive, reserveReceiver, reserveOffline;
        if (!files.isValid() || !QDir(files.path()).mkdir("archive") || !QDir(files.path()).mkdir("received") ||
            !reserveArchive.listen(QHostAddress::LocalHost, 0) || !reserveReceiver.listen(QHostAddress::LocalHost, 0) ||
            !reserveOffline.listen(QHostAddress::LocalHost, 0))
            return false;
        port = reserveArchive.serverPort();
        receivePort = reserveReceiver.serverPort();
        offlinePort = reserveOffline.serverPort();
        QFile config(files.filePath("dcmqrscp.cfg"));
        if (!config.open(QIODevice::WriteOnly))
            return false;
        const auto contents = QString(
            "NetworkTCPPort = %1\nMaxPDUSize = 16384\nMaxAssociations = 4\n"
            "HostTable BEGIN\norigin = (TEST_SCU, 127.0.0.1, 11112)\n"
            "receiver = (TEST_RECEIVE, 127.0.0.1, %2)\n"
            "offline = (TEST_OFFLINE, 127.0.0.1, %3)\n"
            "partial = (TEST_PARTIAL, 127.0.0.1, %4)\nHostTable END\n"
            "VendorTable BEGIN\nVendorTable END\n"
            "AETable BEGIN\nTEST_ARCHIVE \"%5\" RW (20, 64mb) origin\nAETable END\n")
            .arg(port).arg(receivePort).arg(offlinePort).arg(partialPort).arg(files.filePath("archive")).toUtf8();
        if (config.write(contents) != contents.size())
            return false;
        config.close();
        const auto first = files.filePath("one.dcm"), second = files.filePath("two.dcm");
        if (!writeImage(first, 1) || !writeImage(second, 2))
            return false;
        reserveArchive.close();
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(serverExe, {"--check-find", "--check-move", "--disable-host-lookup",
                                  "--config", config.fileName()});
        if (!process.waitForStarted(3000)) {
            error = process.errorString();
            return false;
        }
        QElapsedTimer clock;
        clock.start();
        bool ready = false;
        while (clock.elapsed() < 3000 && process.state() != QProcess::NotRunning) {
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, quint16(port));
            if (probe.waitForConnected(50)) {
                ready = true;
                break;
            }
            QTest::qWait(25);
        }
        if (!ready) {
            error = QString::fromLocal8Bit(process.readAll());
            return false;
        }
        reserveReceiver.close();
        reserveOffline.close();
        QProcess seed;
        seed.setProcessChannelMode(QProcess::MergedChannels);
        seed.start(storescu, {"-aet", originAe, "-aec", "TEST_ARCHIVE", "-to", "3", "-ta", "3", "-td", "3",
                              "127.0.0.1", QString::number(port), first, second});
        if (seed.waitForFinished(8000) && seed.exitStatus() == QProcess::NormalExit && seed.exitCode() == 0)
            return true;
        error = QString::fromLocal8Bit(seed.readAll());
        return false;
    }
};

// Runs a fault-injecting DCMTK peer on an accepted loopback socket.
class Peer final : public QTcpServer {
public:
    enum Mode { PartialStorage, TimeoutMove, CancelMove, GetUnsupported, GetNoStorage,
                GetWrongRole, GetTimeout, GetCancel, GetPartial };
    explicit Peer(Mode mode) : m_mode(mode) {}
    ~Peer() override
    {
        close();
        m_worker.waitForDone();
    }

    std::atomic<int> requests{0};
    std::atomic<bool> gotCancel{false};

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
        if (m_mode >= GetUnsupported) {
            serveGet(association.get());
            return;
        }
        const char *classes[]{m_mode == PartialStorage ? UID_CTImageStorage : UID_MOVEStudyRootQueryRetrieveInformationModel};
        const char *syntaxes[]{UID_LittleEndianImplicitTransferSyntax, UID_LittleEndianExplicitTransferSyntax};
        if (ASC_acceptContextsWithPreferredTransferSyntaxes(association->params, classes, 1, syntaxes, 2).bad() ||
            ASC_acknowledgeAssociation(association.get()).bad())
            return;

        T_ASC_PresentationContextID context = 0;
        T_DIMSE_Message message{};
        const int expected = m_mode == PartialStorage ? 2 : 1;
        for (int index = 0; index < expected; ++index) {
            condition = DIMSE_receiveCommand(association.get(), DIMSE_NONBLOCKING, 3, &context, &message, nullptr);
            if (condition.bad() || message.CommandField != (m_mode == PartialStorage ? DIMSE_C_STORE_RQ : DIMSE_C_MOVE_RQ))
                return;
            DcmDataset *rawData = nullptr;
            condition = DIMSE_receiveDataSetInMemory(association.get(), DIMSE_NONBLOCKING, 3, &context,
                                                     &rawData, nullptr, nullptr);
            const std::unique_ptr<DcmDataset> data(rawData);
            if (condition.bad() || !data)
                return;
            if (m_mode == PartialStorage) {
                ++requests;
                T_DIMSE_C_StoreRSP response{};
                response.DimseStatus = index == 0 ? STATUS_Success : 0xa700;
                if (DIMSE_sendStoreResponse(association.get(), context, &message.msg.CStoreRQ, &response, nullptr).bad())
                    return;
            }
        }
        if (m_mode != PartialStorage) {
            T_DIMSE_C_MoveRSP pending{};
            pending.DimseStatus = STATUS_MOVE_Pending_SubOperationsAreContinuing;
            pending.opts = O_MOVE_NUMBEROFREMAININGSUBOPERATIONS | O_MOVE_NUMBEROFCOMPLETEDSUBOPERATIONS |
                O_MOVE_NUMBEROFFAILEDSUBOPERATIONS | O_MOVE_NUMBEROFWARNINGSUBOPERATIONS;
            pending.NumberOfRemainingSubOperations = 2;
            if (DIMSE_sendMoveResponse(association.get(), context, &message.msg.CMoveRQ, &pending, nullptr, nullptr).bad())
                return;
            ++requests;
        }
        T_DIMSE_Message next{};
        condition = DIMSE_receiveCommand(association.get(), DIMSE_NONBLOCKING, 4, &context, &next, nullptr);
        if (m_mode == CancelMove && condition.good() && next.CommandField == DIMSE_C_CANCEL_RQ &&
            next.msg.CCancelRQ.MessageIDBeingRespondedTo == message.msg.CMoveRQ.MessageID) {
            gotCancel = true;
            T_DIMSE_C_MoveRSP cancelled{};
            cancelled.DimseStatus = STATUS_MOVE_Cancel;
            cancelled.opts = O_MOVE_NUMBEROFREMAININGSUBOPERATIONS;
            cancelled.NumberOfRemainingSubOperations = 2;
            DIMSE_sendMoveResponse(association.get(), context, &message.msg.CMoveRQ, &cancelled, nullptr, nullptr);
            condition = DIMSE_receiveCommand(association.get(), DIMSE_NONBLOCKING, 2, &context, &next, nullptr);
        }
        if (condition == DUL_PEERREQUESTEDRELEASE)
            ASC_acknowledgeRelease(association.get());
    }

    void serveGet(T_ASC_Association *association)
    {
        T_ASC_PresentationContextID get = 0, storage = 0;
        for (int index = 0; index < ASC_countPresentationContexts(association->params); ++index) {
            T_ASC_PresentationContext proposal{};
            if (ASC_getPresentationContext(association->params, index, &proposal).bad())
                return;
            if (OFString(proposal.abstractSyntax) == UID_GETStudyRootQueryRetrieveInformationModel)
                get = proposal.presentationContextID;
            if (OFString(proposal.abstractSyntax) == UID_CTImageStorage && proposal.proposedRole == ASC_SC_ROLE_SCP)
                storage = proposal.presentationContextID;
        }
        if (!get || !storage)
            return;
        if (m_mode != GetUnsupported && ASC_acceptPresentationContext(association->params, get,
            UID_LittleEndianImplicitTransferSyntax).bad())
            return;
        if (m_mode != GetNoStorage && ASC_acceptPresentationContext(association->params, storage,
            UID_LittleEndianImplicitTransferSyntax, m_mode == GetWrongRole ? ASC_SC_ROLE_DEFAULT : ASC_SC_ROLE_SCP).bad())
            return;
        if (ASC_acknowledgeAssociation(association).bad())
            return;

        T_ASC_PresentationContextID context = 0;
        T_DIMSE_Message message{};
        auto condition = DIMSE_receiveCommand(association, DIMSE_NONBLOCKING, 3, &context, &message, nullptr);
        if (condition == DUL_PEERREQUESTEDRELEASE) {
            ASC_acknowledgeRelease(association);
            return;
        }
        if (condition.bad() || context != get || message.CommandField != DIMSE_C_GET_RQ)
            return;
        DcmDataset *raw = nullptr;
        condition = DIMSE_receiveDataSetInMemory(association, DIMSE_NONBLOCKING, 3, &context, &raw, nullptr, nullptr);
        const std::unique_ptr<DcmDataset> identifier(raw);
        if (condition.bad() || !identifier || context != get)
            return;
        const auto &request = message.msg.CGetRQ;
        const auto reply = [&](quint16 status, quint16 remaining, quint16 completed, quint16 failed,
                               DcmDataset *dataset = nullptr) {
            T_DIMSE_C_GetRSP response{};
            response.DimseStatus = status;
            response.opts = O_GET_NUMBEROFCOMPLETEDSUBOPERATIONS |
                            O_GET_NUMBEROFFAILEDSUBOPERATIONS | O_GET_NUMBEROFWARNINGSUBOPERATIONS;
            if (status == 0xff00 || status == 0xfe00)
                response.opts |= O_GET_NUMBEROFREMAININGSUBOPERATIONS;
            response.NumberOfRemainingSubOperations = remaining;
            response.NumberOfCompletedSubOperations = completed;
            response.NumberOfFailedSubOperations = failed;
            return DIMSE_sendGetResponse(association, get, &request, &response, dataset, nullptr).good();
        };
        if (!reply(0xff00, 2, 0, 0))
            return;
        ++requests;
        if (m_mode == GetPartial) {
            for (int index = 1; index <= 2; ++index) {
                T_DIMSE_Message store{};
                store.CommandField = DIMSE_C_STORE_RQ;
                auto &rq = store.msg.CStoreRQ;
                rq.MessageID = Uint16(index + 100);
                rq.Priority = DIMSE_PRIORITY_MEDIUM;
                rq.DataSetType = DIMSE_DATASET_PRESENT;
                qstrncpy(rq.AffectedSOPClassUID, UID_CTImageStorage, sizeof(rq.AffectedSOPClassUID));
                qstrncpy(rq.AffectedSOPInstanceUID, instanceUid(index).toLatin1().constData(), sizeof(rq.AffectedSOPInstanceUID));
                DcmDataset dataset;
                dataset.putAndInsertString(DCM_SOPClassUID, UID_CTImageStorage);
                dataset.putAndInsertString(DCM_SOPInstanceUID, instanceUid(1).toLatin1().constData());
                if (DIMSE_sendMessageUsingMemoryData(association, storage, &store, nullptr, &dataset,
                                                    nullptr, nullptr).bad())
                    return;
                T_DIMSE_Message response{};
                if (DIMSE_receiveCommand(association, DIMSE_NONBLOCKING, 3, &context, &response, nullptr).bad() ||
                    context != storage || response.CommandField != DIMSE_C_STORE_RSP ||
                    response.msg.CStoreRSP.MessageIDBeingRespondedTo != rq.MessageID ||
                    response.msg.CStoreRSP.DimseStatus != (index == 1 ? 0 : 0xa900))
                    return;
            }
            DcmDataset failed;
            failed.putAndInsertString(DCM_FailedSOPInstanceUIDList, instanceUid(2).toLatin1().constData());
            if (!reply(0xb000, 0, 1, 1, &failed))
                return;
        }
        T_DIMSE_Message next{};
        condition = DIMSE_receiveCommand(association, DIMSE_NONBLOCKING, 4, &context, &next, nullptr);
        if (m_mode == GetCancel && condition.good() && context == get && next.CommandField == DIMSE_C_CANCEL_RQ &&
            next.msg.CCancelRQ.MessageIDBeingRespondedTo == request.MessageID) {
            gotCancel = true;
            reply(0xfe00, 2, 0, 0);
            condition = DIMSE_receiveCommand(association, DIMSE_NONBLOCKING, 2, &context, &next, nullptr);
        }
        if (condition == DUL_PEERREQUESTEDRELEASE)
            ASC_acknowledgeRelease(association);
    }

    Mode m_mode;
    QThreadPool m_worker;
};

Operation requestMove(int archivePort, QString target, QList<MoveProgress> &progress, QString series = {},
                      ClientTimeouts timeouts = {2, 2, 4})
{
    return moveOperation(source(), archive(archivePort), {studyUid, std::move(series), std::move(target),
                         nextMoveMessageId()}, timeouts, true, [&](MoveProgress update) { progress.append(update); });
}

}  // namespace

class MoveRetrieveTest final : public QObject {
    Q_OBJECT

private slots:
    void validatesMoveKeysAndDestination()
    {
        MoveRequest request{studyUid, {}, receiverAe, 1};
        QVERIFY(validateMove(request).isEmpty());
        request.seriesUid = seriesUid(1);
        QVERIFY(validateMove(request).isEmpty());
        request.seriesUid = "1.2.*";
        QVERIFY(!validateMove(request).isEmpty());
        request.seriesUid.clear();
        request.studyUid = "1.2.3\\1.2.4";
        QVERIFY(!validateMove(request).isEmpty());
        request.studyUid = studyUid;
        request.destinationAe = QString(17, u'A');
        QVERIFY(!validateMove(request).isEmpty());
        request.destinationAe = receiverAe;
        request.messageId = 0;
        QVERIFY(!validateMove(request).isEmpty());
        const auto result = moveOperation(source(), archive(1), request, {}, false, {})({}, [](DiagnosticEvent) {});
        QVERIFY(result.error);
        QCOMPARE(result.error->code, ErrorCode::InvalidInput);
        QVERIFY(!result.protocol);
    }

    void timeoutAndCancellation()
    {
        for (const bool cancel : {false, true}) {
            Peer peer(cancel ? Peer::CancelMove : Peer::TimeoutMove);
            QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
            logging::Logger logger;
            QList<MoveProgress> progress;
            OperationRunner runner(logger);
            QSignalSpy finished(&runner, &OperationRunner::finished);
            const auto id = runner.start("C-MOVE", requestMove(peer.serverPort(), receiverAe, progress, {},
                                                               {2, 2, cancel ? 4 : 1}));
            QTRY_COMPARE(peer.requests.load(), 1);
            if (cancel)
                QVERIFY(runner.cancel(id));
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
            const auto result = finished.first().at(1).value<OperationResult>();
            QVERIFY(result.elapsedMs < 3200);
            QVERIFY(!progress.isEmpty());
            QCOMPARE(progress.first().status, 0xff00);
            QCOMPARE(progress.first().remaining.value_or(0), 2);
            if (cancel) {
                QVERIFY(peer.gotCancel.load());
                QCOMPARE(result.status, Status::Cancelled);
                QCOMPARE(result.dicomStatus.value_or(0), 0xfe00);
                QCOMPARE(progress.last().remaining.value_or(0), 2);
            } else {
                QVERIFY(result.error);
                QCOMPARE(result.error->code, ErrorCode::DimseTimeout);
                QVERIFY(!result.dicomStatus);
            }
        }
    }

    void archiveAndLiveWorkspace()
    {
        const auto serverExe = qEnvironmentVariable("DINGOBENCH_DCMQRSCP");
        const auto storescu = qEnvironmentVariable("DINGOBENCH_STORESCU");
        if (serverExe.isEmpty() || storescu.isEmpty())
            QSKIP("Set DINGOBENCH_DCMQRSCP and DINGOBENCH_STORESCU to local DCMTK executables.");
        Peer partial(Peer::PartialStorage);
        QVERIFY(partial.listen(QHostAddress::LocalHost, 0));
        ArchiveFixture peer;
        const auto started = peer.start(serverExe, storescu, partial.serverPort());
        QVERIFY2(started, qPrintable(peer.error));

        QList<MoveProgress> progress;
        auto result = requestMove(peer.port, "UNKNOWN_AE", progress)({}, [](DiagnosticEvent) {});
        QCOMPARE(result.status, Status::Failed);
        QCOMPARE(result.dicomStatus.value_or(0), 0xa801);
        QVERIFY(result.error->message.contains("AE-to-host"));
        for (const auto target : {receiverAe, "TEST_OFFLINE"}) {
            progress.clear();
            result = requestMove(peer.port, target, progress)({}, [](DiagnosticEvent) {});
            QCOMPARE(result.status, Status::Failed);
            QCOMPARE(result.dicomStatus.value_or(0), 0xa702);
            QVERIFY(result.error->message.contains("reachability"));
            QCOMPARE(progress.last().failed.value_or(0), 2);
        }

        const auto output = peer.files.filePath("received");
        const auto profilePath = peer.files.filePath("profiles.json");
        profiles::Store profiles(profilePath);
        auto pacs = archive(peer.port);
        pacs.id = 0;
        QVERIFY(profiles.save(profiles::Kind::Destination, pacs).isEmpty());
        auto scu = source();
        scu.id = 0;
        QVERIFY(profiles.save(profiles::Kind::Source, scu).isEmpty());
        const profiles::Profile scp{0, "Local move receiver", receiverAe, "127.0.0.1", peer.receivePort, output};
        QVERIFY(profiles.save(profiles::Kind::Source, scp).isEmpty());

        logging::Logger logger;
        MainWindow window(logger, false, profilePath);
        window.show();
        const auto button = [&](const char *name) { return window.findChild<QPushButton *>(QLatin1String(name)); };
        const auto label = [&](const char *name) { return window.findChild<QLabel *>(QLatin1String(name)); };
        const auto combo = [&](const char *name) { return window.findChild<QComboBox *>(QLatin1String(name)); };
        auto *sourceSelector = combo("sourceProfile");
        auto *results = window.findChild<QTableWidget *>("queryResults");
        button("nav4")->click();
        sourceSelector->setCurrentIndex(sourceSelector->findText(scu.name));
        button("sendFind")->click();
        QTRY_VERIFY_WITH_TIMEOUT(label("querySummary")->text().contains("Query complete"), 5000);
        QCOMPARE(results->rowCount(), 1);
        results->selectRow(0);
        QVERIFY(!button("sendMove")->isEnabled());
        QVERIFY(label("moveListenerStatus")->text().contains("Listener stopped"));

        button("moveOpenReceiver")->click();
        sourceSelector->setCurrentIndex(sourceSelector->findText(scp.name));
        button("receiveStart")->click();
        QTRY_VERIFY_WITH_TIMEOUT(label("receiveStatus")->text().contains("Listening on"), 5000);
        sourceSelector->setCurrentIndex(sourceSelector->findText(scu.name));
        button("nav4")->click();
        QVERIFY(label("moveListenerStatus")->text().contains(receiverAe));
        QVERIFY(button("sendMove")->isEnabled());
        button("sendMove")->click();
        QTRY_VERIFY_WITH_TIMEOUT(label("moveSummary")->text().contains("ms · 0x0000"), 8000);
        QVERIFY(label("moveCounts")->text().contains("Completed 2"));
        QVERIFY(label("moveCounts")->text().contains("Failed 0"));
        QTRY_VERIFY(label("moveReceipts")->text().contains("2 files saved"));
        const QDir saved(output);
        const QSet<QString> expected{instanceUid(1), instanceUid(2)};
        QCOMPARE(readInstances(saved), expected);
        QVERIFY(button("moveProtocolDetails")->isEnabled());
        auto *received = window.findChild<QTableWidget *>("receiveEvents");
        bool linkedStore = false;
        for (int row = 0; row < received->rowCount(); ++row) {
            const auto *item = received->item(row, 0);
            if (item && item->data(Qt::UserRole + 2).toString().contains("Move Originator TEST_SCU"))
                linkedStore = true;
        }
        QVERIFY(linkedStore);

        combo("queryLevel")->setCurrentIndex(1);
        window.findChild<QLineEdit *>("queryStudyUid")->setText(studyUid);
        button("sendFind")->click();
        QTRY_VERIFY_WITH_TIMEOUT(label("querySummary")->text().contains("Query complete"), 5000);
        QCOMPARE(results->rowCount(), 2);
        int selectedRow = -1;
        for (int row = 0; row < results->rowCount(); ++row)
            if (results->item(row, 6)->text() == seriesUid(2))
                selectedRow = row;
        QVERIFY(selectedRow >= 0);
        results->selectRow(selectedRow);
        QVERIFY(button("sendMove")->isEnabled());
        button("sendMove")->click();
        QTRY_VERIFY_WITH_TIMEOUT(label("moveSummary")->text().contains("ms · 0x0000"), 8000);
        QVERIFY(label("moveCounts")->text().contains("Completed 1"));
        QTRY_VERIFY(label("moveReceipts")->text().contains("1 files saved"));
        QCOMPARE(saved.entryList({"*.dcm"}, QDir::Files).size(), 3);
        QCOMPARE(readInstances(saved), expected);

        QProcess unrelatedStore;
        unrelatedStore.start(storescu, {"-aet", "UNRELATED", "-aec", receiverAe, "-to", "3", "-ta", "3", "-td", "3",
            "127.0.0.1", QString::number(peer.receivePort), peer.files.filePath("one.dcm")});
        QVERIFY(unrelatedStore.waitForFinished(8000));
        QCOMPARE(unrelatedStore.exitCode(), 0);
        QTRY_VERIFY(label("receiveTotals")->text().contains("4 files saved"));
        QVERIFY(label("moveReceipts")->text().contains("1 files saved"));

        button("receiveStop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(label("moveListenerStatus")->text().contains("Listener stopped"), 5000);
        QVERIFY(!button("sendMove")->isEnabled());
        QVERIFY(window.close());

        progress.clear();
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        QVERIFY(!runner.start("C-MOVE", requestMove(peer.port, "TEST_PARTIAL", progress)).isNull());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 8000);
        result = finished.first().at(1).value<OperationResult>();
        QCOMPARE(partial.requests.load(), 2);
        QCOMPARE(result.status, Status::Warning);
        QCOMPARE(result.dicomStatus.value_or(0), 0xb000);
        QVERIFY(progress.size() >= 2);
        QCOMPARE(progress.last().completed.value_or(0), 1);
        QCOMPARE(progress.last().failed.value_or(0), 1);
        QVERIFY(!progress.last().remaining);
        QVERIFY(result.protocol->messages.last().dataset);
        bool failedUidList = false;
        for (const auto &attribute : result.protocol->messages.last().dataset->attributes)
            failedUidList |= attribute.tag == "(0008,0058)" &&
                             (attribute.value == instanceUid(1) || attribute.value == instanceUid(2));
        QVERIFY(failedUidList);
    }
    void getNegotiationAndOutputErrors()
    {
        QTemporaryDir output(QDir(QCoreApplication::applicationDirPath()).filePath("get-errors-XXXXXX"));
        QVERIFY(output.isValid());
        QVERIFY(QDir(output.path()).mkdir("GitHub"));
        for (const auto &path : {output.filePath("missing"), output.filePath("GitHub")}) {
            const auto result = getOperation(source(), archive(1), {studyUid, {}, path}, {1, 1, 1}, true, {})(
                {}, [](DiagnosticEvent) {});
            QVERIFY(result.error);
            QCOMPARE(result.error->code, ErrorCode::InvalidInput);
            QVERIFY(result.error->message.contains("outside Git checkouts"));
            QVERIFY(!result.protocol);
        }

        for (const auto mode : {Peer::GetUnsupported, Peer::GetNoStorage, Peer::GetWrongRole}) {
            Peer peer(mode);
            QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
            logging::Logger logger;
            OperationRunner runner(logger);
            QSignalSpy finished(&runner, &OperationRunner::finished);
            QVERIFY(!runner.start("C-GET", getOperation(source(), archive(peer.serverPort()),
                {studyUid, {}, output.path()}, {2, 2, 2}, true, {})).isNull());
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
            const auto result = finished.first().at(1).value<OperationResult>();
            QVERIFY(result.error);
            QVERIFY(result.protocol);
            if (mode == Peer::GetWrongRole) {
                QCOMPARE(result.error->code, ErrorCode::StorageRoleRejected);
                QVERIFY(result.error->message.contains("requestor SCP role"));
            } else {
                QCOMPARE(result.error->code, ErrorCode::PresentationContextRejected);
                QVERIFY(result.error->message.contains(mode == Peer::GetUnsupported ? "did not accept Study Root C-GET"
                                                                                  : "rejected all proposed Storage"));
            }
            QVERIFY(result.protocol->messages.isEmpty());
            QCOMPARE(peer.requests.load(), 0);
        }
        QVERIFY(readInstances(QDir(output.path())).isEmpty());
        QVERIFY(QDir(output.path()).entryList({".dingobench-receive-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
    }

    void getPartialTimeoutAndCancellation()
    {
        for (const auto mode : {Peer::GetPartial, Peer::GetTimeout, Peer::GetCancel}) {
            Peer peer(mode);
            QVERIFY(peer.listen(QHostAddress::LocalHost, 0));
            QTemporaryDir output(QDir(QCoreApplication::applicationDirPath()).filePath("get-scripted-XXXXXX"));
            QVERIFY(output.isValid());
            logging::Logger logger;
            GetProgress latest;
            OperationRunner runner(logger);
            QSignalSpy finished(&runner, &OperationRunner::finished);
            const auto id = runner.start("C-GET", getOperation(source(), archive(peer.serverPort()),
                {studyUid, {}, output.path()}, {2, 2, mode == Peer::GetTimeout ? 1 : 4}, true,
                [&](GetProgress progress) { latest = progress; }));
            QTRY_COMPARE(peer.requests.load(), 1);
            if (mode == Peer::GetCancel)
                QVERIFY(runner.cancel(id));
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 6000);
            const auto result = finished.first().at(1).value<OperationResult>();
            QVERIFY(result.protocol);
            if (mode == Peer::GetPartial) {
                QCOMPARE(result.status, Status::Warning);
                QCOMPARE(result.dicomStatus.value_or(0), 0xb000);
                QCOMPARE(latest.completed.value_or(0), 1);
                QCOMPARE(latest.failed.value_or(0), 1);
                QCOMPARE(latest.filesSaved, 1);
                QCOMPARE(latest.storesFailed, 1);
                const QSet<QString> expected{instanceUid(1)};
                QCOMPARE(readInstances(QDir(output.path())), expected);
                QVERIFY(result.protocol->messages.last().dataset);
            } else if (mode == Peer::GetCancel) {
                QVERIFY(peer.gotCancel.load());
                QCOMPARE(result.status, Status::Cancelled);
                QCOMPARE(result.dicomStatus.value_or(0), 0xfe00);
                QCOMPARE(latest.remaining.value_or(0), 2);
            } else {
                QVERIFY(result.error);
                QCOMPARE(result.error->code, ErrorCode::DimseTimeout);
                QCOMPARE(latest.status.value_or(0), 0xff00);
                QVERIFY(!result.dicomStatus);
            }
            QVERIFY(QDir(output.path()).entryList({".dingobench-receive-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
        }
    }

    void getNativeArchiveAndWorkspace()
    {
        const auto serverExe = qEnvironmentVariable("DINGOBENCH_DCMQRSCP");
        const auto storescu = qEnvironmentVariable("DINGOBENCH_STORESCU");
        if (serverExe.isEmpty() || storescu.isEmpty())
            QSKIP("Set DINGOBENCH_DCMQRSCP and DINGOBENCH_STORESCU to local DCMTK executables.");
        ArchiveFixture peer;
        const auto started = peer.start(serverExe, storescu, 11119);
        QVERIFY2(started, qPrintable(peer.error));
        const auto output = peer.files.filePath("received");
        const auto profilePath = peer.files.filePath("profiles.json");
        profiles::Store profiles(profilePath);
        auto pacs = archive(peer.port);
        pacs.id = 0;
        QVERIFY(profiles.save(profiles::Kind::Destination, pacs).isEmpty());
        auto scu = source();
        scu.id = 0;
        QVERIFY(profiles.save(profiles::Kind::Source, scu).isEmpty());

        auto rejected = pacs;
        rejected.ae = "WRONG_AE";
        const auto denied = getOperation(scu, rejected, {studyUid, {}, output}, {2, 2, 2}, true, {})(
            {}, [](DiagnosticEvent) {});
        QVERIFY(denied.error);
        QCOMPARE(denied.error->code, ErrorCode::AssociationRejected);

        logging::Logger logger;
        MainWindow window(logger, false, profilePath);
        window.show();
        const auto button = [&](const char *name) { return window.findChild<QPushButton *>(QLatin1String(name)); };
        const auto label = [&](const char *name) { return window.findChild<QLabel *>(QLatin1String(name)); };
        button("nav4")->click();
        button("sendFind")->click();
        QTRY_VERIFY_WITH_TIMEOUT(label("querySummary")->text().contains("Query complete"), 5000);
        auto *results = window.findChild<QTableWidget *>("queryResults");
        QCOMPARE(results->rowCount(), 1);
        results->selectRow(0);
        window.findChild<QTabWidget *>("retrieveMethodTabs")->setCurrentIndex(1);
        window.findChild<QLineEdit *>("getDirectory")->setText(output);
        QVERIFY(button("sendGet")->isEnabled());
        QVERIFY(label("receiveStatus")->text().contains("Listener stopped"));
        button("sendGet")->click();
        QTRY_VERIFY_WITH_TIMEOUT(label("getSummary")->text().contains("ms · 0x0000") ||
                                 label("getSummary")->property("tone") == "error", 8000);
        QVERIFY2(label("getSummary")->text().contains("ms · 0x0000"), qPrintable(label("getSummary")->text()));
        QVERIFY(label("getCounts")->text().contains("Completed 2"));
        QVERIFY(label("getReceipts")->text().contains("2 files saved"));
        QVERIFY(label("receiveStatus")->text().contains("Listener stopped"));
        QVERIFY(button("getProtocolDetails")->isEnabled());
        const QSet<QString> both{instanceUid(1), instanceUid(2)};
        QCOMPARE(readInstances(QDir(output)), both);
        QVERIFY(window.close());

        GetProgress series;
        const auto result = getOperation(scu, pacs, {studyUid, seriesUid(2), output}, {2, 2, 4}, true,
            [&](GetProgress update) { series = update; })({}, [](DiagnosticEvent) {});
        QVERIFY2(result.status == Status::Succeeded, qPrintable(result.error ? result.error->message : result.details));
        QCOMPARE(series.completed.value_or(0), 1);
        QCOMPARE(series.filesSaved, 1);
        QCOMPARE(QDir(output).entryList({"*.dcm"}, QDir::Files).size(), 3);
        QCOMPARE(readInstances(QDir(output)), both);
        QVERIFY(result.protocol);
        bool storageRoleAccepted = false, receivedStore = false;
        for (const auto &context : result.protocol->association.contexts)
            storageRoleAccepted |= context.requestStorageScp && context.acceptedRole == "SCP (requestor)";
        for (const auto &message : result.protocol->messages)
            receivedStore |= message.direction == Direction::Incoming && message.command == "C-STORE-RQ";
        QVERIFY(storageRoleAccepted);
        QVERIFY(receivedStore);
        QVERIFY(QDir(output).entryList({".dingobench-receive-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
        peer.process.kill();
        QVERIFY(peer.process.waitForFinished(3000));
        QCOMPARE(peer.process.state(), QProcess::NotRunning);
    }

};

QTEST_MAIN(MoveRetrieveTest)
#include "MoveRetrieveTest.moc"
