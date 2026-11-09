// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace dingobench::profiles {

enum class Kind { Source, Destination };
constexpr Kind kind(bool source) { return source ? Kind::Source : Kind::Destination; }

bool validAeTitle(const QString &value);

struct Profile {
    int id = 0;
    QString name;
    QString ae;
    QString address;
    int port = 0;
    QString directory;
};

// Owned by the UI thread. An empty path creates an isolated, in-memory store.
class Store {
public:
    explicit Store(QString path = {});
    const QList<Profile> &entries(Kind kind) const;
    const Profile &active(Kind kind) const;
    const QString &loadWarning() const { return m_warning; }

    // ID zero creates a profile and selects it. Editing preserves its ID and
    // the existing selection. An empty return value means success.
    QString save(Kind kind, Profile profile);
    QString remove(Kind kind, int id);
    QString select(Kind kind, int id);
    static QString validate(Kind kind, const Profile &profile);

private:
    struct State {
        QList<Profile> sources;
        QList<Profile> destinations;
        int sourceId;
        int destinationId;
        int nextId;
    };

    QString commit(State candidate);
    void load();

    QString m_path;
    State m_state;
    QByteArray m_onDisk;
    QString m_warning;
    bool m_exists = false;
    bool m_corrupt = false;
    bool m_readOnly = false;
};

}  // namespace dingobench::profiles
