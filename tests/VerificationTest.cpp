// SPDX-License-Identifier: GPL-3.0-only
#include "DiagnosticReportAssertions.h"
#include "dingobench/dicom/Verification.h"
#include "ProtocolInspector.h"
#include "MainWindow.h"
#include "Ui.h"

#include <dcmtk/config/osconfig.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/assoc.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/ofstd/ofstd.h>

#include <QMutex>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QDir>
#include <QTabWidget>
#include <QTableWidget>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

#include <atomic>
#include <algorithm>
#include <memory>
#include <vector>

using namespace dingobench;
using namespace dingobench::dicom;
using Status = OperationResult::Status;

namespace {

profiles::Profile source(QString ae = "BENCH_A")
{
    return {1, "Test source", ae, {}, 11112, {}};
}

profiles::Profile destination(int port, QString host = "127.0.0.1", QString ae = "TEST_SCP")
{
    return {2, "Test peer", ae, host, port, {}};
}

// Bind in Qt so the test fixture listens exclusively on loopback. DCMTK
// receives the accepted socket and handles the complete DICOM protocol.
class LocalScp final : public QTcpServer {
public:
    enum Mode { Respond, RejectContext, RejectSyntax, IgnoreEcho, FailEcho };

    explicit LocalScp(Mode mode = Respond) : m_mode(mode) { m_workers.setMaxThreadCount(1); }
    ~LocalScp() override
    {
        close();
        m_workers.waitForDone();
    }

    QStringList callers()
    {
        const QMutexLocker lock(&m_mutex);
        return m_callers;
    }

    std::atomic<int> echoed{0};
    std::atomic<int> released{0};
    std::atomic<int> disconnected{0};
    std::atomic<int> rejected{0};

protected:
    void incomingConnection(qintptr socket) override
    {
        m_workers.start([this, socket] { serve(static_cast<DcmNativeSocketType>(socket)); });
    }

private:
    void serve(DcmNativeSocketType socket)
    {
        dcmExternalSocketHandle.set(socket);
        T_ASC_Network *network = nullptr;
        auto condition = ASC_initializeNetwork(NET_ACCEPTOR, 0, 2, &network);
        const auto dropNetwork = [](T_ASC_Network *value) { ASC_dropNetwork(&value); };
        const std::unique_ptr<T_ASC_Network, decltype(dropNetwork)> ownedNetwork(network, dropNetwork);
        T_ASC_Association *association = nullptr;
        if (condition.good())
            condition = ASC_receiveAssociation(network, &association, ASC_DEFAULTMAXPDU,
                                               nullptr, nullptr, OFFalse, DUL_NOBLOCK, 2);
        dcmExternalSocketHandle.set(DCMNET_INVALID_SOCKET);
        const auto destroyAssociation = [](T_ASC_Association *value) { ASC_destroyAssociation(&value); };
        const std::unique_ptr<T_ASC_Association, decltype(destroyAssociation)> ownedAssociation(
            association, destroyAssociation);
        if (condition.bad())
            return;

        char calling[DIC_AE_LEN + 1]{};
        char called[DIC_AE_LEN + 1]{};
        ASC_getAPTitles(association->params, calling, sizeof(calling), called, sizeof(called), nullptr, 0);
        {
            const QMutexLocker lock(&m_mutex);
            m_callers.append(QString::fromLatin1(calling));
        }
        if (qstrcmp(called, "TEST_SCP") != 0 && qstrcmp(called, "DINGO_SMOKE") != 0) {
            const T_ASC_RejectParameters rejection{ASC_RESULT_REJECTEDPERMANENT, ASC_SOURCE_SERVICEUSER,
                                                  ASC_REASON_SU_CALLEDAETITLENOTRECOGNIZED};
            if (ASC_rejectAssociation(association, &rejection).good())
                ++rejected;
            return;
        }

        T_ASC_PresentationContext context{};
        if (ASC_getPresentationContext(association->params, 0, &context).bad() ||
            qstrcmp(context.abstractSyntax, UID_VerificationSOPClass) != 0)
            return;
        condition = m_mode == RejectContext || m_mode == RejectSyntax
            ? ASC_refusePresentationContext(association->params, context.presentationContextID,
                m_mode == RejectSyntax ? ASC_P_TRANSFERSYNTAXESNOTSUPPORTED : ASC_P_ABSTRACTSYNTAXNOTSUPPORTED)
            : ASC_acceptPresentationContext(association->params, context.presentationContextID,
                                             UID_LittleEndianImplicitTransferSyntax);
        if (condition.bad() || ASC_acknowledgeAssociation(association).bad())
            return;

        T_ASC_PresentationContextID presentationId = 0;
        T_DIMSE_Message message{};
        condition = DIMSE_receiveCommand(association, DIMSE_NONBLOCKING, 3, &presentationId, &message, nullptr);
        if (condition.good() && message.CommandField == DIMSE_C_ECHO_RQ) {
            ++echoed;
            if (m_mode != IgnoreEcho) {
                const DIC_US status = m_mode == FailEcho ? 0x0122 : STATUS_Success;
                if (DIMSE_sendEchoResponse(association, presentationId, &message.msg.CEchoRQ,
                                           status, nullptr).bad())
                    return;
            }
            condition = DIMSE_receiveCommand(association, DIMSE_NONBLOCKING, 3,
                                             &presentationId, &message, nullptr);
        }
        if (condition == DUL_PEERREQUESTEDRELEASE && ASC_acknowledgeRelease(association).good())
            ++released;
        if (condition == DUL_NETWORKCLOSED || condition == DUL_PEERABORTEDASSOCIATION)
            ++disconnected;
    }

    Mode m_mode;
    QThreadPool m_workers;
    QMutex m_mutex;
    QStringList m_callers;
};

}  // namespace

class VerificationTest final : public QObject {
    Q_OBJECT

    static bool screenshot(const DiagnosticSession &session, int tab, const QString &name)
    {
        const auto directory = qEnvironmentVariable("DINGOBENCH_PROTOCOL_CAPTURE_DIR");
        if (directory.isEmpty())
            return true;
        ui::ProtocolInspector inspector(session, nullptr);
        inspector.resize(1100, 750);
        inspector.findChild<QTabWidget *>("protocolTabs")->setCurrentIndex(tab);
        inspector.show();
        QCoreApplication::processEvents();
        return inspector.grab().save(QDir(directory).filePath(name + ".png"));
    }

private slots:
    void initTestCase() { QVERIFY(ui::installTheme(*qApp)); }

    void applicationEcho()
    {
        const auto executable = qEnvironmentVariable("DINGOBENCH_TEST_APP");
        if (executable.isEmpty())
            QSKIP("Set DINGOBENCH_TEST_APP to exercise a standalone executable.");
        LocalScp peer;
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(executable, {"--smoke-test", "--smoke-echo-port", QString::number(peer.serverPort())});
        QVERIFY2(process.waitForStarted(3000), qPrintable(process.errorString()));
        QTRY_COMPARE_WITH_TIMEOUT(process.state(), QProcess::NotRunning, 12000);
        QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, process.readAll().constData());
        QCOMPARE(peer.echoed.load(), 1);
        QCOMPARE(peer.released.load(), 1);
    }

    void dashboardRecordsSuccessfulEcho()
    {
        LocalScp peer;
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("profiles.json");
        profiles::Store store(path);
        auto destinationProfile = destination(peer.serverPort());
        destinationProfile.id = 0;
        QVERIFY(store.save(profiles::Kind::Destination, destinationProfile).isEmpty());
        logging::Logger logger;
        MainWindow window(logger, false, path);
        window.show();
        auto *history = window.findChild<QTableWidget *>("recentOperations");
        QVERIFY(history);
        QCOMPARE(history->rowCount(), 0);
        window.findChild<QPushButton *>("nav1")->click();
        window.findChild<QPushButton *>("sendEcho")->click();
        QTRY_COMPARE(history->rowCount(), 1);
        QCOMPARE(history->item(0, 1)->text(), QString("C-ECHO"));
        QCOMPARE(history->item(0, 2)->text(), destinationProfile.name + " / " + destinationProfile.ae);
        QCOMPARE(history->item(0, 3)->text(), QString("Succeeded"));
        QVERIFY(history->item(0, 4)->text().endsWith(" ms"));
        QCOMPARE(peer.echoed.load(), 1);
    }

    void repeatedEchoAndCallingAe()
    {
        LocalScp peer;
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        logging::Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        for (int index = 0; index < 10; ++index) {
            const auto calling = index % 2 ? QString("BENCH_B") : QString("BENCH_A");
            const auto id = runner.start("C-ECHO", verificationOperation(source(calling),
                destination(peer.serverPort(), index == 0 ? "localhost" : "127.0.0.1")), false, "Verification");
            QVERIFY(!id.isNull());
            QTRY_COMPARE(finished.size(), 1);
            const auto arguments = finished.takeFirst();
            QCOMPARE(arguments.at(0).toUuid(), id);
            const auto result = arguments.at(1).value<OperationResult>();
            QVERIFY2(result.status == Status::Succeeded, qPrintable(result.details));
            QCOMPARE(result.dicomStatus.value_or(0xffff), quint16(0));
            QVERIFY(result.protocol);
            const auto &session = *result.protocol;
            QCOMPARE(session.id, id);
            QVERIFY(session.identities.source);
            QCOMPARE(session.identities.source->ae, calling);
            QCOMPARE(session.identities.destination->port, peer.serverPort());
            QCOMPARE(session.association.connection, QString("TCP established"));
            if (index == 0)
                verifyDiagnosticReport(session, "echo-success");
            QCOMPARE(session.association.callingAe, calling);
            QCOMPARE(session.association.calledAe, QString("TEST_SCP"));
            QCOMPARE(session.association.applicationContext, QString("1.2.840.10008.3.1.1.1"));
            QVERIFY(session.association.requestSent);
            QVERIFY(session.association.localEndpoint.startsWith("127.0.0.1:"));
            QCOMPARE(session.association.remoteEndpoint, QString("127.0.0.1:%1").arg(peer.serverPort()));
            QCOMPARE(session.association.contexts.size(), 1);
            const auto &context = session.association.contexts.first();
            QCOMPARE(context.id, 1);
            QCOMPARE(context.proposedSyntaxes.size(), 2);
            QCOMPARE(context.acceptedSyntax, QString(UID_LittleEndianImplicitTransferSyntax));
            QCOMPARE(session.association.outcome, QString("A-ASSOCIATE-AC received"));
            QCOMPARE(session.association.closure, QString("A-RELEASE confirmed"));
            QCOMPARE(session.messages.size(), 2);
            QCOMPARE(session.messages.first().command, QString("C-ECHO-RQ"));
            QCOMPARE(session.messages.last().command, QString("C-ECHO-RSP"));
            QCOMPARE(session.requestIndex(1).value_or(-1), 0);
            QVERIFY(session.messages.first().direction == Direction::Outgoing);
            QVERIFY(session.messages.last().direction == Direction::Incoming);
            for (const auto &message : session.messages) {
                QVERIFY(message.datasetPresent == false);
                QVERIFY(!message.dataset);
                QVERIFY(!message.commandSet.attributes.isEmpty());
            }
            QVERIFY(datasetText(session.messages.first().commandSet, true).contains("set by DCMTK during serialization"));
            if (index == 0) {
                ui::ProtocolInspector inspector(session, nullptr);
                QCOMPARE(inspector.findChild<QTableWidget *>("protocolMessages")->rowCount(), 2);
                QVERIFY(screenshot(session, 2, "verification-success-dimse"));
            }
            QVERIFY(result.details.contains(calling));
            QVERIFY(result.details.contains("Accepted context 1"));
            QCOMPARE(peer.callers().value(index), calling);
            QTRY_COMPARE(peer.released.load(), index + 1);
            const auto operation = id.toString(QUuid::WithoutBraces);
            const auto events = logger.snapshot();
            QVERIFY(std::ranges::any_of(events, [&](const auto &event) {
                return event.operation == operation && event.component == "Verification" &&
                    !event.synthetic && event.message.contains("0x0000");
            }));
        }
        QCOMPARE(peer.echoed.load(), 10);
    }

    void wrongPortAndUnreachableLoopback()
    {
        for (const auto &host : {QString("127.0.0.1"), QString("127.0.0.2")}) {
            QTcpServer reservation;
            QVERIFY(reservation.listen(QHostAddress::LocalHost));
            const auto unusedPort = reservation.serverPort();
            reservation.close();
            logging::Logger logger;
            OperationRunner runner(logger);
            QSignalSpy finished(&runner, &OperationRunner::finished);
            QVERIFY(!runner.start("C-ECHO", verificationOperation(source(), destination(unusedPort, host),
                                                                   {1, 1, 1})).isNull());
            QTRY_COMPARE(finished.size(), 1);
            const auto result = finished.first().at(1).value<OperationResult>();
            QCOMPARE(result.status, Status::Failed);
            QVERIFY(result.error.has_value());
            QVERIFY2(result.error->code == ErrorCode::TcpFailure, qPrintable(result.details));
            QVERIFY(result.elapsedMs < 4000);
            QVERIFY(!result.dicomStatus);
            QVERIFY(result.protocol);
            QVERIFY(!result.protocol->association.requestSent.value_or(false));
            QVERIFY(!result.protocol->association.peerMaxPdu);
            QVERIFY(result.protocol->association.contexts.first().acceptedSyntax.isEmpty());
            QVERIFY(result.protocol->messages.isEmpty());
        }
    }

    void silentTcpEndpointTimesOut()
    {
        struct SocketRuntime {
            SocketRuntime() { OFStandard::initializeNetwork(); }
            ~SocketRuntime() { OFStandard::shutdownNetwork(); }
        } runtime;
        struct Listener {
            DcmNativeSocketType socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            ~Listener()
            {
                if (socket == DCMNET_INVALID_SOCKET)
                    return;
#ifdef _WIN32
                closesocket(socket);
#else
                ::close(socket);
#endif
            }
        } peer;
        QVERIFY(peer.socket != DCMNET_INVALID_SOCKET);
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        QCOMPARE(::bind(peer.socket, reinterpret_cast<sockaddr *>(&endpoint), sizeof(endpoint)), 0);
        QCOMPARE(::listen(peer.socket, 1), 0);
#ifdef _WIN32
        int endpointSize = sizeof(endpoint);
#else
        socklen_t endpointSize = sizeof(endpoint);
#endif
        QCOMPARE(getsockname(peer.socket, reinterpret_cast<sockaddr *>(&endpoint), &endpointSize), 0);
        const auto port = ntohs(endpoint.sin_port);
        std::vector<std::unique_ptr<QTcpSocket>> backlog;
        bool silent = false;
        for (int index = 0; index < 8 && !silent; ++index) {
            auto socket = std::make_unique<QTcpSocket>();
            socket->connectToHost(QHostAddress::LocalHost, port);
            if (socket->waitForConnected(250))
                backlog.push_back(std::move(socket));
            else
                silent = true;
        }
#ifndef _WIN32
        if (!silent)
            QSKIP("The kernel does not silently drop connections when the listen backlog is full.");
#endif
        QVERIFY(silent);
        QVERIFY(!backlog.empty());

        auto work = verificationOperation(source(), destination(port), {1, 1, 1});
        std::atomic<bool> drained = false;
        logging::Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        const auto id = runner.start("C-ECHO", [work = std::move(work), &peer, &drained](
                                        auto stop, const auto &report) {
            return work(stop, [&](DiagnosticEvent event) {
                // Windows can eventually report refusal on a full backlog.
                // Reopen it after the SCU deadline to exercise a transient timeout.
                if (event.message == "TCP connection attempt ended without establishing a connection.") {
                    const auto pending = ::accept(peer.socket, nullptr, nullptr);
                    drained = pending != DCMNET_INVALID_SOCKET;
                    if (drained) {
#ifdef _WIN32
                        closesocket(pending);
#else
                        ::close(pending);
#endif
                    }
                }
                report(std::move(event));
            });
        });
        QVERIFY(!id.isNull());
        QTRY_COMPARE(finished.size(), 1);
        const auto result = finished.takeFirst().at(1).value<OperationResult>();
        QVERIFY(result.error.has_value());
        QVERIFY2(result.error->code == ErrorCode::ConnectionTimeout, qPrintable(result.details));
        QVERIFY(result.elapsedMs >= 850 && result.elapsedMs < 4000);
        QVERIFY(!result.dicomStatus);
        QVERIFY(drained);
    }

    void rejectionAndEchoFailure_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<QString>("called");
        QTest::addColumn<int>("error");
        QTest::newRow("called AE rejected") << int(LocalScp::Respond) << QString("WRONG_AE")
                                            << int(ErrorCode::AssociationRejected);
        QTest::newRow("presentation context rejected") << int(LocalScp::RejectContext) << QString("TEST_SCP")
                                                        << int(ErrorCode::PresentationContextRejected);
        QTest::newRow("transfer syntax rejected") << int(LocalScp::RejectSyntax) << QString("TEST_SCP")
                                                   << int(ErrorCode::PresentationContextRejected);
        QTest::newRow("nonzero DIMSE status") << int(LocalScp::FailEcho) << QString("TEST_SCP")
                                              << int(ErrorCode::EchoFailure);
    }

    void rejectionAndEchoFailure()
    {
        QFETCH(int, mode);
        QFETCH(QString, called);
        QFETCH(int, error);
        LocalScp peer{LocalScp::Mode(mode)};
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        logging::Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        QVERIFY(!runner.start("C-ECHO", verificationOperation(source(),
            destination(peer.serverPort(), "127.0.0.1", called))).isNull());
        QTRY_COMPARE(finished.size(), 1);
        const auto result = finished.first().at(1).value<OperationResult>();
        QCOMPARE(result.status, Status::Failed);
        QVERIFY(result.error.has_value());
        QVERIFY2(int(result.error->code) == error, qPrintable(result.details));
        QVERIFY(result.protocol);
        const auto &session = *result.protocol;
        QCOMPARE(session.result, QString("Failed"));
        QVERIFY(!session.errorCategory.isEmpty());
        verifyDiagnosticReport(session, QString("echo-failed-%1").arg(mode == LocalScp::RejectSyntax ? "syntax"
            : mode == LocalScp::RejectContext ? "context" : mode == LocalScp::FailEcho ? "dimse" : "association"));
        if (mode == LocalScp::FailEcho) {
            QCOMPARE(result.dicomStatus.value_or(0), quint16(0x0122));
            QTRY_COMPARE(peer.released.load(), 1);
        } else if (mode == LocalScp::RejectContext || mode == LocalScp::RejectSyntax) {
            QTRY_COMPARE(peer.disconnected.load(), 1);
            QCOMPARE(peer.echoed.load(), 0);
            const auto &context = session.association.contexts.first();
            QVERIFY(context.outcome.startsWith("Rejected"));
            QCOMPARE(context.rejectionReason.value_or(0), mode == LocalScp::RejectSyntax ? 4 : 3);
            QVERIFY(context.acceptedSyntax.isEmpty());
            QVERIFY(!context.proposedSyntaxes.isEmpty());
            QVERIFY(session.messages.isEmpty());
        } else {
            QCOMPARE(peer.rejected.load(), 1);
            QVERIFY(result.details.contains("Called AE", Qt::CaseInsensitive));
            QVERIFY(session.association.requestSent);
            QVERIFY(!session.association.rejection.isEmpty());
            QVERIFY(!session.association.peerMaxPdu);
            QVERIFY(session.messages.isEmpty());
            QVERIFY(screenshot(session, 1, "verification-rejected-association"));
        }
    }

    void dimseTimeoutAndCancel()
    {
        LocalScp peer(LocalScp::IgnoreEcho);
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        logging::Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        QVERIFY(!runner.start("C-ECHO", verificationOperation(source(), destination(peer.serverPort()),
                                                              {1, 1, 1})).isNull());
        QTRY_COMPARE(finished.size(), 1);
        auto result = finished.takeFirst().at(1).value<OperationResult>();
        QVERIFY(result.error.has_value());
        QCOMPARE(result.error->code, ErrorCode::DimseTimeout);
        QVERIFY(result.protocol);
        QCOMPARE(result.protocol->messages.size(), 1);
        QCOMPARE(result.protocol->errorCategory, QString("DIMSE response timed out"));
        verifyDiagnosticReport(*result.protocol, "echo-dimse-timeout");
        QVERIFY(!result.protocol->status);
        QVERIFY(result.protocol->association.closure.contains("without a confirmed"));
        QVERIFY(result.elapsedMs >= 850 && result.elapsedMs < 2500);
        QTRY_COMPARE(peer.disconnected.load(), 1);

        const auto id = runner.start("C-ECHO", verificationOperation(source(), destination(peer.serverPort()),
                                                                     {1, 1, 10}));
        QTRY_COMPARE(peer.echoed.load(), 2);
        QVERIFY(runner.cancel(id));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        result = finished.takeFirst().at(1).value<OperationResult>();
        QCOMPARE(result.status, Status::Cancelled);
        QTRY_COMPARE(peer.disconnected.load(), 2);
        QVERIFY(result.protocol);
        QCOMPARE(result.protocol->result, QString("Cancelled"));
        verifyDiagnosticReport(*result.protocol, "echo-cancelled");
    }

    void partialAssociationResponseTimesOut()
    {
        QTcpServer peer;
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        connect(&peer, &QTcpServer::newConnection, &peer, [&] {
            auto *socket = peer.nextPendingConnection();
            socket->write(QByteArray(1, char(2))); // A-ASSOCIATE-AC type without its header or payload.
        });
        logging::Logger logger;
        OperationRunner runner(logger);
        QSignalSpy finished(&runner, &OperationRunner::finished);
        QVERIFY(!runner.start("C-ECHO", verificationOperation(source(), destination(peer.serverPort()),
                                                              {1, 1, 1})).isNull());
        QTRY_COMPARE(finished.size(), 1);
        const auto result = finished.first().at(1).value<OperationResult>();
        QVERIFY(result.error.has_value());
        QCOMPARE(result.error->code, ErrorCode::AssociationTimeout);
        QVERIFY(result.elapsedMs >= 850 && result.elapsedMs < 2500);
    }
};

QTEST_MAIN(VerificationTest)
#include "VerificationTest.moc"
