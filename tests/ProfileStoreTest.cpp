// SPDX-License-Identifier: GPL-3.0-only
#include "dingobench/infrastructure/ProfileStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace dingobench::profiles;

namespace {
QByteArray contents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}
}

class ProfileStoreTest final : public QObject {
    Q_OBJECT

private slots:
    void crudAndRestart_data()
    {
        QTest::addColumn<bool>("source");
        QTest::newRow("source") << true;
        QTest::newRow("destination") << false;
    }

    void crudAndRestart()
    {
        QFETCH(bool, source);
        const auto type = kind(source);
        const auto other = kind(!source);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("nested/profiles.json");
        Store store(path);
        const auto original = store.active(type);
        const int otherId = store.active(other).id;
        const auto count = store.entries(type).size();
        Profile created{0, QStringLiteral("Banco — 検査"), "QA LOCAL",
                        source ? "vEthernet (Default Switch)" : "2001:db8::12", 65535,
                        source ? directory.path() : QString()};
        QCOMPARE(store.save(type, created), QString());
        QCOMPARE(store.entries(type).size(), count + 1);
        const int id = store.active(type).id;
        QVERIFY(id != original.id && id != otherId);
        QCOMPARE(store.active(type).name, created.name);
        QCOMPARE(store.active(type).address, created.address);
        QCOMPARE(store.active(type).port, 65535);
        QCOMPARE(store.active(type).directory, created.directory);
        QCOMPARE(store.active(other).id, otherId);
        created.id = id;
        created.name += " edited";
        QCOMPARE(store.save(type, created), QString());
        QCOMPARE(store.active(type).id, id);
        QVERIFY(!store.select(other, id).isEmpty());
        QVERIFY(!store.save(other, created).isEmpty());
        auto duplicate = created;
        duplicate.id = 0;
        duplicate.name += " copy";
        QCOMPARE(store.save(type, duplicate), QString());
        const int copiedId = store.active(type).id;
        QVERIFY(copiedId != id);
        QCOMPARE(store.active(type).ae, created.ae);
        QCOMPARE(store.select(type, id), QString());
        Store restarted(path);
        QCOMPARE(restarted.active(type).id, id);
        QCOMPARE(restarted.active(type).name, created.name);
        QCOMPARE(restarted.active(type).directory, created.directory);
        QCOMPARE(restarted.entries(type).size(), count + 2);
        QCOMPARE(restarted.remove(type, id), QString());
        QCOMPARE(restarted.active(type).id, source ? original.id : copiedId);
        QVERIFY(!restarted.select(type, id).isEmpty());
        QCOMPARE(restarted.remove(type, copiedId), QString());
        QCOMPARE(restarted.entries(type).size(), count);
        Store reopened(path);
        QCOMPARE(reopened.active(type).id, original.id);
        QCOMPARE(reopened.active(other).id, otherId);
        QCOMPARE(reopened.entries(type).size(), count);
        const auto json = QJsonDocument::fromJson(contents(path)).object();
        QCOMPARE(json.value("version").toInt(), 2);
        QVERIFY(!json.contains("credentials"));
        QVERIFY(json.value("destinations").toArray().isEmpty());
    }

    void validation()
    {
        Store store;
        Profile node{0, "Node", "VALID AE", "example.org", 104, {}};
        for (const auto &invalid : {QString(), QString("   "), QString(17, 'A'), QString("A\\B"),
                 QString("A\tB"), QString(QChar(0x7f)), QStringLiteral("DÍCOM")}) {
            node.ae = invalid;
            QVERIFY2(!store.save(Kind::Destination, node).isEmpty(), qPrintable(invalid));
        }
        node.ae = "0123456789ABCDEF";
        for (int port : {-1, 0, 65536}) {
            node.port = port;
            QVERIFY(!store.save(Kind::Destination, node).isEmpty());
        }
        node.port = 1;
        for (const auto *invalid : {"", "http://example.org", "host:104", "127.0.0.999", "127.1",
                 "2001:::1", "[::1]:104", "bad host", "-host", "a..b", "abc_", "user@host"}) {
            node.address = invalid;
            QVERIFY2(!store.save(Kind::Destination, node).isEmpty(), invalid);
        }
        QVERIFY(store.entries(Kind::Destination).isEmpty());
        for (const auto *valid : {"localhost", "example.org.", "192.0.2.23", "::1", "2001:db8::1"}) {
            node.address = valid;
            QVERIFY2(Store::validate(Kind::Destination, node).isEmpty(), valid);
        }
        node.address = "example.org";
        QVERIFY(store.save(Kind::Destination, node).isEmpty());
        node.name = "node";
        QVERIFY(!store.save(Kind::Destination, node).isEmpty());
        node.address = "http://invalid";
        QVERIFY(!Store::validate(Kind::Source, node).isEmpty());
        node.address.clear();
        QVERIFY(Store::validate(Kind::Source, node).isEmpty());
    }

    void damagedAndUnsupportedConfiguration()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("profiles.json");
        const QByteArray damaged = "{damaged json";
        QVERIFY(writeFile(path, damaged));
        Store fallback(path);
        QVERIFY(!fallback.loadWarning().isEmpty());
        QCOMPARE(contents(path), damaged);
        QCOMPARE(fallback.active(Kind::Source).ae, QString("DINGOBENCH"));
        QCOMPARE(fallback.save(Kind::Source, {0, "New source", "NEW_SCU", {}, 11113, {}}), QString());
        QCOMPARE(fallback.save(Kind::Destination, {0, "New peer", "NEW_SCP", "localhost", 104, {}}), QString());
        const auto backups = QDir(directory.path()).entryList({"profiles.json.corrupt-*"}, QDir::Files);
        QCOMPARE(backups.size(), 1);
        QCOMPARE(contents(directory.filePath(backups.first())), damaged);
        Store recovered(path);
        QVERIFY(recovered.loadWarning().isEmpty());
        QCOMPARE(recovered.active(Kind::Source).name, QString("New source"));
        auto invalidSelection = QJsonDocument::fromJson(contents(path)).object();
        invalidSelection["activeSource"] = 4; // Valid destination ID, invalid source selection.
        QVERIFY(writeFile(path, QJsonDocument(invalidSelection).toJson()));
        Store corrected(path);
        QCOMPARE(corrected.active(Kind::Source).id, 1);
        auto destinations = invalidSelection.value("destinations").toArray();
        auto node = destinations.first().toObject();
        node["port"] = 65536;
        destinations[0] = node;
        invalidSelection["destinations"] = destinations;
        QVERIFY(writeFile(path, QJsonDocument(invalidSelection).toJson()));
        Store invalidProfile(path);
        QVERIFY(!invalidProfile.loadWarning().isEmpty());
        QCOMPARE(invalidProfile.active(Kind::Destination).id, 0);
        const QByteArray newer = R"({"version":99})";
        QVERIFY(writeFile(path, newer));
        Store unsupported(path);
        QVERIFY(!unsupported.loadWarning().isEmpty());
        QVERIFY(!unsupported.save(Kind::Source, {0, "Source", "BENCH", {}, 11113, {}}).isEmpty());
        QCOMPARE(contents(path), newer);
    }

    void legacyFixturesAreRetiredOnLoad()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("profiles.json");
        const QByteArray legacy = R"({
          "version":1, "nextId":9, "activeSource":2, "activeDestination":3,
          "sources":[
            {"id":1,"name":"Bench Workstation","aeTitle":"DINGOBENCH","bind":"","port":11112,"receivedDirectory":""},
            {"id":2,"name":"Receive Fixture","aeTitle":"BENCH_SCP","bind":"127.0.0.1","port":11113,"receivedDirectory":""}],
          "destinations":[
            {"id":3,"name":"Imaging Lab","aeTitle":"LAB_ARCHIVE","host":"192.0.2.24","port":104},
            {"id":4,"name":"QA Gateway","aeTitle":"QA_GATEWAY","host":"198.51.100.18","port":1104},
            {"id":5,"name":"Worklist Fixture","aeTitle":"LAB_MWL","host":"203.0.113.12","port":11114},
            {"id":8,"name":"User peer","aeTitle":"PACS","host":"localhost","port":104}]})";
        QVERIFY(writeFile(path, legacy));
        Store store(path);
        QCOMPARE(contents(path), legacy);
        QCOMPARE(store.entries(Kind::Source).size(), 1);
        QCOMPARE(store.active(Kind::Source).id, 1);
        QCOMPARE(store.entries(Kind::Destination).size(), 2);
        QCOMPARE(store.active(Kind::Destination).id, 4);
        QCOMPARE(store.active(Kind::Destination).port, 1104);
        QCOMPARE(store.select(Kind::Destination, 8), QString());
        auto json = QJsonDocument::fromJson(contents(path)).object();
        QCOMPARE(json.value("version").toInt(), 2);
        QCOMPARE(json.value("nextId").toInt(), 9);
        Store restarted(path);
        QCOMPARE(restarted.active(Kind::Destination).id, 8);
        QCOMPARE(restarted.remove(Kind::Destination, 4), QString());
        QCOMPARE(restarted.remove(Kind::Destination, 8), QString());
        Store empty(path);
        QVERIFY(empty.loadWarning().isEmpty());
        QCOMPARE(empty.active(Kind::Destination).id, 0);
        QVERIFY(empty.entries(Kind::Destination).isEmpty());
    }

    void atomicSaveRecovery()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("profiles.json");
        Store store(path);
        QCOMPARE(store.save(Kind::Destination, {0, "Peer A", "PEER_A", "localhost", 104, {}}), QString());
        const int first = store.active(Kind::Destination).id;
        QCOMPARE(store.save(Kind::Destination, {0, "Peer B", "PEER_B", "localhost", 105, {}}), QString());
        const int second = store.active(Kind::Destination).id;
        QCOMPARE(store.select(Kind::Destination, first), QString());
        const auto baseline = contents(path);
        QVERIFY(!baseline.isEmpty());
        {
            QSaveFile interrupted(path);
            QVERIFY(interrupted.open(QIODevice::WriteOnly));
            QVERIFY(interrupted.write("incomplete replacement") > 0);
        }
        QCOMPARE(contents(path), baseline);
        {
            QLockFile lock(path + ".lock");
            QVERIFY(lock.tryLock(0));
            QVERIFY(!store.select(Kind::Destination, second).isEmpty());
            QCOMPARE(contents(path), baseline);
            QCOMPARE(store.active(Kind::Destination).id, first);
        }
#ifdef Q_OS_WIN
        {
            const auto nativePath = QDir::toNativeSeparators(path);
            HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            QVERIFY(handle != INVALID_HANDLE_VALUE);
            const auto release = qScopeGuard([handle] { CloseHandle(handle); });
            QVERIFY(!store.select(Kind::Destination, second).isEmpty());
            QCOMPARE(contents(path), baseline);
            QCOMPARE(store.active(Kind::Destination).id, first);
        }
#endif
        Store concurrent(path);
        QCOMPARE(store.select(Kind::Destination, second), QString());
        QVERIFY(!concurrent.select(Kind::Destination, second).isEmpty());
        QCOMPARE(Store(path).active(Kind::Destination).id, second);
    }
};

QTEST_GUILESS_MAIN(ProfileStoreTest)
#include "ProfileStoreTest.moc"
