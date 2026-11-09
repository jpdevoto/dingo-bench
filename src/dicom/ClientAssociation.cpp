// SPDX-License-Identifier: GPL-3.0-only
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include "ClientAssociation.h"

#include <dcmtk/config/osconfig.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <cerrno>
#endif
#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/assoc.h>
#include <dcmtk/dcmnet/dcmlayer.h>
#include <dcmtk/dcmnet/dcmtrans.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/ofstd/ofstd.h>

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostInfo>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

namespace dingobench::dicom {
QString conditionText(const OFCondition &condition)
{
    return QString::fromLocal8Bit(condition.text());
}

namespace {

using logging::Severity;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

template <typename T, OFCondition (*Destroy)(T **)>
void destroy(T *value)
{
    Destroy(&value);
}

struct NetworkRuntime {
    NetworkRuntime() { OFStandard::initializeNetwork(); }
    ~NetworkRuntime() { OFStandard::shutdownNetwork(); }
};

QString socketEndpoint(DcmNativeSocketType socket, bool peer)
{
    sockaddr_storage address{};
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    auto *native = reinterpret_cast<sockaddr *>(&address);
    if ((peer ? getpeername(socket, native, &length) : getsockname(socket, native, &length)) != 0)
        return {};
    char host[NI_MAXHOST]{}, port[NI_MAXSERV]{};
    if (getnameinfo(native, length, host, sizeof(host), port, sizeof(port),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return {};
    const auto numeric = QString::fromLatin1(host);
    return (numeric.contains(u':') ? u'[' + numeric + u']' : numeric) + u':' + QString::fromLatin1(port);
}

class ClientTransport final : public DcmTransportLayer {
public:
    ClientTransport(ClientPhase &phase, int timeout, const ReportDiagnostic &report, AssociationDetails &capture)
        : m_phase(phase), m_timeout(timeout), m_report(report), m_capture(capture) {}

    ~ClientTransport() override
    {
        if (connected) {
            if (m_capture.closure.isEmpty())
                m_capture.closure = "TCP closed without a confirmed DICOM release";
            m_report({Severity::Info, "TCP connection closed."});
        }
    }

    DcmTransportConnection *createConnection(DcmNativeSocketType socket, OFBool secure) override
    {
        if (secure)
            return nullptr;
        connected = true;
        m_capture.connection = "TCP established";
        m_capture.localEndpoint = socketEndpoint(socket, false);
        m_capture.remoteEndpoint = socketEndpoint(socket, true);
        m_report({Severity::Info, "TCP connection established. Negotiating the DICOM association."});
        m_phase.begin(m_timeout);
        return new BoundedConnection(socket, m_phase, &m_capture);
    }

    bool connected = false;

private:
    ClientPhase &m_phase;
    int m_timeout;
    const ReportDiagnostic &m_report;
    AssociationDetails &m_capture;
};

QHostAddress resolve(const QString &host, int seconds, std::stop_token stop)
{
    QHostAddress address;
    if (address.setAddress(host))
        return address;

    QEventLoop wait;
    bool completed = false;
    QElapsedTimer deadline;
    deadline.start();
    QTimer check;
    check.setInterval(25);
    QObject::connect(&check, &QTimer::timeout, &wait, [&] {
        if (stop.stop_requested() || deadline.hasExpired(seconds * 1000))
            wait.quit();
    });
    const auto id = QHostInfo::lookupHost(host, &wait, [&](const QHostInfo &result) {
        const auto addresses = result.addresses();
        const auto ipv4 = std::ranges::find_if(addresses, [](const QHostAddress &candidate) {
            return candidate.protocol() == QAbstractSocket::IPv4Protocol;
        });
        if (!addresses.isEmpty())
            address = ipv4 == addresses.end() ? addresses.first() : *ipv4;
        completed = true;
        wait.quit();
    });
    check.start();
    if (!completed)
        wait.exec();
    if (!completed)
        QHostInfo::abortHostLookup(id);
    return address;
}

bool connectionTimedOut(const OFCondition &condition, const QHostAddress &address, quint16 port,
                        std::stop_token stop)
{
    if (!conditionText(condition).endsWith(QStringLiteral(" (Timeout)")))
        return false;
#ifdef _WIN32
    // 3.7.1 waits on Winsock's write set alone, so an asynchronous refusal can
    // look like a timeout. Confirm a negative TCP result with a bounded probe.
    const auto refused = [](int error) {
        return error == WSAECONNREFUSED || error == WSAENETUNREACH || error == WSAEHOSTUNREACH;
    };
    addrinfo hints{};
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *entry = nullptr;
    if (getaddrinfo(address.toString().toLatin1().constData(),
                    QByteArray::number(port).constData(), &hints, &entry) != 0)
        return true;
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> resolved(entry, freeaddrinfo);
    struct ProbeSocket {
        SOCKET handle;
        ~ProbeSocket() { if (handle != INVALID_SOCKET) closesocket(handle); }
    } probe{socket(entry->ai_family, SOCK_STREAM, IPPROTO_TCP)};
    u_long nonBlocking = 1;
    if (probe.handle == INVALID_SOCKET || ioctlsocket(probe.handle, FIONBIO, &nonBlocking) != 0)
        return true;
    if (connect(probe.handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0)
        return true;
    const int initialError = WSAGetLastError();
    if (initialError != WSAEWOULDBLOCK)
        return !refused(initialError);

    const auto deadline = Clock::now() + 2300ms;
    while (!stop.stop_requested() && Clock::now() < deadline) {
        fd_set writable, errors;
        FD_ZERO(&writable);
        FD_ZERO(&errors);
        FD_SET(probe.handle, &writable);
        FD_SET(probe.handle, &errors);
        timeval interval{0, 25000};
        const int ready = select(0, nullptr, &writable, &errors, &interval);
        if (ready < 0)
            break;
        if (ready > 0) {
            int error = 0;
            int size = sizeof(error);
            return getsockopt(probe.handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) != 0
                || !refused(error);
        }
    }
#else
    Q_UNUSED(address);
    Q_UNUSED(port);
    Q_UNUSED(stop);
#endif
    return true;
}

}  // namespace

OperationResult withClientAssociation(const profiles::Profile &source, const profiles::Profile &destination,
    ClientTimeouts timeouts, std::stop_token stop, const ReportDiagnostic &report,
    DiagnosticSession &session, const QString &command, const AssociationWork &work)
{
    auto &capture = session.association;
    session.identities = {source, destination};
    capture.connection = "Not attempted";
    capture.requestSent = false;
    capture.callingAe = source.ae.trimmed();
    capture.calledAe = destination.ae.trimmed();
    const auto validTimeout = [](int value) { return value >= 1 && value <= maximumClientTimeout; };
    const auto sourceError = profiles::Store::validate(profiles::Kind::Source, source);
    const auto destinationError = profiles::Store::validate(profiles::Kind::Destination, destination);
    if (!sourceError.isEmpty())
        return OperationResult::failed({ErrorCode::InvalidInput, "Source profile: " + sourceError});
    if (!destinationError.isEmpty())
        return OperationResult::failed({ErrorCode::InvalidInput, "Destination profile: " + destinationError});
    if (!validTimeout(timeouts.connection) || !validTimeout(timeouts.association) || !validTimeout(timeouts.dimse))
        return OperationResult::failed({ErrorCode::InvalidInput, "DICOM timeouts are out of range."});

    const auto host = destination.address.trimmed();
    const auto displayHost = host.contains(u':') ? u'[' + host + u']' : host;
    capture.requestedEndpoint = displayHost + u':' + QString::number(destination.port);
    QString details = QString("Calling AE %1 → Called AE %2\nRemote endpoint %3:%4")
        .arg(source.ae.trimmed(), destination.ae.trimmed(), displayHost)
        .arg(destination.port);
    const auto fail = [&](ErrorCode code, QString message, const QString &technical = {}) {
        if (!technical.isEmpty()) {
            details += u'\n' + technical;
            session.technicalError = technical;
        }
        if (capture.outcome.isEmpty())
            capture.outcome = message;
        auto result = OperationResult::failed({code, std::move(message)});
        result.details = details;
        report({Severity::Debug, "Association failure details.", details});
        return result;
    };
    const auto cancel = [&] {
        if (capture.outcome.isEmpty())
            capture.outcome = "Cancelled before negotiation completed";
        auto result = OperationResult::cancelled();
        result.details = details;
        return result;
    };

    report({Severity::Info, "Starting TCP connection for " + command + u'.', details});
    capture.connection = "Resolving remote address";
    const auto address = resolve(host, timeouts.connection, stop);
    if (stop.stop_requested())
        return cancel();
    if (address.isNull()) {
        capture.connection = "DNS resolution failed or timed out";
        return fail(ErrorCode::Dns, "DNS resolution failed or exceeded the connection timeout.");
    }
    capture.connection = "Address resolved; TCP not attempted";
    details += "\nResolved address " + address.toString();

    const NetworkRuntime runtime;
    ClientPhase phase{stop};
    ClientTransport transport(phase, timeouts.association, report, capture);
    T_ASC_Network *rawNetwork = nullptr;
    auto condition = ASC_initializeNetwork(NET_REQUESTOR, 0, timeouts.association, &rawNetwork);
    std::unique_ptr<T_ASC_Network, decltype(&destroy<T_ASC_Network, ASC_dropNetwork>)> network(
        rawNetwork, &destroy<T_ASC_Network, ASC_dropNetwork>);
    if (condition.bad())
        return fail(ErrorCode::TcpFailure, "Could not initialize the DICOM network.", conditionText(condition));

    T_ASC_Parameters *rawParams = nullptr;
    condition = ASC_createAssociationParameters(&rawParams, ASC_DEFAULTMAXPDU, timeouts.connection);
    std::unique_ptr<T_ASC_Parameters, decltype(&destroy<T_ASC_Parameters, ASC_destroyAssociationParameters>)>
        params(rawParams, &destroy<T_ASC_Parameters, ASC_destroyAssociationParameters>);
    if (condition.bad())
        return fail(ErrorCode::Internal, "Could not create association parameters.", conditionText(condition));

    const auto calling = source.ae.trimmed().toLatin1();
    const auto called = destination.ae.trimmed().toLatin1();
    const auto endpoint = (address.toString() + u':' + QString::number(destination.port)).toLatin1();
    ASC_setAPTitles(params.get(), calling.constData(), called.constData(), nullptr);
    ASC_setPresentationAddresses(params.get(), "localhost", endpoint.constData());
    ASC_setProtocolFamily(params.get(), address.protocol() == QAbstractSocket::IPv6Protocol
                                       ? ASC_AF_INET6 : ASC_AF_INET);
    for (auto &proposal : capture.contexts) {
        const auto sop = proposal.abstractSyntax.toLatin1();
        QList<QByteArray> names;
        for (const auto &syntax : proposal.proposedSyntaxes)
            names.append(syntax.toLatin1());
        QList<const char *> syntaxes;
        for (const auto &name : names)
            syntaxes.append(name.constData());
        condition = ASC_addPresentationContext(params.get(), static_cast<T_ASC_PresentationContextID>(proposal.id),
                                               sop.constData(), syntaxes.data(), int(syntaxes.size()),
                                               proposal.requestStorageScp ? ASC_SC_ROLE_SCP : ASC_SC_ROLE_DEFAULT);
        if (condition.bad())
            break;
        proposal.sopClass = QString::fromLatin1(dcmFindNameOfUID(sop.constData(), "Unknown SOP Class"));
        proposal.proposedRole = proposal.requestStorageScp ? "SCP (requestor)" : "Default (requestor SCU)";
    }
    if (condition.good())
        condition = ASC_setTransportLayer(network.get(), &transport, 0);
    if (condition.bad())
        return fail(ErrorCode::Internal, "Could not configure the DICOM association.", conditionText(condition));
    char applicationContext[65]{};
    if (ASC_getApplicationContextName(params.get(), applicationContext, sizeof(applicationContext)).good())
        capture.applicationContext = QString::fromLatin1(applicationContext);
    capture.localMaxPdu = params->ourMaxPDUReceiveSize;
    report({Severity::Debug, QString("Proposed %1 presentation context(s).").arg(capture.contexts.size()), details});
    if (stop.stop_requested())
        return cancel();

    T_ASC_Association *rawAssociation = nullptr;
    capture.connection = "TCP connection attempted";
    // Allow for DCMTK's whole-second clock so BoundedConnection's monotonic deadline wins.
    condition = ASC_requestAssociation(network.get(), params.get(), &rawAssociation, nullptr, nullptr,
                                       DUL_NOBLOCK, timeouts.association + 1);
    std::unique_ptr<T_ASC_Association, decltype(&destroy<T_ASC_Association, ASC_destroyAssociation>)>
        association(rawAssociation, &destroy<T_ASC_Association, ASC_destroyAssociation>);
    if (association)
        params.release(); // ASC_requestAssociation transfers ownership once it allocates an association.
    if (condition == DUL_PEERABORTEDASSOCIATION)
        capture.closure = "Peer A-ABORT reported by DCMTK";
    if (!transport.connected && condition.bad()) {
        if (stop.stop_requested())
            return cancel();
        report({Severity::Info, "TCP connection attempt ended without establishing a connection."});
        const bool timedOut = connectionTimedOut(condition, address, static_cast<quint16>(destination.port), stop);
        if (stop.stop_requested())
            return cancel();
        capture.connection = timedOut ? "TCP timed out" : "TCP refused or unreachable";
        return fail(timedOut ? ErrorCode::ConnectionTimeout : ErrorCode::TcpFailure,
                    timedOut ? "TCP connection timed out." : "TCP connection refused or unreachable.",
                    conditionText(condition));
    }
    if (condition == DUL_ASSOCIATIONREJECTED) {
        T_ASC_RejectParameters rejection{};
        OFString description;
        ASC_getRejectParameters(association->params, &rejection);
        ASC_printRejectParameters(description, &rejection);
        capture.outcome = "A-ASSOCIATE-RJ received";
        capture.rejection = QString::fromLatin1(description.c_str()).trimmed();
        return fail(ErrorCode::AssociationRejected, "The peer rejected the DICOM association.",
                    capture.rejection);
    }
    if (stop.stop_requested())
        return cancel();
    if (phase.expired || condition == DUL_READTIMEOUT)
        return fail(ErrorCode::AssociationTimeout, "DICOM association negotiation timed out.");
    if (condition.bad())
        return fail(ErrorCode::ProtocolFailure, "DICOM association negotiation failed.", conditionText(condition));

    report({Severity::Info, "DICOM association accepted."});
    capture.outcome = "A-ASSOCIATE-AC received";
    capture.peerMaxPdu = association->params->theirMaxPDUReceiveSize;
    for (int index = 0; index < capture.contexts.size(); ++index) {
        T_ASC_PresentationContext context{};
        if (ASC_getPresentationContext(association->params, index, &context).bad() ||
            context.resultReason == ASC_P_NOTYETNEGOTIATED)
            continue;
        auto &record = capture.contexts[index];
        if (context.resultReason == ASC_P_ACCEPTANCE) {
            record.outcome = "Accepted";
            record.acceptedSyntax = QString::fromLatin1(context.acceptedTransferSyntax);
            record.acceptedRole = context.acceptedRole == ASC_SC_ROLE_DEFAULT ? QString("Default (requestor SCU)")
                : QString::fromLatin1(ASC_role2String(context.acceptedRole)) + " (requestor)";
            if (!record.requestStorageScp)
                details += QString("\nAccepted context %1 · Transfer syntax %2")
                    .arg(context.presentationContextID).arg(record.acceptedSyntax);
        } else {
            record.rejectionReason = int(context.resultReason);
            record.outcome = context.resultReason == ASC_P_TRANSFERSYNTAXESNOTSUPPORTED
                ? "Rejected: transfer syntaxes not supported"
                : context.resultReason == ASC_P_ABSTRACTSYNTAXNOTSUPPORTED
                    ? "Rejected: abstract syntax not supported"
                    : QString("Rejected (reason %1)").arg(*record.rejectionReason);
        }
        report({record.requestStorageScp ? Severity::Debug : Severity::Info,
                QString("Presentation context %1: %2.").arg(record.id).arg(record.outcome),
                record.abstractSyntax + " · " + record.proposedSyntaxes.join(", ") +
                (record.acceptedRole.isEmpty() ? QString{} : " · Role " + record.acceptedRole)});
    }
    auto result = work(*association, phase);
    if (phase.release && !stop.stop_requested()) {
        phase.begin(1);
        if (ASC_releaseAssociation(association.get()).good()) {
            capture.closure = "A-RELEASE confirmed";
            report({Severity::Info, "DICOM association released."});
        } else {
            capture.closure = "Release unconfirmed; TCP closed";
            details += "\nThe peer did not complete association release. The local connection was closed.";
            report({Severity::Warning, "Association release did not complete. Closing the local connection."});
        }
    }
    if (stop.stop_requested() && result.status == OperationResult::Status::Succeeded)
        result = OperationResult::cancelled();
    result.details = result.details.isEmpty() ? details : details + u'\n' + result.details;
    return result;
}

}  // namespace dingobench::dicom
