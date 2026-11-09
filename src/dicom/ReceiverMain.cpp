// SPDX-License-Identifier: GPL-3.0-only
#include "BoundedConnection.h"
#include "ReceivedObject.h"
#include "dingobench/dicom/StorageReceive.h"

#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmdata/dcvrui.h>
#include <dcmtk/dcmdata/dcxfer.h>
#include <dcmtk/dcmnet/assoc.h>
#include <dcmtk/dcmnet/dcmlayer.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/ofstd/ofstd.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QTcpServer>

#include <cstdio>
#include <thread>

namespace dingobench::dicom {
namespace {

constexpr int associationSeconds = 5;
constexpr int commandSeconds = 30;
constexpr int datasetSeconds = 30;

void emitEvent(QJsonObject event)
{
    event.insert("timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    const auto line = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

int startupError(const QString &message)
{
    emitEvent({{"type", "error"}, {"message", message}});
    return 1;
}

QHostAddress addressFor(const QString &bind)
{
    QHostAddress address;
    if (address.setAddress(bind))
        return address;
    for (const auto &adapter : QNetworkInterface::allInterfaces()) {
        if (adapter.name() != bind && adapter.humanReadableName() != bind)
            continue;
        QList<QHostAddress> candidates;
        for (const auto &entry : adapter.addressEntries()) {
            if (!entry.ip().isNull())
                candidates.append(entry.ip());
        }
        const bool loopbackAdapter = adapter.flags().testFlag(QNetworkInterface::IsLoopBack);
        std::ranges::sort(candidates, [loopbackAdapter](const QHostAddress &left, const QHostAddress &right) {
            if (left.protocol() != right.protocol())
                return left.protocol() == QAbstractSocket::IPv4Protocol;
            if (loopbackAdapter && left.isLoopback() != right.isLoopback())
                return left.isLoopback();
            return left.toString() < right.toString();
        });
        return candidates.value(0);
    }
    return {};
}

QString endpoint(const QHostAddress &address, quint16 port)
{
    const auto host = address.toString();
    return (host.contains(u':') ? u'[' + host + u']' : host) + u':' + QString::number(port);
}

class ReceiveTransport final : public DcmTransportLayer {
public:
    explicit ReceiveTransport(ClientPhase &phase) : m_phase(phase) {}
    DcmTransportConnection *createConnection(DcmNativeSocketType socket, OFBool secure) override
    {
        return secure ? nullptr : new BoundedConnection(socket, m_phase);
    }

private:
    ClientPhase &m_phase;
};

struct Settings {
    QString ae, output, stage;
    bool strict = true;
};

bool supportedSyntax(const char *uid)
{
    const DcmXfer xfer(uid);
    return xfer.getXfer() != EXS_Unknown && xfer.getStreamCompression() == ESC_none;
}

bool negotiate(T_ASC_Parameters *parameters)
{
    for (int i = 0; i < ASC_countPresentationContexts(parameters); ++i) {
        T_ASC_PresentationContext context{};
        if (ASC_getPresentationContext(parameters, i, &context).bad())
            return false;
        const bool storage = dcmIsaStorageSOPClassUID(context.abstractSyntax, ESSC_All);
        const char *chosen = nullptr;
        if (storage) {
            for (int j = 0; j < context.transferSyntaxCount && !chosen; ++j)
                if (supportedSyntax(context.proposedTransferSyntaxes[j]))
                    chosen = context.proposedTransferSyntaxes[j];
        }
        const auto result = chosen ? ASC_acceptPresentationContext(parameters, context.presentationContextID, chosen)
            : ASC_refusePresentationContext(parameters, context.presentationContextID,
                storage ? ASC_P_TRANSFERSYNTAXESNOTSUPPORTED : ASC_P_ABSTRACTSYNTAXNOTSUPPORTED);
        if (result.bad())
            return false;
    }
    return true;
}

void serve(DcmNativeSocketType socket, const Settings &settings, std::stop_token stop)
{
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto started = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const auto publish = [&](QJsonObject fields) {
        fields.insert("association", id);
        fields.insert("associationStarted", started);
        emitEvent(std::move(fields));
    };
    ClientPhase phase{stop};
    ReceiveTransport transport(phase);
    dcmExternalSocketHandle.set(socket);
    T_ASC_Network *rawNetwork = nullptr;
    auto condition = ASC_initializeNetwork(NET_ACCEPTOR, 0, associationSeconds, &rawNetwork);
    const auto drop = [](T_ASC_Network *network) { ASC_dropNetwork(&network); };
    const std::unique_ptr<T_ASC_Network, decltype(drop)> network(rawNetwork, drop);
    if (condition.good())
        condition = ASC_setTransportLayer(network.get(), &transport, 0);
    T_ASC_Association *rawAssociation = nullptr;
    phase.begin(associationSeconds);
    phase.limitRead(1024 * 1024);
    if (condition.good())
        condition = ASC_receiveAssociation(network.get(), &rawAssociation, ASC_DEFAULTMAXPDU,
                                           nullptr, nullptr, OFFalse, DUL_NOBLOCK, associationSeconds);
    else {
#ifdef _WIN32
        closesocket(socket);
#else
        ::close(socket);
#endif
    }
    dcmExternalSocketHandle.set(DCMNET_INVALID_SOCKET);
    const auto destroy = [](T_ASC_Association *association) { ASC_destroyAssociation(&association); };
    const std::unique_ptr<T_ASC_Association, decltype(destroy)> association(rawAssociation, destroy);
    if (condition.bad() || stop.stop_requested()) {
        publish({{"type", "association"}, {"action", "Interrupted"},
                 {"message", "Incoming association ended before negotiation completed."}});
        return;
    }
    char calling[DIC_AE_LEN + 1]{}, called[DIC_AE_LEN + 1]{}, peer[128]{};
    ASC_getAPTitles(association->params, calling, sizeof(calling), called, sizeof(called), nullptr, 0);
    ASC_getPresentationAddresses(association->params, peer, sizeof(peer), nullptr, 0);
    const QJsonObject metadata{{"callingAe", QString::fromLatin1(calling).trimmed()},
        {"calledAe", QString::fromLatin1(called).trimmed()}, {"peer", QString::fromLatin1(peer)}};
    const auto event = [&](const QString &action, const QString &message) {
        auto fields = metadata;
        fields.insert("type", "association");
        fields.insert("action", action);
        fields.insert("message", message);
        if (action == "Accepted" || action == "Rejected") {
            QJsonArray contexts;
            for (int index = 0; index < ASC_countPresentationContexts(association->params); ++index) {
                T_ASC_PresentationContext context{};
                if (ASC_getPresentationContext(association->params, index, &context).bad())
                    continue;
                QJsonArray syntaxes;
                for (int syntax = 0; syntax < context.transferSyntaxCount; ++syntax)
                    syntaxes.append(QString::fromLatin1(context.proposedTransferSyntaxes[syntax]));
                contexts.append(QJsonObject{{"id", context.presentationContextID},
                    {"abstractSyntax", QString::fromLatin1(context.abstractSyntax)},
                    {"syntaxes", syntaxes}, {"acceptedSyntax", QString::fromLatin1(context.acceptedTransferSyntax)},
                    {"reason", int(context.resultReason)},
                    {"proposedRole", QString::fromLatin1(ASC_role2String(context.proposedRole))},
                    {"acceptedRole", context.resultReason == ASC_P_ACCEPTANCE
                        ? QString::fromLatin1(ASC_role2String(context.acceptedRole)) : QString()}});
            }
            fields.insert("contexts", contexts);
            char applicationContext[65]{};
            if (ASC_getApplicationContextName(association->params, applicationContext, sizeof(applicationContext)).good())
                fields.insert("applicationContext", QString::fromLatin1(applicationContext));
            fields.insert("localMaxPdu", qint64(association->params->ourMaxPDUReceiveSize));
            fields.insert("peerMaxPdu", qint64(association->params->theirMaxPDUReceiveSize));
        }
        publish(fields);
    };
    phase.begin(associationSeconds);
    if (settings.strict && metadata.value("calledAe").toString() != settings.ae) {
        const T_ASC_RejectParameters rejection{ASC_RESULT_REJECTEDPERMANENT, ASC_SOURCE_SERVICEUSER,
                                                ASC_REASON_SU_CALLEDAETITLENOTRECOGNIZED};
        ASC_rejectAssociation(association.get(), &rejection);
        event("Rejected", "Association rejected: Called AE Title not recognized.");
        return;
    }
    if (!negotiate(association->params) || ASC_acknowledgeAssociation(association.get()).bad()) {
        event("Interrupted", "Incoming association negotiation failed.");
        return;
    }
    event("Accepted", "Incoming Storage association accepted.");
    while (!stop.stop_requested()) {
        phase.begin(commandSeconds);
        phase.limitRead(1024 * 1024);
        T_ASC_PresentationContextID context = 0;
        T_DIMSE_Message command{};
        condition = DIMSE_receiveCommand(association.get(), DIMSE_NONBLOCKING, commandSeconds,
                                         &context, &command, nullptr);
        if (condition == DUL_PEERREQUESTEDRELEASE) {
            phase.begin(associationSeconds);
            if (ASC_acknowledgeRelease(association.get()).good()) {
                event("Released", "Incoming association released.");
                return;
            }
            break;
        }
        if (condition.bad())
            break;
        if (command.CommandField != DIMSE_C_STORE_RQ || command.msg.CStoreRQ.DataSetType == DIMSE_DATASET_NULL) {
            event("Interrupted", "Unexpected DIMSE command on a Storage association.");
            return;
        }
        auto &request = command.msg.CStoreRQ;
        T_ASC_PresentationContext negotiated{};
        if (ASC_findAcceptedPresentationContext(association->params, context, &negotiated).bad() ||
            qstrcmp(negotiated.abstractSyntax, request.AffectedSOPClassUID) != 0) {
            event("Interrupted", "C-STORE command did not match its negotiated Storage context.");
            return;
        }
        auto row = metadata;
        row.insert("type", "store");
        row.insert("context", int(context));
        row.insert("messageId", int(request.MessageID));
        const auto diagnosticUid = [](const char *uid) {
            return DcmUniqueIdentifier::checkStringValue(uid, "1").good()
                ? QString::fromLatin1(uid) : QString();
        };
        row.insert("sopClass", diagnosticUid(request.AffectedSOPClassUID));
        row.insert("sopInstance", diagnosticUid(request.AffectedSOPInstanceUID));
        if (request.opts & O_STORE_MOVEORIGINATORAETITLE)
            row.insert("moveOriginatorAe", QString::fromLatin1(request.MoveOriginatorApplicationEntityTitle).trimmed());
        if (request.opts & O_STORE_MOVEORIGINATORID)
            row.insert("moveOriginatorId", int(request.MoveOriginatorID));
        auto started = row;
        started.insert("action", "Receiving");
        started.insert("message", "C-STORE request received.");
        publish(started);
        phase.begin(datasetSeconds);
        phase.limitRead(datasetWireLimit);
        DcmDataset *rawDataset = nullptr;
        auto datasetContext = context;
        condition = DIMSE_receiveDataSetInMemory(association.get(), DIMSE_NONBLOCKING, datasetSeconds,
                                                 &datasetContext, &rawDataset, nullptr, nullptr);
        std::unique_ptr<DcmDataset> dataset(rawDataset);
        if (condition.bad() || !dataset || datasetContext != context || stop.stop_requested()) {
            row.insert("action", "Interrupted");
            row.insert("message", phase.readLimitReached ? "Incoming object exceeded the 64 MiB wire limit."
                                                        : "Incoming object was incomplete and was not saved.");
            publish(row);
            break;
        }
        const auto outcome = saveReceived(std::move(dataset), request, settings.output, settings.stage, stop);
        row.insert("action", outcome.status == STATUS_Success ? "Stored" : "Failed");
        row.insert("message", outcome.message);
        row.insert("bytes", outcome.bytes);
        row.insert("file", outcome.name);
        row.insert("status", int(outcome.status));
        T_DIMSE_C_StoreRSP response{};
        response.DimseStatus = outcome.status;
        phase.begin(associationSeconds);
        const auto sent = DIMSE_sendStoreResponse(association.get(), context, &request, &response, nullptr);
        row.insert("responseSent", sent.good());
        if (sent.bad())
            row.insert("message", outcome.message + " Response delivery was not confirmed.");
        publish(row);
        if (sent.bad())
            break;
    }
    event("Interrupted", stop.stop_requested() ? "Incoming association closed because the receiver stopped."
                                             : "Incoming association ended without a confirmed release.");
}

class Listener final : public QTcpServer {
public:
    explicit Listener(Settings settings) : m_settings(std::move(settings)) { m_worker.setMaxThreadCount(1); }
    ~Listener() override { m_worker.waitForDone(); }

    void stop()
    {
        close();
        m_stop.request_stop();
        if (!m_busy)
            QCoreApplication::quit();
    }

protected:
    void incomingConnection(qintptr socket) override
    {
        pauseAccepting();
        m_busy = true;
        m_worker.start([this, socket, stop = m_stop.get_token()] {
            serve(static_cast<DcmNativeSocketType>(socket), m_settings, stop);
            QMetaObject::invokeMethod(this, [this] {
                m_busy = false;
                if (m_stop.stop_requested())
                    QCoreApplication::quit();
                else
                    resumeAccepting();
            }, Qt::QueuedConnection);
        });
    }

private:
    Settings m_settings;
    QThreadPool m_worker;
    std::stop_source m_stop;
    bool m_busy = false;
};

}  // namespace
}  // namespace dingobench::dicom

int main(int argc, char **argv)
{
    using namespace dingobench::dicom;
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    const QCommandLineOption ae("ae", "Local AE Title", "title");
    const QCommandLineOption port("port", "Listening port", "port");
    const QCommandLineOption bind("bind", "Local interface or IP", "address");
    const QCommandLineOption output("output", "Received-file directory", "directory");
    const QCommandLineOption stage("stage", "Private staging directory", "directory");
    const QCommandLineOption strict("strict-ae", "Require the configured Called AE Title");
    const QCommandLineOption allowAny("allow-any-ae", "Accept any Called AE Title");
    parser.addOptions({ae, port, bind, output, stage, strict, allowAny});
    if (!parser.parse(app.arguments()))
        return startupError("Invalid receiver arguments.");
    bool validPort = false;
    const int number = parser.value(port).toInt(&validPort);
    const Settings settings{parser.value(ae), parser.value(output), parser.value(stage), !parser.isSet(allowAny)};
    if (!validPort || number < 1 || number > 65535 || settings.ae.isEmpty() ||
        !stagingAllowed(settings.output, settings.stage))
        return startupError("Invalid receiver port, AE Title, or output directory.");
    const auto address = addressFor(parser.value(bind));
    if (address.isNull())
        return startupError("The configured local interface or bind address was not found.");
    dcmDisableGethostbyaddr.set(OFTrue);
    OFStandard::initializeNetwork();
    Listener listener(settings);
    if (!listener.listen(address, quint16(number))) {
        OFStandard::shutdownNetwork();
        return startupError("Cannot bind " + endpoint(address, quint16(number)) + ". " + listener.errorString());
    }
    emitEvent({{"type", "ready"}, {"endpoint", endpoint(listener.serverAddress(), listener.serverPort())}});
    // EOF on the private QProcess stdin pipe is the graceful shutdown signal.
    std::thread control([&listener] {
        while (std::getchar() != EOF) {}
        QMetaObject::invokeMethod(&listener, [&listener] { listener.stop(); }, Qt::QueuedConnection);
    });
    const auto result = app.exec();
    control.join();
    OFStandard::shutdownNetwork();
    return result;
}
