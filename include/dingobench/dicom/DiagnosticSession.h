// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QUuid>

#include <optional>

#include "dingobench/infrastructure/Logger.h"
#include "dingobench/infrastructure/ProfileStore.h"

class DcmItem;

namespace dingobench::dicom {

struct Attribute {
    QString tag, name, vr, value;
    int vm = 0;
    enum Kind { Value, Empty, Binary, Sequence, Item, Unavailable } kind = Value;
    QList<Attribute> children;
    std::optional<quint32> encodedLength;
};

struct DatasetSnapshot {
    QList<Attribute> attributes;
    bool truncated = false;
    bool decodingIssue = false;
};

struct PresentationContext {
    int id = 0;
    QString abstractSyntax, sopClass;
    QStringList proposedSyntaxes;
    QString proposedRole, acceptedSyntax, acceptedRole, outcome;
    std::optional<int> rejectionReason;
    bool requestStorageScp = false;
};

struct AssociationDetails {
    QString callingAe, calledAe, requestedEndpoint, localEndpoint, remoteEndpoint;
    QString applicationContext, outcome, rejection, closure;
    std::optional<quint32> localMaxPdu, peerMaxPdu;
    std::optional<bool> requestSent;
    QString connection;
    QList<PresentationContext> contexts;
};

enum class Direction { Outgoing, Incoming };

struct DimseRecord {
    Direction direction = Direction::Outgoing;
    QDateTime timestamp;
    QString command, sopClass;
    std::optional<quint16> id, responseTo, context, status;
    std::optional<bool> datasetPresent;
    DatasetSnapshot commandSet;
    std::optional<DatasetSnapshot> dataset, statusDetail;
};

struct DiagnosticSession {
    struct Identity {
        Identity(const profiles::Profile &profile)
            : id(profile.id), name(profile.name), ae(profile.ae), address(profile.address), port(profile.port) {}
        int id;
        QString name, ae, address;
        int port;
    };
    struct Identities {
        std::optional<Identity> source, destination;
    };

    QUuid id;
    QString operation, result, error, technicalError, errorCategory;
    QString captureSource = "Decoded protocol";
    QMap<QString, qint64> counts;
    void finish(const QDateTime &end, QString result, QString closure);
    Identities identities;
    bool inbound = false;
    QDateTime started, ended;
    qint64 durationMs = 0;
    bool synthetic = false;
    bool detailedCapture = false;
    AssociationDetails association;
    std::optional<quint16> status;
    QList<DimseRecord> messages;
    bool messagesTruncated = false;

    qsizetype messagesOmitted = 0;
    QList<logging::Event> logs;
    qsizetype logsOmitted = 0;

    void addLog(logging::Event event);
    // Retain the initial commands and the newest traffic, including the terminal response.
    void record(DimseRecord message);
    void captureCommand(DcmItem &command, Direction direction, QDateTime timestamp, quint16 context,
                        DcmItem *dataset = nullptr, DcmItem *statusDetail = nullptr);
    std::optional<int> requestIndex(int responseIndex) const;
};

// Call on the operation worker. Snapshots own display data, never toolkit objects.
DatasetSnapshot snapshotDataset(DcmItem &dataset);
DimseRecord snapshotCommand(DcmItem &command, Direction direction, QDateTime timestamp,
                            quint16 context);
QString statusDescription(quint16 status);
QString directionName(Direction direction);
QString datasetText(const DatasetSnapshot &dataset, bool includeValues = false);
QJsonObject sessionJson(const DiagnosticSession &session, bool includeValues = false);
QString sessionText(const DiagnosticSession &session, bool includeValues = false);
QString associationText(const AssociationDetails &association, bool includeIdentities = false);
QString writeReport(const QString &path, const DiagnosticSession &session, bool json, bool includeValues = false);
QString readReport(const QString &path, QString &error);
QString messageText(const DimseRecord &message, bool includeValues = false);

}  // namespace dingobench::dicom
