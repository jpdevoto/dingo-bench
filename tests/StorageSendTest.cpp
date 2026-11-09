// SPDX-License-Identifier: GPL-3.0-only
#include "DiagnosticReportAssertions.h"
#include "dingobench/dicom/StorageSend.h"

#include <dcmtk/config/osconfig.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/assoc.h>
#include <dcmtk/dcmnet/dimse.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <memory>
#include <vector>

using namespace dingobench;
using namespace dingobench::dicom;
using Status = OperationResult::Status;

namespace {

profiles::Profile source()
{
    return {1, "Storage SCU fixture", "TEST_SCU", {}, 11112, {}};
}

profiles::Profile destination(int port)
{
    return {2, "Loopback SCP", "TEST_SCP", "127.0.0.1", port, {}};
}

QString instance(int index)
{
    return QString("1.2.826.0.1.3680043.10.999.11.%1").arg(index);
}

QString testDirectoryTemplate()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("storage-synthetic-XXXXXX");
}

bool generate(const QString &path, int index, E_TransferSyntax syntax = EXS_LittleEndianImplicit, bool large = false)
{
    DcmFileFormat format;
    auto &d = *format.getDataset();
    const auto uid = instance(index).toLatin1();
    const Uint16 rows = large ? 2048 : 2;
    const Uint16 columns = large ? 4096 : 2;
    const std::vector<Uint16> pixels(size_t(rows) * columns, 1);
    if (d.putAndInsertString(DCM_SOPClassUID, UID_CTImageStorage).bad() ||
        d.putAndInsertString(DCM_SOPInstanceUID, uid.constData()).bad() ||
        d.putAndInsertString(DCM_PatientName, "Synthetic^Storage").bad() ||
        d.putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2").bad() ||
        d.putAndInsertUint16(DCM_Rows, rows).bad() || d.putAndInsertUint16(DCM_Columns, columns).bad() ||
        d.putAndInsertUint16(DCM_SamplesPerPixel, 1).bad() || d.putAndInsertUint16(DCM_BitsAllocated, 16).bad() ||
        d.putAndInsertUint16(DCM_BitsStored, 16).bad() || d.putAndInsertUint16(DCM_HighBit, 15).bad() ||
        d.putAndInsertUint16(DCM_PixelRepresentation, 0).bad() ||
        d.putAndInsertUint16Array(DCM_PixelData, pixels.data(), pixels.size()).bad())
        return false;
    const auto name = QFile::encodeName(path);
    return format.saveFile(OFFilename(name.data()), syntax).good();
}

QByteArray digest(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray();
}

QSet<QString> receivedInstances(const QDir &directory)
{
    QSet<QString> instances;
    for (const auto &path : directory.entryList(QDir::Files)) {
        DcmFileFormat received;
        if (received.loadFile(QFile::encodeName(directory.filePath(path)).constData()).good()) {
            OFString uid;
            if (received.getDataset()->findAndGetOFString(DCM_SOPInstanceUID, uid).good())
                instances.insert(QString::fromLatin1(uid.c_str()));
        }
    }
    return instances;
}

// Bind a loopback-only fault peer in Qt, then pass the accepted socket to
// DCMTK so each test still exchanges actual association and DIMSE messages.
class FaultPeer final : public QTcpServer {
public:
    enum Mode { Disconnect, Stall, Refuse, Warn, StallDataset };

    explicit FaultPeer(Mode mode) : m_mode(mode) { m_worker.setMaxThreadCount(1); }
    ~FaultPeer() override
    {
        close();
        m_worker.waitForDone();
    }

    std::atomic<bool> received = false;
    std::atomic<bool> closed = false;

protected:
    void incomingConnection(qintptr socket) override
    {
        m_worker.start([this, socket] { serve(static_cast<DcmNativeSocketType>(socket)); });
    }

private:
    void serve(DcmNativeSocketType socket)
    {
        if (m_mode == StallDataset) {
            const int receiveBuffer = 8192;
            setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&receiveBuffer),
                       sizeof(receiveBuffer));
        }
        dcmExternalSocketHandle.set(socket);
        T_ASC_Network *network = nullptr;
        auto condition = ASC_initializeNetwork(NET_ACCEPTOR, 0, 2, &network);
        const auto drop = [](T_ASC_Network *value) { ASC_dropNetwork(&value); };
        const std::unique_ptr<T_ASC_Network, decltype(drop)> ownedNetwork(network, drop);
        T_ASC_Association *association = nullptr;
        if (condition.good())
            condition = ASC_receiveAssociation(network, &association, ASC_DEFAULTMAXPDU,
                                               nullptr, nullptr, OFFalse, DUL_NOBLOCK, 2);
        dcmExternalSocketHandle.set(DCMNET_INVALID_SOCKET);
        const auto destroy = [](T_ASC_Association *value) { ASC_destroyAssociation(&value); };
        const std::unique_ptr<T_ASC_Association, decltype(destroy)> ownedAssociation(association, destroy);
        if (condition.bad())
            return;
        T_ASC_PresentationContext proposed{};
        if (ASC_getPresentationContext(association->params, 0, &proposed).bad() ||
            ASC_acceptPresentationContext(association->params, proposed.presentationContextID,
                                          UID_LittleEndianImplicitTransferSyntax).bad() ||
            ASC_acknowledgeAssociation(association).bad())
            return;

        T_ASC_PresentationContextID context = 0;
        T_DIMSE_Message message{};
        if (DIMSE_receiveCommand(association, DIMSE_NONBLOCKING, 2, &context, &message, nullptr).bad() ||
            message.CommandField != DIMSE_C_STORE_RQ)
            return;
        if (m_mode == StallDataset) {
            received = true;
            QThread::msleep(1600);
            return;
        }
        DcmDataset *dataset = nullptr;
        condition = DIMSE_receiveDataSetInMemory(association, DIMSE_NONBLOCKING, 2, &context,
                                                 &dataset, nullptr, nullptr);
        const std::unique_ptr<DcmDataset> ownedDataset(dataset);
        if (condition.bad())
            return;
        received = true;
        if (m_mode == Disconnect) {
            ASC_dropAssociation(association);
            closed = true;
            return;
        }
        if (m_mode == Warn || m_mode == Refuse) {
            T_DIMSE_C_StoreRSP response{};
            response.DimseStatus = m_mode == Warn ? 0xb000 : 0xa700;
            if (DIMSE_sendStoreResponse(association, context, &message.msg.CStoreRQ, &response, nullptr).bad())
                return;
        }
        condition = DIMSE_receiveCommand(association, DIMSE_NONBLOCKING, 3, &context, &message, nullptr);
        if (condition == DUL_PEERREQUESTEDRELEASE)
            ASC_acknowledgeRelease(association);
        closed = true;
    }

    Mode m_mode;
    QThreadPool m_worker;
};

}  // namespace

class StorageSendTest final : public QObject {
    Q_OBJECT

private slots:
    void validationRejectsMalformedInput()
    {
        QTemporaryDir dir(testDirectoryTemplate());
        QVERIFY(dir.isValid());
        const auto valid = dir.filePath("valid.dcm");
        const auto malformed = dir.filePath("malformed.dcm");
        const auto truncated = dir.filePath("truncated.dcm");
        QVERIFY(generate(valid, 1));
        QVERIFY(generate(truncated, 2, EXS_LittleEndianImplicit, true));
        QVERIFY(QFile(truncated).resize(QFileInfo(truncated).size() - 100));
        QFile file(malformed);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("invalid DICOM"), 13);
        file.close();

        QList<StorageFile> files;
        logging::Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        runner.start("Validate", inspectStorageFiles({valid, malformed, truncated}, dir.path(), {},
            [&](StorageFile entry) { files.append(std::move(entry)); }));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(files.size(), 3);
        QCOMPARE(files.first().state, StorageFile::Ready);
        QCOMPARE(files.first().sopInstance, instance(1));
        QCOMPARE(files.first().transferSyntax, QString(UID_LittleEndianImplicitTransferSyntax));
        QCOMPARE(files.first().size, QFileInfo(valid).size());
        QCOMPARE(files.at(1).state, StorageFile::Invalid);
        QCOMPARE(files.at(2).state, StorageFile::Invalid);
        finished.clear();
        QVERIFY(generate(valid, 5));
        runner.start("C-STORE", sendStorageFiles(source(), destination(1), {valid}, {1, 1, 1}, false,
            [&](StorageFile entry) { files.append(std::move(entry)); }, {files.first()}));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(files.last().state, StorageFile::Invalid);
        QVERIFY(files.last().message.contains("changed after queue validation"));
        QVERIFY(finished.first().at(1).value<OperationResult>().protocol->messages.isEmpty());
    }

    void storescpSingleBatchAndUnsupportedSyntax()
    {
        const auto executable = qEnvironmentVariable("DINGOBENCH_STORESCP");
        if (executable.isEmpty())
            QSKIP("Set DINGOBENCH_STORESCP to a local DCMTK storescp executable.");
        QVERIFY(QFileInfo(executable).isExecutable());
        QTemporaryDir dir(testDirectoryTemplate());
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("received"));
        const QDir received(dir.filePath("received"));
        QTcpServer reserve;
        QVERIFY(reserve.listen(QHostAddress::LocalHost));
        const auto port = reserve.serverPort();
        reserve.close();
        struct Server {
            QProcess process;
            ~Server()
            {
                if (process.state() != QProcess::NotRunning) {
                    process.terminate();
                    if (!process.waitForFinished(250)) {
                        process.kill();
                        process.waitForFinished(1000);
                    }
                }
            }
        } server;
        server.process.setWorkingDirectory(dir.path());
        server.process.start(executable, {"+xi", "+B", "-aet", "TEST_SCP", "-od", received.absolutePath(),
                                          QString::number(port)});
        QVERIFY(server.process.waitForStarted(2000));
        QTest::qWait(350);
        QVERIFY2(server.process.state() == QProcess::Running, server.process.readAllStandardError().constData());
        const auto first = dir.filePath("instance-实例.dcm");
        const auto second = dir.filePath("second.dcm");
        const auto third = dir.filePath("third.dcm");
        const auto unsupported = dir.filePath("unsupported.dcm");
        const auto firstFixture = dir.filePath("first.dcm");
        QVERIFY(generate(firstFixture, 1));
        QVERIFY(QFile::rename(firstFixture, first));
        QVERIFY(generate(second, 2));
        QVERIFY(generate(third, 3));
        QVERIFY(generate(unsupported, 4, EXS_BigEndianExplicit));
        const QStringList paths{first, second, third, unsupported};
        QList<QByteArray> hashes;
        for (const auto &path : paths) {
            hashes.append(digest(path));
            QVERIFY(!hashes.last().isEmpty());
        }

        QHash<QString, StorageFile> results;
        logging::Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        const auto update = [&](StorageFile file) {
            const auto path = file.path;
            results.insert(path, std::move(file));
        };
        runner.start("C-STORE", sendStorageFiles(source(), destination(port), {first}, {2, 2, 2}, false, update));
        QTRY_COMPARE(finished.size(), 1);
        auto operation = finished.takeFirst().at(1).value<OperationResult>();
        QVERIFY2(operation.status == Status::Succeeded,
                 qPrintable(operation.error ? operation.error->message : operation.details));
        QCOMPARE(results.value(first).state, StorageFile::Stored);
        QCOMPARE(results.value(first).status.value_or(1), quint16(0));
        QCOMPARE(receivedInstances(received), QSet<QString>{instance(1)});
        QVERIFY(operation.protocol);
        QCOMPARE(operation.protocol->messages.size(), 2);
        QCOMPARE(operation.protocol->messages.first().command, QString("C-STORE-RQ"));
        QVERIFY(operation.protocol->messages.first().datasetPresent == true);
        QVERIFY(!operation.protocol->messages.first().dataset);
        QCOMPARE(operation.protocol->requestIndex(1).value_or(-1), 0);
        QCOMPARE(operation.protocol->counts.value("Stored"), 1);
        verifyDiagnosticReport(*operation.protocol, "store-success");

        runner.start("C-STORE", sendStorageFiles(source(), destination(port), {second, unsupported, third},
                                                {2, 2, 2}, true, update));
        QTRY_COMPARE(finished.size(), 1);
        operation = finished.takeFirst().at(1).value<OperationResult>();
        QCOMPARE(operation.status, Status::Failed);
        QCOMPARE(results.value(second).state, StorageFile::Stored);
        QCOMPARE(results.value(third).state, StorageFile::Stored);
        QCOMPARE(results.value(unsupported).state, StorageFile::Rejected);
        QVERIFY(!results.value(unsupported).status);
        const QSet<QString> expected{instance(1), instance(2), instance(3)};
        QCOMPARE(receivedInstances(received), expected);
        QVERIFY(operation.protocol->messages.first().dataset);
        QVERIFY(operation.details.contains("2 stored"));
        QCOMPARE(operation.protocol->counts.value("Failed"), 1);
        QCOMPARE(operation.protocol->counts.value("Stored"), 2);
        verifyDiagnosticReport(*operation.protocol, "store-partial-failure");
        for (int index = 0; index < paths.size(); ++index)
            QCOMPARE(digest(paths.at(index)), hashes.at(index));
    }

    void faultCases_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("cancel");
        QTest::newRow("disconnect") << int(FaultPeer::Disconnect) << false;
        QTest::newRow("DIMSE timeout") << int(FaultPeer::Stall) << false;
        QTest::newRow("cancel while awaiting response") << int(FaultPeer::Stall) << true;
        QTest::newRow("SCP failure status") << int(FaultPeer::Refuse) << false;
        QTest::newRow("SCP warning status") << int(FaultPeer::Warn) << false;
    }

    void faultCases()
    {
        QFETCH(int, mode);
        QFETCH(bool, cancel);
        QTemporaryDir dir(testDirectoryTemplate());
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("fault-fixture.dcm");
        QVERIFY(generate(path, 8));
        const auto original = digest(path);
        FaultPeer peer{FaultPeer::Mode(mode)};
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        StorageFile file;
        logging::Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        const auto id = runner.start("C-STORE", sendStorageFiles(source(), destination(peer.serverPort()), {path},
            {1, 1, cancel ? 10 : 1}, false, [&](StorageFile update) { file = std::move(update); }));
        QTRY_VERIFY(peer.received.load());
        if (cancel)
            QVERIFY(runner.cancel(id));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2500);
        const auto result = finished.takeFirst().at(1).value<OperationResult>();
        if (mode == FaultPeer::Warn || mode == FaultPeer::Refuse) {
            QCOMPARE(file.status.value_or(0), quint16(mode == FaultPeer::Warn ? 0xb000 : 0xa700));
            QCOMPARE(file.state, mode == FaultPeer::Warn ? StorageFile::Warning : StorageFile::Failed);
            QCOMPARE(result.status, mode == FaultPeer::Warn ? Status::Succeeded : Status::Failed);
            QVERIFY(result.protocol);
            QCOMPARE(result.protocol->status, file.status);
            verifyDiagnosticReport(*result.protocol, mode == FaultPeer::Warn ? "store-warning" : "store-dimse-failure");
        } else {
            QCOMPARE(file.state, StorageFile::Unconfirmed);
            QVERIFY(!file.status);
            QCOMPARE(result.status, cancel ? Status::Cancelled : Status::Failed);
            if (!cancel) {
                QVERIFY(result.error);
                QCOMPARE(result.error->code, mode == FaultPeer::Stall ? ErrorCode::DimseTimeout : ErrorCode::ProtocolFailure);
            }
        }
        QTRY_VERIFY(peer.closed.load());
        QCOMPARE(digest(path), original);
    }

    void stalledPayloadHonorsWriteDeadline()
    {
        QTemporaryDir dir(testDirectoryTemplate());
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("large-fixture.dcm");
        QVERIFY(generate(path, 9, EXS_LittleEndianImplicit, true));
        const auto original = digest(path);
        FaultPeer peer(FaultPeer::StallDataset);
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        StorageFile file;
        logging::Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        runner.start("C-STORE", sendStorageFiles(source(), destination(peer.serverPort()), {path},
            {1, 1, 1}, false, [&](StorageFile update) { file = std::move(update); }));
        QTRY_VERIFY(peer.received.load());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2500);
        const auto result = finished.first().at(1).value<OperationResult>();
        QVERIFY(result.error);
        QCOMPARE(result.error->code, ErrorCode::DimseTimeout);
        QCOMPARE(file.state, StorageFile::Unconfirmed);
        QVERIFY(!file.status);
        QCOMPARE(digest(path), original);
    }
};

QTEST_GUILESS_MAIN(StorageSendTest)
#include "StorageSendTest.moc"
