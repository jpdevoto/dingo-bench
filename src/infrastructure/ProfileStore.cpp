// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/infrastructure/ProfileStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <limits>

namespace dingobench::profiles {
namespace {

constexpr int schemaVersion = 2;
constexpr int legacyVersion = 1;
constexpr int firstUserId = 6;

bool unchangedFixture(const Profile &profile, Kind kind)
{
    const auto matches = [&](int id, const QString &name, const QString &ae, const QString &address, int port) {
        return profile.id == id && profile.name == name && profile.ae == ae &&
            profile.address == address && profile.port == port && profile.directory.isEmpty();
    };
    if (kind == Kind::Source)
        return matches(2, "Receive Fixture", "BENCH_SCP", "127.0.0.1", 11113);
    return matches(3, "Imaging Lab", "LAB_ARCHIVE", "192.0.2.24", 104) ||
        matches(4, "QA Gateway", "QA_GATEWAY", "198.51.100.18", 11112) ||
        matches(5, "Worklist Fixture", "LAB_MWL", "203.0.113.12", 11114);
}
constexpr qint64 maxFileSize = 4 * 1024 * 1024;

bool ipLiteral(const QString &value)
{
    QHostAddress address;
    if (!address.setAddress(value))
        return false;
    // Qt also accepts shorthand and non-decimal IPv4. Require dotted decimal.
    if (address.protocol() == QAbstractSocket::IPv4Protocol)
        return address.toString() == value;
    static const QRegularExpression scope(R"(^[A-Za-z0-9_.-]+$)");
    return !value.contains('%') || scope.match(address.scopeId()).hasMatch();
}

bool hostname(const QString &value)
{
    static const QRegularExpression numeric(R"(^[0-9.]+$)");
    static const QRegularExpression disallowed(R"([\s:/\\@%?#\[\]])");
    if (value.isEmpty() || numeric.match(value).hasMatch() || disallowed.match(value).hasMatch())
        return false;
    auto ascii = QUrl::toAce(value);
    if (ascii.endsWith('.'))
        ascii.chop(1);
    if (ascii.isEmpty() || ascii.size() > 253)
        return false;
    static const QRegularExpression label(R"(^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$)");
    for (const auto &part : ascii.split('.'))
        if (!label.match(QString::fromLatin1(part)).hasMatch())
            return false;
    return true;
}

QJsonArray encode(const QList<Profile> &profiles, Kind kind)
{
    QJsonArray array;
    for (const auto &entry : profiles) {
        QJsonObject object{{"id", entry.id}, {"name", entry.name}, {"aeTitle", entry.ae}, {"port", entry.port}};
        if (kind == Kind::Source) {
            object.insert("bind", entry.address);
            object.insert("receivedDirectory", entry.directory);
        } else {
            object.insert("host", entry.address);
        }
        array.append(object);
    }
    return array;
}

bool decode(const QJsonValue &value, Kind kind, QList<Profile> &profiles, QSet<int> &ids)
{
    if (!value.isArray() || (kind == Kind::Source && value.toArray().isEmpty()))
        return false;
    QSet<QString> names;
    for (const auto &item : value.toArray()) {
        const auto object = item.toObject();
        const auto address = object.value(kind == Kind::Source ? "bind" : "host");
        const auto directory = object.value("receivedDirectory");
        const auto name = object.value("name");
        const auto ae = object.value("aeTitle");
        if (!name.isString() || !ae.isString() || !address.isString() ||
            (kind == Kind::Source && !directory.isString()))
            return false;
        Profile profile{object.value("id").toInt(), name.toString(), ae.toString(),
                        address.toString(), object.value("port").toInt(), directory.toString()};
        const auto nameKey = profile.name.trimmed().toCaseFolded();
        if (profile.id <= 0 || ids.contains(profile.id) || names.contains(nameKey) ||
            !Store::validate(kind, profile).isEmpty())
            return false;
        ids.insert(profile.id);
        names.insert(nameKey);
        profiles.append(std::move(profile));
    }
    return true;
}

QString writeAtomic(const QString &path, const QByteArray &bytes)
{
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
        return output.errorString();
    return {};
}

}  // namespace

Store::Store(QString path)
    : m_path(std::move(path)),
      m_state{{{1, "Bench Workstation", "DINGOBENCH", "", 11112, ""}}, {}, 1, 0, firstUserId}
{
    if (!m_path.isEmpty())
        load();
}

bool validAeTitle(const QString &value)
{
    return !value.trimmed().isEmpty() && value.size() <= 16 &&
        std::ranges::none_of(value, [](QChar ch) {
            return ch.unicode() < 0x20 || ch.unicode() > 0x7e || ch == '\\';
        });
}

QString Store::validate(Kind kind, const Profile &profile)
{
    if (profile.name.trimmed().isEmpty())
        return "Enter a profile name.";
    if (profile.name.size() > 48)
        return "Use a profile name of 48 characters or fewer.";
    if (!validAeTitle(profile.ae))
        return "Enter an AE title of 1–16 printable ASCII characters, without backslashes.";
    if (profile.port < 1 || profile.port > 65535)
        return "Enter a TCP port between 1 and 65535.";
    const auto address = profile.address.trimmed();
    if (std::ranges::any_of(profile.address, [](QChar ch) { return ch.category() == QChar::Other_Control; }))
        return "Enter an address without control characters.";
    if (kind == Kind::Destination && !ipLiteral(address) && !hostname(address))
        return "Enter a valid hostname, IPv4 address, or IPv6 address without a port or URL scheme.";
    if (kind == Kind::Source && !address.isEmpty() && !ipLiteral(address)) {
        static const QRegularExpression interfaceName(R"(^[\p{L}\p{M}\p{N}_. (){}+-]{1,128}$)");
        static const QRegularExpression malformedIp(R"(^[0-9.]+$)");
        if (!interfaceName.match(address).hasMatch() || malformedIp.match(address).hasMatch())
            return "Enter a local interface name or a valid IPv4 or IPv6 address.";
    }
    if (kind == Kind::Source && profile.directory.contains(QChar::Null))
        return "Enter a valid received-file directory.";
    return {};
}

const QList<Profile> &Store::entries(Kind kind) const
{
    return kind == Kind::Source ? m_state.sources : m_state.destinations;
}

const Profile &Store::active(Kind kind) const
{
    const auto &list = entries(kind);
    const int id = kind == Kind::Source ? m_state.sourceId : m_state.destinationId;
    const auto match = std::ranges::find(list, id, &Profile::id);
    static const Profile none;
    return match == list.end() ? none : *match;
}

void Store::load()
{
    QFile file(m_path);
    m_exists = file.exists();
    if (!m_exists)
        return;
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxFileSize) {
        m_readOnly = true;
        m_warning = "Profile configuration could not be read. The file was left untouched and defaults are available.";
        return;
    }
    m_onDisk = file.read(maxFileSize + 1);
    if (file.error() != QFileDevice::NoError || m_onDisk.size() > maxFileSize) {
        m_readOnly = true;
        m_warning = "Profile configuration could not be read. The file was left untouched and defaults are available.";
        return;
    }
    const auto document = QJsonDocument::fromJson(m_onDisk);
    const auto object = document.object();
    const auto version = object.value("version").toDouble();
    if (object.value("version").isDouble() && version != schemaVersion && version != legacyVersion) {
        m_readOnly = true;
        m_warning = "This profile file uses an unsupported version. It was left untouched and defaults are available.";
        return;
    }
    State candidate{};
    QSet<int> ids;
    if ((version != schemaVersion && version != legacyVersion) ||
        !decode(object.value("sources"), Kind::Source, candidate.sources, ids) ||
        !decode(object.value("destinations"), Kind::Destination, candidate.destinations, ids) ||
        (candidate.nextId = object.value("nextId").toInt()) <= *std::max_element(ids.cbegin(), ids.cend())) {
        m_corrupt = true;
        m_warning = "The profile configuration is damaged. Defaults are loaded, and the original will be backed up before the first successful change.";
        return;
    }
    if (version == legacyVersion) {
        // Keep a valid source even in a legacy file containing only the receive fixture.
        const auto removed = candidate.sources.removeIf([](const Profile &profile) {
            return unchangedFixture(profile, Kind::Source);
        });
        if (removed && candidate.sources.isEmpty()) {
            auto source = m_state.sources.first();
            if (ids.contains(source.id))
                source.id = 2;
            candidate.sources.append(std::move(source));
        }
        candidate.destinations.removeIf([](const Profile &profile) {
            return unchangedFixture(profile, Kind::Destination);
        });
    }
    candidate.sourceId = object.value("activeSource").toInt();
    candidate.destinationId = object.value("activeDestination").toInt();
    if (std::ranges::find(candidate.sources, candidate.sourceId, &Profile::id) == candidate.sources.end())
        candidate.sourceId = candidate.sources.first().id;
    if (std::ranges::find(candidate.destinations, candidate.destinationId, &Profile::id) == candidate.destinations.end())
        candidate.destinationId = candidate.destinations.isEmpty() ? 0 : candidate.destinations.first().id;
    m_state = std::move(candidate);
}

QString Store::commit(State candidate)
{
    if (!m_path.isEmpty()) {
        if (m_readOnly)
            return "The local profile configuration is protected. Resolve the configuration problem before saving.";
        if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
            return "Could not create the local profile directory.";
        QLockFile lock(m_path + ".lock");
        if (!lock.tryLock(0))
            return "Another process is writing the profile file, or the profile directory is not writable.";
        QFile original(m_path);
        if (original.exists() != m_exists || (m_exists &&
            (!original.open(QIODevice::ReadOnly) || original.size() != m_onDisk.size()
             || original.read(maxFileSize + 1) != m_onDisk)))
            return "The profile file changed outside this window. Restart DingoBench before making changes.";
        original.close();
        const auto bytes = QJsonDocument(QJsonObject{
            {"version", schemaVersion}, {"nextId", candidate.nextId},
            {"activeSource", candidate.sourceId}, {"activeDestination", candidate.destinationId},
            {"sources", encode(candidate.sources, Kind::Source)},
            {"destinations", encode(candidate.destinations, Kind::Destination)}
        }).toJson();
        if (bytes.size() > maxFileSize)
            return "The profile configuration is too large.";
        if (m_corrupt) {
            const auto backup = m_path + ".corrupt-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto error = writeAtomic(backup, m_onDisk);
            if (!error.isEmpty())
                return "Could not back up the damaged profile file: " + error;
            m_corrupt = false; // A verified copy now exists even if replacing the original fails.
        }
        const auto error = writeAtomic(m_path, bytes);
        if (!error.isEmpty())
            return "Could not save the profile configuration: " + error;
        m_onDisk = bytes;
        m_exists = true;
        m_warning.clear();
    }
    m_state = std::move(candidate);
    return {};
}

QString Store::save(Kind kind, Profile profile)
{
    if (const auto error = validate(kind, profile); !error.isEmpty())
        return error;
    profile.name = profile.name.trimmed();
    profile.ae = profile.ae.trimmed();
    profile.address = profile.address.trimmed();
    profile.directory = kind == Kind::Source ? profile.directory.trimmed() : QString();
    State candidate = m_state;
    auto &list = kind == Kind::Source ? candidate.sources : candidate.destinations;
    if (std::ranges::any_of(list, [&](const auto &entry) {
        return entry.id != profile.id && entry.name.compare(profile.name, Qt::CaseInsensitive) == 0;
    }))
        return "A profile with this name already exists.";
    if (profile.id == 0) {
        if (candidate.nextId == std::numeric_limits<int>::max())
            return "The profile ID limit has been reached.";
        profile.id = candidate.nextId++;
        (kind == Kind::Source ? candidate.sourceId : candidate.destinationId) = profile.id;
        list.append(std::move(profile));
    } else {
        const auto entry = std::ranges::find(list, profile.id, &Profile::id);
        if (entry == list.end())
            return "This profile no longer exists.";
        *entry = std::move(profile);
    }
    return commit(std::move(candidate));
}

QString Store::remove(Kind kind, int id)
{
    State candidate = m_state;
    auto &list = kind == Kind::Source ? candidate.sources : candidate.destinations;
    if (kind == Kind::Source && list.size() == 1)
        return "Keep at least one source profile.";
    if (!list.removeIf([id](const Profile &entry) { return entry.id == id; }))
        return "This profile no longer exists.";
    int &active = kind == Kind::Source ? candidate.sourceId : candidate.destinationId;
    if (active == id)
        active = list.isEmpty() ? 0 : list.first().id;
    return commit(std::move(candidate));
}

QString Store::select(Kind kind, int id)
{
    const auto &list = entries(kind);
    if (std::ranges::find(list, id, &Profile::id) == list.end())
        return "This profile no longer exists.";
    if (active(kind).id == id)
        return {};
    State candidate = m_state;
    (kind == Kind::Source ? candidate.sourceId : candidate.destinationId) = id;
    return commit(std::move(candidate));
}

}  // namespace dingobench::profiles
