// SPDX-License-Identifier: GPL-3.0-only
#include "DiagnosticReportAssertions.h"
#include "dingobench/dicom/StorageReceive.h"
#include "ClientAssociation.h"

#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcuid.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#endif

#include <QDir>
#include <QFile>
#include <QNetworkInterface>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

using namespace dingobench;
using namespace dingobench::dicom;
using State = StorageReceiver::State;

namespace {

constexpr auto instanceUid = "1.2.826.0.1.3680043.10.999.12.1";

QString testPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("receive-synthetic-XXXXXX");
}

profiles::Profile profile(int port, QString address = "127.0.0.1")
{
    return {1, "Loopback receiver", "TEST_SCP", std::move(address), port, {}};
}

int freePort()
{
    QTcpServer reserve;
    return reserve.listen(QHostAddress::LocalHost) ? reserve.serverPort() : 0;
}

bool canRebind(int port)
{
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, quint16(port));
}

bool processGone(qint64 id)
{
#ifdef _WIN32
    const auto handle = OpenProcess(SYNCHRONIZE, FALSE, DWORD(id));
    if (!handle)
        return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool exited = WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
    CloseHandle(handle);
    return exited;
#else
    return ::kill(pid_t(id), 0) == -1 && errno == ESRCH;
#endif
}

bool terminateFixture(qint64 id)
{
#ifdef _WIN32
    const auto handle = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(id));
    if (!handle)
        return false;
    const bool result = TerminateProcess(handle, 91);
    CloseHandle(handle);
    return result;
#else
    return ::kill(pid_t(id), SIGKILL) == 0;
#endif
}

bool writeObject(const QString &fileName)
{
    DcmFileFormat file;
    auto &dataset = *file.getDataset();
    const Uint16 pixels[]{1, 2, 3, 4};
    return dataset.putAndInsertString(DCM_SOPClassUID, UID_CTImageStorage).good() &&
        dataset.putAndInsertString(DCM_SOPInstanceUID, instanceUid).good() &&
        dataset.putAndInsertString(DCM_PatientName, "Synthetic^Receiver").good() &&
        dataset.putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2").good() &&
        dataset.putAndInsertUint16(DCM_Rows, 2).good() && dataset.putAndInsertUint16(DCM_Columns, 2).good() &&
        dataset.putAndInsertUint16(DCM_SamplesPerPixel, 1).good() &&
        dataset.putAndInsertUint16(DCM_BitsAllocated, 16).good() &&
        dataset.putAndInsertUint16(DCM_BitsStored, 16).good() &&
        dataset.putAndInsertUint16(DCM_HighBit, 15).good() &&
        dataset.putAndInsertUint16(DCM_PixelRepresentation, 0).good() &&
        dataset.putAndInsertUint16Array(DCM_PixelData, pixels, 4).good() &&
        file.saveFile(QFile::encodeName(fileName).constData(), EXS_LittleEndianImplicit).good();
}

int sendWithStorescu(const QString &executable, const QString &file, int port, const QString &called)
{
    QProcess process;
    process.start(executable, {"-aet", "TEST_SCU", "-aec", called, "-xi",
        "-to", "2", "-ta", "3", "-td", "4", "127.0.0.1", QString::number(port), file});
    if (!process.waitForStarted(2000) || !process.waitForFinished(8000)) {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(1000);
        }
        return -1;
    }
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

}  // namespace

class StorageReceiveTest final : public QObject {
    Q_OBJECT

private slots:
    void repeatedLifecycleAndPortRelease()
    {
        QTemporaryDir dir(testPath());
        QVERIFY(dir.isValid());
        const int port = freePort();
        QVERIFY(port);
        QString loopbackName;
        for (const auto &adapter : QNetworkInterface::allInterfaces()) {
            if (std::ranges::any_of(adapter.addressEntries(), [](const auto &entry) {
                return entry.ip() == QHostAddress::LocalHost;
            })) {
                loopbackName = adapter.name();
                break;
            }
        }
        logging::Logger logger;
        StorageReceiver receiver(logger);
        for (int cycle = 0; cycle < 3; ++cycle) {
            const auto address = cycle == 0 ? QString() :
                cycle == 2 && !loopbackName.isEmpty() ? loopbackName : QString("127.0.0.1");
            receiver.start(profile(port, address), dir.path(), true);
            QTRY_COMPARE(receiver.state(), State::Listening);
            QVERIFY(receiver.message().contains("127.0.0.1:"));
            const auto pid = receiver.processId();
            QVERIFY(pid > 0);
            QVERIFY(!canRebind(port));
            receiver.stop();
            QTRY_COMPARE(receiver.state(), State::Stopped);
            QVERIFY(!receiver.active());
            QVERIFY(processGone(pid));
            QVERIFY(canRebind(port));
            QVERIFY(QDir(dir.path()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        }
    }

    void startupFailures()
    {
        QTemporaryDir dir(testPath());
        QVERIFY(dir.isValid());
        QTcpServer held;
        QVERIFY(held.listen(QHostAddress::LocalHost));
        logging::Logger logger;
        StorageReceiver receiver(logger);
        receiver.start(profile(held.serverPort()), dir.path(), true);
        QTRY_COMPARE(receiver.state(), State::Error);
        QVERIFY(receiver.message().contains("Cannot bind"));
        QVERIFY(!receiver.active());
        held.close();

        receiver.start(profile(freePort()), dir.filePath("missing"), true);
        QCOMPARE(receiver.state(), State::Error);
        QVERIFY(receiver.message().contains("existing output directory"));
        QVERIFY(QDir(dir.path()).mkdir("GitHub"));
        receiver.start(profile(freePort()), dir.filePath("GitHub"), true);
        QCOMPARE(receiver.state(), State::Error);
        QVERIFY(!receiver.active());
        receiver.start(profile(freePort(), "no-such-interface"), dir.path(), true);
        QTRY_COMPARE(receiver.state(), State::Error);
        QVERIFY(receiver.message().contains("interface"));
        QVERIFY(!receiver.active());
    }

    void storescuReceivesAndDuplicates()
    {
        const auto storescu = qEnvironmentVariable("DINGOBENCH_STORESCU");
        if (storescu.isEmpty())
            QSKIP("Set DINGOBENCH_STORESCU to the local DCMTK storescu executable.");
        QVERIFY(QFileInfo(storescu).isExecutable());
        QTemporaryDir dir(testPath());
        QVERIFY(dir.isValid());
        const auto input = dir.filePath("synthetic.dcm");
        QVERIFY(writeObject(input));
        QVERIFY(QDir(dir.path()).mkdir("réception-实例"));
        const auto output = dir.filePath("réception-实例");
        QFile existing(QDir(output).filePath(QString(instanceUid) + ".dcm"));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("existing file"), 13);
        existing.close();
        const int port = freePort();
        QVERIFY(port);
        logging::Logger logger;
        StorageReceiver receiver(logger);
        QList<ReceiveEvent> events;
        QSignalSpy sessions(&receiver, &StorageReceiver::sessionFinished);
        connect(&receiver, &StorageReceiver::eventReceived, this, [&](ReceiveEvent event) {
            events.append(std::move(event));
        });
        receiver.start(profile(port), output, true);
        QTRY_COMPARE(receiver.state(), State::Listening);
        const auto pid = receiver.processId();
        QVERIFY(sendWithStorescu(storescu, input, port, "WRONG_AE") > 0);
        QTRY_VERIFY(std::ranges::any_of(events, [](const auto &event) {
            return event.action == "Rejected" && event.calledAe == "WRONG_AE";
        }));
        QCOMPARE(QDir(output).entryList(QDir::Files).size(), 1);
        QCOMPARE(sendWithStorescu(storescu, input, port, "TEST_SCP"), 0);
        QCOMPARE(sendWithStorescu(storescu, input, port, "TEST_SCP"), 0);
        QTRY_COMPARE(std::ranges::count_if(events, [](const auto &e) { return e.action == "Stored"; }), 2);
        QTRY_COMPARE(sessions.size(), 3);
        const auto rejected = sessions.first().first().value<DiagnosticSession>();
        QCOMPARE(rejected.result, QString("Failed"));
        verifyDiagnosticReport(rejected, "storage-scp-rejected");
        const auto received = sessions.last().first().value<DiagnosticSession>();
        QCOMPARE(received.result, QString("Succeeded"));
        QVERIFY(received.inbound);
        QVERIFY(!received.association.contexts.isEmpty());
        QCOMPARE(received.messages.first().command, QString("C-STORE-RQ"));
        QCOMPARE(received.messages.last().status.value_or(1), quint16(0));
        QCOMPARE(received.requestIndex(1).value_or(-1), 0);
        verifyDiagnosticReport(received, "storage-scp-success");
        receiver.stop();
        QTRY_COMPARE(receiver.state(), State::Stopped);
        QVERIFY(processGone(pid));
        QVERIFY(canRebind(port));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("existing file"));
        existing.close();

        QSet<QString> names;
        for (const auto &event : events) {
            if (event.action != "Stored")
                continue;
            QCOMPARE(event.sopInstance, QString(instanceUid));
            QCOMPARE(event.sopClass, QString(UID_CTImageStorage));
            QCOMPARE(event.status.value_or(1), quint16(0));
            QCOMPARE(event.bytes, QFileInfo(event.path).size());
            QVERIFY(event.bytes > 0);
            names.insert(event.path);
            std::optional<DatasetSnapshot> inspected;
            const auto operation = inspectReceivedFile(event.path, [&](DatasetSnapshot data) { inspected = std::move(data); });
            QCOMPARE(operation({}, [](DiagnosticEvent) {}).status, OperationResult::Status::Succeeded);
            QVERIFY(inspected);
            const auto item = std::ranges::find(inspected->attributes, QString("(0008,0018)"), &Attribute::tag);
            QVERIFY(item != inspected->attributes.end());
            QCOMPARE(item->value, QString(instanceUid));
        }
        QCOMPARE(names.size(), 2);
        QCOMPARE(QDir(output).entryList(QDir::Files).size(), 3);
        const auto logged = logger.snapshot();
        QVERIFY(std::ranges::any_of(logged, [](const auto &entry) {
            return entry.component == "Storage Receive" && entry.message.contains("object saved") &&
                entry.details.contains(instanceUid) && !entry.synthetic;
        }));
        receiver.start(profile(port), output, false);
        QTRY_COMPARE(receiver.state(), State::Listening);
        QCOMPARE(sendWithStorescu(storescu, input, port, "WRONG_AE"), 0);
        receiver.stop();
        QTRY_COMPARE(receiver.state(), State::Stopped);
        QCOMPARE(QDir(output).entryList(QDir::Files).size(), 4);
    }

    void stopDuringActiveAssociationAndDetectCrash()
    {
        QTemporaryDir dir(testPath());
        QVERIFY(dir.isValid());
        const int port = freePort();
        QVERIFY(port);
        logging::Logger logger;
        StorageReceiver receiver(logger);
        bool accepted = false;
        connect(&receiver, &StorageReceiver::eventReceived, this, [&](const ReceiveEvent &event) {
            accepted |= event.action == "Accepted";
        });
        receiver.start(profile(port), dir.path(), true);
        QTRY_COMPARE(receiver.state(), State::Listening);
        const auto pid = receiver.processId();
        OperationRunner peer(logger);
        QSignalSpy finished(&peer, &OperationRunner::finished);
        const auto id = peer.start("Idle test association", [port](std::stop_token stop, const ReportDiagnostic &report) {
            DiagnosticSession session;
            session.association.contexts.append({1, UID_CTImageStorage, {}, {UID_LittleEndianImplicitTransferSyntax}});
            const profiles::Profile source{1, "Synthetic SCU", "TEST_SCU", {}, 11112, {}};
            const profiles::Profile destination{2, "Synthetic SCP", "TEST_SCP", "127.0.0.1", port, {}};
            return withClientAssociation(source, destination, {1, 2, 2}, stop, report, session, "Fixture",
                [stop](T_ASC_Association &, ClientPhase &phase) {
                    while (!stop.stop_requested())
                        QThread::msleep(10);
                    phase.release = false;
                    return OperationResult::cancelled();
                });
        });
        QTRY_VERIFY(accepted);
        receiver.stop();
        QTRY_COMPARE_WITH_TIMEOUT(receiver.state(), State::Stopped, 3000);
        QCOMPARE(receiver.message(), QString("Listener stopped."));
        QVERIFY(processGone(pid));
        QVERIFY(canRebind(port));
        QVERIFY(peer.cancel(id));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());

        receiver.start(profile(port), dir.path(), true);
        QTRY_COMPARE(receiver.state(), State::Listening);
        const auto crashed = receiver.processId();
        QVERIFY(crashed > 0 && terminateFixture(crashed));
        QTRY_COMPARE(receiver.state(), State::Error);
        QVERIFY(receiver.message().contains("exited unexpectedly"));
        QVERIFY(processGone(crashed));
        QVERIFY(canRebind(port));
        QVERIFY(QDir(dir.path()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
};

QTEST_GUILESS_MAIN(StorageReceiveTest)
#include "StorageReceiveTest.moc"
