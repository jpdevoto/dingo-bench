// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/dicom/DiagnosticSession.h"

#include "dingobench/dicom/StorageReceive.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QRegularExpression>
#include <algorithm>
#include <type_traits>
#include <utility>

namespace dingobench::dicom {
namespace {

QJsonValue available(const QString &value)
{
    return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
}

template <class T>
QJsonValue available(const std::optional<T> &value)
{
    if (!value)
        return QJsonValue(QJsonValue::Null);
    if constexpr (std::is_same_v<T, bool>)
        return QJsonValue(*value);
    else
        return QJsonValue(qint64(*value));
}

QString show(const QString &value)
{
    return value.isEmpty() ? QString("Unavailable") : value;
}

template <class T>
QString show(const std::optional<T> &value)
{
    return value ? QString::number(*value) : QString("Unavailable");
}

QString when(const QDateTime &value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QString hexStatus(quint16 value)
{
    return "0x" + QString::number(value, 16).toUpper().rightJustified(4, u'0');
}

const QString reportTitle = "DingoBench diagnostic session report";
constexpr qint64 maximumReportBytes = 16 * 1024 * 1024;

QString privateValue(const QString &value, bool include)
{
    return include || value.isEmpty() ? value : QString("<redacted>");
}

QString uid(const QString &value)
{
    static const QRegularExpression pattern(R"(^[0-9]+(?:\.[0-9]+)+$)");
    return value.size() <= 64 && pattern.match(value).hasMatch() ? value : QString();
}

QString logSummary(const QString &message)
{
    static const QStringList safe{
        "Operation started.", "Operation completed.", "Operation completed with warnings.",
        "Operation cancelled.", "TCP connection closed.",
        "TCP connection established. Negotiating the DICOM association.",
        "TCP connection attempt ended without establishing a connection.",
        "DICOM association accepted.", "Association failure details.",
        "Inbound association received", "Association ended", "Association accepted",
        "Association rejected", "C-STORE request received.", "Incoming Storage association accepted.",
        "Incoming association released.", "Correlated C-STORE object saved locally.",
        "Correlated incoming C-STORE failed or was interrupted.", "C-FIND request received", "C-FIND complete",
        "C-FIND match", "C-FIND match with warning", "C-FIND cancelled",
        "C-FIND identifier rejected", "C-FIND response warning or failure"
    };
    return safe.contains(message) ? message : QString("<redacted>");
}

bool protocolCounter(const Attribute &attribute)
{
    static const QStringList counters{"(0000,1020)", "(0000,1021)", "(0000,1022)", "(0000,1023)"};
    bool numeric = false;
    attribute.value.toUShort(&numeric);
    return numeric && counters.contains(attribute.tag);
}

QString shownValue(const Attribute &attribute, bool includeValues)
{
    return privateValue(attribute.value, includeValues || attribute.kind != Attribute::Value || protocolCounter(attribute));
}

QJsonArray attributeJson(const QList<Attribute> &attributes, bool includeValues)
{
    QJsonArray array;
    for (const auto &attribute : attributes) {
        static const QStringList kinds{"value", "empty", "binary_omitted", "sequence", "item", "unavailable"};
        array.append(QJsonObject{
            {"tag", available(attribute.tag)}, {"name", attribute.name}, {"vr", available(attribute.vr)},
            {"vm", attribute.kind == Attribute::Item || attribute.vm < 0
                ? QJsonValue(QJsonValue::Null) : QJsonValue(attribute.vm)},
            {"kind", kinds.at(attribute.kind)}, {"value", shownValue(attribute, includeValues)},
            {"children", attributeJson(attribute.children, includeValues)}
        });
    }
    return array;
}

QJsonObject datasetJson(const DatasetSnapshot &dataset, bool includeValues)
{
    return {{"truncated", dataset.truncated}, {"attributes", attributeJson(dataset.attributes, includeValues)}};
}

QJsonValue datasetJson(const std::optional<DatasetSnapshot> &dataset, bool includeValues)
{
    return dataset ? QJsonValue(datasetJson(*dataset, includeValues)) : QJsonValue(QJsonValue::Null);
}

void appendAttributes(QString &text, const QList<Attribute> &attributes, bool includeValues, int depth)
{
    for (const auto &attribute : attributes) {
        text += QString(depth * 2, u' ') + (attribute.tag.isEmpty() ? QString() : attribute.tag + u' ') +
            (attribute.vr.isEmpty() ? QString() : attribute.vr + u' ') + attribute.name;
        if (attribute.kind != Attribute::Item)
            text += QString(" [VM %1] %2").arg(attribute.vm < 0 ? QString("Unavailable") : QString::number(attribute.vm),
                                              shownValue(attribute, includeValues));
        text += u'\n';
        appendAttributes(text, attribute.children, includeValues, depth + 1);
    }
}

}  // namespace

void DiagnosticSession::finish(const QDateTime &end, QString finalResult, QString closure)
{
    ended = end;
    durationMs = qMax(qint64(0), started.msecsTo(ended));
    result = std::move(finalResult);
    association.closure = std::move(closure);
}

void DiagnosticSession::record(DimseRecord message)
{
    if (messages.size() >= 24) {
        messages.removeAt(8);
        messagesTruncated = true;
        ++messagesOmitted;
    }
    if (!detailedCapture)
        message.dataset.reset();
    messages.append(std::move(message));
}

void DiagnosticSession::addLog(logging::Event event)
{
    if (event.operation != id.toString(QUuid::WithoutBraces))
        return;
    constexpr int prefix = 16;
    // A replay from the central logger cannot restore the bounded capture gap.
    if (logsOmitted && logs.size() > prefix && logs.at(prefix - 1).sequence < event.sequence &&
        event.sequence < logs.at(prefix).sequence)
        return;
    if (logs.isEmpty() || event.sequence > logs.constLast().sequence) {
        logs.append(std::move(event));
    } else {
        const auto position = std::ranges::lower_bound(std::as_const(logs), event.sequence, {}, &logging::Event::sequence);
        if (position != logs.cend() && position->sequence == event.sequence)
            return;
        logs.insert(position, std::move(event));
    }
    if (logs.size() > 256) {
        logs.removeAt(prefix);
        ++logsOmitted;
    }
}

std::optional<int> DiagnosticSession::requestIndex(int responseIndex) const
{
    if (responseIndex < 0 || responseIndex >= messages.size())
        return std::nullopt;
    const auto &response = messages.at(responseIndex);
    if (!response.responseTo || !response.context || !response.command.endsWith("-RSP"))
        return std::nullopt;
    const auto requestCommand = response.command.chopped(3) + "RQ";
    for (int index = responseIndex - 1; index >= 0; --index) {
        const auto &request = messages.at(index);
        if (request.direction != response.direction && request.id == response.responseTo &&
            request.context == response.context && request.command == requestCommand)
            return index;
    }
    return std::nullopt;
}

QString directionName(Direction direction)
{
    return direction == Direction::Outgoing ? QString("Outgoing") : QString("Incoming");
}

QString statusDescription(quint16 status)
{
    const QString description = status == 0 ? "Success"
        : status == 0xff00 || status == 0xff01 ? "Pending"
        : status == 0xfe00 ? "Cancelled"
        : status == 0xa801 ? "Move destination unknown"
        : status == 0xa900 ? "Dataset does not match the SOP Class"
        : status == 0xa701 || status == 0xa702 ? "Out of resources"
        : (status & 0xf000) == 0xb000 ? "Warning"
        : (status & 0xf000) == 0xa000 || (status & 0xf000) == 0xc000 ? "Failure or refusal"
        : status == 0x0122 ? "SOP class not supported" : "Service-specific status";
    return hexStatus(status) + " · " + description;
}

QString datasetText(const DatasetSnapshot &dataset, bool includeValues)
{
    QString text;
    appendAttributes(text, dataset.attributes, includeValues, 0);
    if (dataset.truncated)
        text += "<capture limit reached>\n";
    return text.isEmpty() ? QString("<empty dataset>\n") : text;
}

QString associationText(const AssociationDetails &a, bool includeIdentities)
{
    QString text = QString("Calling AE: %1\nCalled AE: %2\nConfigured destination: %3\nLocal endpoint: %4\n"
                           "Connected peer: %5\nApplication Context UID (request): %6\n"
                           "Local maximum receive PDU: %7\nPeer maximum receive PDU: %8\n"
                           "Association request: %9\nOutcome: %10\nRejection: %11\nClosure: %12\nConnection: %13\n")
        .arg(show(privateValue(a.callingAe, includeIdentities)), show(privateValue(a.calledAe, includeIdentities)),
             show(privateValue(a.requestedEndpoint, includeIdentities)), show(privateValue(a.localEndpoint, includeIdentities)),
             show(privateValue(a.remoteEndpoint, includeIdentities)), show(uid(a.applicationContext)),
             show(a.localMaxPdu), show(a.peerMaxPdu), !a.requestSent ? "Unavailable" :
             *a.requestSent ? "Sent" : "Send not confirmed")
        .arg(show(a.outcome), show(a.rejection), show(a.closure), show(a.connection));
    for (const auto &context : a.contexts) {
        QStringList proposed;
        for (const auto &syntax : context.proposedSyntaxes)
            if (const auto value = uid(syntax); !value.isEmpty())
                proposed.append(value);
        text += QString("\nContext %1 · %2\n  Abstract syntax: %3\n  Proposed transfer syntaxes: %4\n"
                        "  Proposed role: %5\n  Outcome: %6\n  Accepted transfer syntax: %7\n  Accepted role: %8\n")
            .arg(context.id).arg(show(context.sopClass), show(uid(context.abstractSyntax)),
                show(proposed.join(", ")), show(context.proposedRole), show(context.outcome),
                show(uid(context.acceptedSyntax)), show(context.acceptedRole));
    }
    return text;
}

QString messageText(const DimseRecord &message, bool includeValues)
{
    const auto presence = !message.datasetPresent ? "Unavailable" : *message.datasetPresent ? "Present" : "Absent";
    QString text = QString("%1 · %2 · %3\nMessage ID: %4\nResponse to ID: %5\n"
                           "Presentation context: %6\nSOP Class UID: %7\nStatus: %8\nDataset: %9\n")
        .arg(directionName(message.direction), show(when(message.timestamp)), show(message.command),
             show(message.id), show(message.responseTo), show(message.context), show(uid(message.sopClass)),
             message.status ? statusDescription(*message.status) : QString("Unavailable"), presence);
    if (!message.commandSet.attributes.isEmpty())
        text += "\nCommand set\n" + datasetText(message.commandSet, includeValues);
    if (includeValues && message.dataset)
        text += "\nDataset\n" + datasetText(*message.dataset, includeValues);
    else if (message.dataset && !includeValues)
        text += "\nCaptured dataset omitted from this report.\n";
    else if (message.datasetPresent == true)
        text += "\nDataset values unavailable in this capture.\n";
    if (includeValues && message.statusDetail)
        text += "\nStatus details\n" + datasetText(*message.statusDetail, includeValues);
    return text;
}

QString sessionText(const DiagnosticSession &s, bool includeValues)
{
    QString text = reportTitle + "\nApplication-level protocol diagnostics\n";
    text += QString("Session: %1\nOperation: %2 (%3)\nStart (UTC): %4\nEnd (UTC): %5\n"
                    "Duration: %6 ms\nResult: %7\nDIMSE status: %8\nFailure category: %9\n"
                    "Capture: %10\nSensitive values: %11\n")
        .arg(s.id.toString(QUuid::WithoutBraces), show(s.operation), s.inbound ? "Inbound" : "Outbound",
             show(when(s.started)), show(when(s.ended)))
        .arg(s.durationMs)
        .arg(show(s.result), s.status ? statusDescription(*s.status) : QString("Unavailable"),
             show(s.errorCategory), s.synthetic ? "Synthetic fixture" : "Live operation",
             includeValues ? "Included (review before sharing)" : "Redacted");
    text += "Capture source: " + show(s.captureSource) + u'\n';
    for (auto it = s.counts.cbegin(); it != s.counts.cend(); ++it)
        text += QString("%1: %2\n").arg(it.key()).arg(it.value());
    const auto appendIdentity = [&](const QString &label, const std::optional<DiagnosticSession::Identity> &profile) {
        text += label + ": ";
        if (!profile)
            text += "Unavailable\n";
        else if (!includeValues)
            text += "<redacted>\n";
        else
            text += QString("%1 (ID %2), AE %3, address %4, port %5\n").arg(show(profile->name))
                .arg(profile->id).arg(show(profile->ae), show(profile->address)).arg(profile->port);
    };
    appendIdentity(s.inbound ? "Selected local receiving profile" : "Selected source profile", s.identities.source);
    if (!s.inbound)
        appendIdentity("Selected destination profile", s.identities.destination);
    text += "\nAssociation\n" + associationText(s.association, includeValues);
    text += "\nDIMSE messages\n";
    for (const auto &message : s.messages)
        text += "\n" + messageText(message, includeValues);
    if (s.messages.isEmpty())
        text += "No DIMSE messages were captured.\n";
    if (s.messagesOmitted)
        text += QString("\n%1 older DIMSE messages omitted by the capture limit.\n").arg(s.messagesOmitted);
    text += "\nCorrelated log events (free-text details withheld)\n";
    for (const auto &event : s.logs)
        text += QString("%1 · #%2 · %3 · %4 · %5\n").arg(when(event.timestampUtc)).arg(event.sequence)
            .arg(logging::severityName(event.severity), event.component, logSummary(event.message));
    if (s.logsOmitted)
        text += QString("%1 older log events omitted by the capture limit.\n").arg(s.logsOmitted);
    return text;
}

QJsonObject sessionJson(const DiagnosticSession &s, bool includeValues)
{
    const auto &a = s.association;
    QJsonArray contexts, messages, logs;
    QJsonObject counts;
    for (auto it = s.counts.cbegin(); it != s.counts.cend(); ++it)
        counts.insert(it.key(), it.value());
    for (const auto &context : a.contexts) {
        QJsonArray proposed;
        for (const auto &syntax : context.proposedSyntaxes)
            if (const auto value = uid(syntax); !value.isEmpty())
                proposed.append(value);
        contexts.append(QJsonObject{{"id", context.id}, {"abstractSyntax", available(uid(context.abstractSyntax))},
            {"sopClass", available(context.sopClass)}, {"proposedSyntaxes", proposed},
            {"proposedRole", available(context.proposedRole)}, {"acceptedSyntax", available(uid(context.acceptedSyntax))},
            {"acceptedRole", available(context.acceptedRole)}, {"outcome", available(context.outcome)},
            {"rejectionReason", available(context.rejectionReason)}});
    }
    for (int i = 0; i < s.messages.size(); ++i) {
        const auto &m = s.messages.at(i);
        messages.append(QJsonObject{{"direction", directionName(m.direction)}, {"timestampUtc", available(when(m.timestamp))},
            {"command", m.command}, {"sopClassUid", available(uid(m.sopClass))}, {"messageId", available(m.id)},
            {"responseTo", available(m.responseTo)}, {"contextId", available(m.context)}, {"status", available(m.status)},
            {"requestIndex", available(s.requestIndex(i))}, {"datasetPresent", available(m.datasetPresent)},
            {"datasetCaptured", m.dataset.has_value()}, {"commandSet", datasetJson(m.commandSet, includeValues)},
            {"dataset", includeValues ? datasetJson(m.dataset, true) : QJsonValue(QJsonValue::Null)},
            {"statusDetail", includeValues ? datasetJson(m.statusDetail, true) : QJsonValue(QJsonValue::Null)}});
    }
    for (const auto &event : s.logs)
        logs.append(QJsonObject{{"sequence", qint64(event.sequence)}, {"timestampUtc", available(when(event.timestampUtc))},
            {"severity", logging::severityName(event.severity)}, {"component", event.component},
            {"message", logSummary(event.message)}});
    const auto identity = [includeValues](const std::optional<DiagnosticSession::Identity> &value) -> QJsonValue {
        if (!value)
            return QJsonValue::Null;
        if (!includeValues)
            return QJsonObject{{"redacted", true}};
        return QJsonObject{{"id", value->id}, {"name", available(value->name)}, {"ae", available(value->ae)},
            {"address", available(value->address)}, {"port", value->port}};
    };
    const QJsonObject association{{"callingAe", available(privateValue(a.callingAe, includeValues))},
        {"calledAe", available(privateValue(a.calledAe, includeValues))},
        {"configuredDestination", available(privateValue(a.requestedEndpoint, includeValues))},
        {"localEndpoint", available(privateValue(a.localEndpoint, includeValues))},
        {"remoteEndpoint", available(privateValue(a.remoteEndpoint, includeValues))},
        {"applicationContextUidProposed", available(uid(a.applicationContext))}, {"connection", available(a.connection)},
        {"localMaxPdu", available(a.localMaxPdu)}, {"peerMaxPdu", available(a.peerMaxPdu)},
        {"requestSent", available(a.requestSent)}, {"outcome", available(a.outcome)}, {"rejection", available(a.rejection)},
        {"closure", available(a.closure)}, {"contexts", contexts}};
    return {{"schemaVersion", 2}, {"captureLayer", "application"}, {"captureSource", s.captureSource}, {"sessionId", s.id.toString(QUuid::WithoutBraces)},
        {"operation", s.operation}, {"direction", s.inbound ? "Inbound" : "Outbound"},
        {"startedUtc", available(when(s.started))}, {"endedUtc", available(when(s.ended))},
        {"durationMs", s.durationMs}, {"result", available(s.result)}, {"errorCategory", available(s.errorCategory)},
        {"sourceProfile", identity(s.identities.source)}, {"destinationProfile", identity(s.identities.destination)},
        {"status", available(s.status)}, {"synthetic", s.synthetic}, {"detailedCapture", s.detailedCapture},
        {"attributeValuesIncluded", includeValues}, {"association", association}, {"messages", messages},
        {"messagesTruncated", s.messagesTruncated}, {"messagesOmitted", s.messagesOmitted},
        {"logs", logs}, {"logsOmitted", s.logsOmitted}, {"counts", counts}};
}

QString writeReport(const QString &path, const DiagnosticSession &session, bool json, bool includeValues)
{
    const QFileInfo target(path);
    if (target.isSymLink() || receiveDirectory(target.absolutePath()).isEmpty())
        return "Choose a report file outside Git checkouts and avoid symbolic links.";
    const auto bytes = json ? QJsonDocument(sessionJson(session, includeValues)).toJson()
                            : sessionText(session, includeValues).toUtf8();
    if (bytes.size() > maximumReportBytes)
        return "This report exceeds the 16 MiB report size limit.";
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
        return output.errorString();
    return {};
}

QString readReport(const QString &path, QString &error)
{
    error.clear();
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        error = input.errorString();
        return {};
    }
    const auto bytes = input.read(maximumReportBytes + 1);
    if (input.error() != QFileDevice::NoError) {
        error = input.errorString();
        return {};
    }
    if (bytes.size() > maximumReportBytes) {
        error = "This report exceeds the 16 MiB report size limit.";
        return {};
    }
    if (bytes.startsWith((reportTitle + u'\n').toUtf8()) ||
        bytes.startsWith("DICOM Protocol Inspector \xc2\xb7 Decoded protocol report\n"))
        return QString::fromUtf8(bytes);
    const auto document = QJsonDocument::fromJson(bytes);
    const auto object = document.object();
    const int version = object.value("schemaVersion").toInt();
    if ((version == 1 || version == 2) && !QUuid::fromString(object.value("sessionId").toString()).isNull() &&
        object.value("operation").isString() && object.value("association").isObject() && object.value("messages").isArray() &&
        (version == 1 || (object.value("captureLayer") == "application" && object.value("logs").isArray())))
        return QString::fromUtf8(document.toJson());
    error = "This file is not a supported DingoBench diagnostic report.";
    return {};
}

}  // namespace dingobench::dicom
