// SPDX-License-Identifier: GPL-3.0-only
#include "LogArchive.h"
#include "dingobench/dicom/StorageReceive.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace dingobench::archive {
namespace {
constexpr auto signature = "DingoBench log v1";
constexpr qsizetype maxLine = 100 * 1024;
constexpr qsizetype maxRecords = 2000;
constexpr qint64 maxBytes = 64 * 1024 * 1024;

QJsonObject encode(const logging::Event &event, bool includeDetails)
{
    QJsonObject object{
        {"timestamp", event.timestampUtc.toString(Qt::ISODateWithMs)},
        {"severity", logging::severityName(event.severity)},
        {"component", event.component}, {"operation", event.operation},
        {"message", event.message}, {"synthetic", event.synthetic}
    };
    if (includeDetails && !event.details.isEmpty())
        object.insert("details", event.details);
    return object;
}

// Text uses a tab-separated header and JSON-quoted text fields. Quoting
// preserves arbitrary tabs, newlines, and Unicode on import.
QByteArray textRecord(const QJsonObject &event)
{
    QByteArray line;
    for (const auto *field : {"timestamp", "severity", "component", "operation",
                              "message", "synthetic", "details"}) {
        if (!line.isEmpty())
            line += '\t';
        QJsonArray token{event.value(field).isUndefined() ? QJsonValue() : event.value(field)};
        const auto wrapped = QJsonDocument(token).toJson(QJsonDocument::Compact);
        line += wrapped.mid(1, wrapped.size() - 2);
    }
    return line;
}

QJsonObject parseText(const QByteArray &line)
{
    const auto fields = line.split('\t');
    if (fields.size() != 7)
        return {};
    const auto document = QJsonDocument::fromJson("[" + fields.join(',') + "]");
    const auto values = document.array();
    if (values.size() != 7)
        return {};
    QJsonObject object;
    int index = 0;
    for (const auto *field : {"timestamp", "severity", "component", "operation",
                              "message", "synthetic", "details"})
        object.insert(field, values.at(index++));
    return object;
}
}

QString describe(const logging::Event &event, bool includeDetails)
{
    QString text = QString("%1  %2  %3").arg(event.timestampUtc.toLocalTime().toString(Qt::ISODateWithMs),
        logging::severityName(event.severity), event.component);
    if (!event.operation.isEmpty())
        text += "  [" + event.operation + "]";
    if (event.synthetic)
        text += "  (synthetic)";
    text += "\n" + event.message;
    if (includeDetails && !event.details.isEmpty())
        text += "\n\nTechnical details\n" + event.details;
    return text;
}

bool save(const QString &path, const QList<logging::Event> &events, Format format,
          bool includeDetails, QString &error)
{
    if (events.size() > maxRecords) {
        error = "Select up to 2,000 events to export.";
        return false;
    }
    const QFileInfo target(path);
    if (target.isSymLink() || dicom::receiveDirectory(target.absolutePath()).isEmpty()) {
        error = "Choose a log file outside Git checkouts and avoid symbolic links.";
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    auto write = [&file, &error](const QByteArray &line) {
        if (line.size() >= maxLine - 1 || file.pos() + line.size() > maxBytes) {
            error = "The export exceeds the supported record or file size.";
            return false;
        }
        if (file.write(line) == line.size())
            return true;
        error = file.errorString();
        return false;
    };
    if (!write(format == Format::Text ? QByteArray(signature) +
        "\nTimestamp\tSeverity\tComponent\tOperation\tMessage\tSynthetic\tDetails\n"
        : QJsonDocument(QJsonObject{{"format", signature}}).toJson(QJsonDocument::Compact) + '\n'))
        return false;
    for (const auto &event : events) {
        const auto object = encode(event, includeDetails);
        const auto record = format == Format::Text ? textRecord(object)
            : QJsonDocument(object).toJson(QJsonDocument::Compact);
        if (!write(record + '\n'))
            return false;
    }
    if (file.commit())
        return true;
    error = file.errorString();
    return false;
}

bool load(const QString &path, QList<logging::Event> &events, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    const auto header = file.readLine(maxLine).trimmed();
    const bool plain = header == signature;
    if (file.size() > maxBytes ||
        (!plain && QJsonDocument::fromJson(header).object().value("format") != signature)) {
        error = "Expected a DingoBench log export of at most 64 MiB.";
        return false;
    }
    int lineNumber = 1;
    if (plain) {
        ++lineNumber;
        if (file.readLine(maxLine).trimmed() !=
            "Timestamp\tSeverity\tComponent\tOperation\tMessage\tSynthetic\tDetails") {
            error = "The text export has an invalid column header.";
            return false;
        }
    }
    QList<logging::Event> loaded;
    while (!file.atEnd()) {
        ++lineNumber;
        const auto line = file.readLine(maxLine).trimmed();
        if (line.isEmpty())
            continue;
        const auto object = plain ? parseText(line) : QJsonDocument::fromJson(line).object();
        const auto timestamp = QDateTime::fromString(object.value("timestamp").toString(), Qt::ISODateWithMs);
        const auto level = object.value("severity").toString();
        const int severity = QStringList{"DEBUG", "INFO", "WARNING", "ERROR"}.indexOf(level);
        if (line.size() >= maxLine - 1 || loaded.size() >= maxRecords || !timestamp.isValid()
            || severity < 0 || !object.value("component").isString() || !object.value("message").isString()
            || !object.value("operation").isString() || !object.value("synthetic").isBool()
            || (!object.value("details").isUndefined() && !object.value("details").isNull()
                && !object.value("details").isString())) {
            error = QString("Invalid record or 2,000-event import limit reached at line %1.").arg(lineNumber);
            return false;
        }
        loaded.append({quint64(loaded.size()) + 1, timestamp.toUTC(), logging::Severity(severity),
            object.value("component").toString(), object.value("message").toString(),
            object.value("operation").toString(), object.value("details").toString(),
            object.value("synthetic").toBool()});
    }
    if (file.error() != QFileDevice::NoError) {
        error = file.errorString();
        return false;
    }
    events = std::move(loaded);
    return true;
}

}  // namespace dingobench::archive
