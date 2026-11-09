// SPDX-License-Identifier: GPL-3.0-only
#include "MainWindow.h"
#include "LogArchive.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>

#include <algorithm>
#include <filesystem>
#include <thread>

using dingobench::MainWindow;
using dingobench::logging::Severity;

class WorkspaceTest final : public QObject {
    Q_OBJECT

private:
    template <typename Widget>
    static Widget *control(QWidget &window, const char *name)
    {
        auto *found = window.findChild<Widget *>(QLatin1String(name));
        Q_ASSERT(found);
        return found;
    }

    static void navigate(MainWindow &window, int index)
    {
        auto *button = control<QPushButton>(window, qPrintable(QString("nav%1").arg(index)));
        button->setFocus();
        button->click();
    }

    static void chooseFile(QWidget &window, const char *name, const QString &path, bool directory)
    {
        QTimer action;
        action.setInterval(30);
        connect(&action, &QTimer::timeout, &window, [&] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            action.stop();
            if ((dialog->fileMode() == QFileDialog::Directory) != directory) {
                dialog->reject();
                return;
            }
            dialog->setDirectory(QFileInfo(path).absolutePath());
            auto *name = dialog->findChild<QLineEdit *>("fileNameEdit");
            if (!name) {
                dialog->reject();
                return;
            }
            name->setText(QDir::toNativeSeparators(path));
            static_cast<QDialog *>(dialog)->accept();
            if (dialog->isVisible())
                dialog->reject();
        });
        action.start();
        if (auto *actionButton = window.findChild<QAction *>(QLatin1String(name)))
            actionButton->trigger();
        else
            control<QPushButton>(window, name)->click();
    }

    static void saveProfile(QWidget &window, const char *command, const QString &name)
    {
        QTimer action;
        action.setInterval(30);
        connect(&action, &QTimer::timeout, &window, [&] {
            auto *dialog = QApplication::activeModalWidget();
            if (!dialog)
                return;
            action.stop();
            control<QLineEdit>(*dialog, "profileName")->setText(name);
            control<QLineEdit>(*dialog, "profileAe")->setText("QA_PROFILE");
            control<QLineEdit>(*dialog, "profileAddress")->setText("192.0.2.72");
            control<QPushButton>(*dialog, "saveProfile")->click();
        });
        action.start();
        control<QPushButton>(window, command)->click();
    }

    static void deleteProfile(QWidget &window, const char *command, bool confirm)
    {
        QTimer action;
        action.setInterval(30);
        connect(&action, &QTimer::timeout, &window, [&] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != "confirmProfileDelete")
                return;
            action.stop();
            dialog->button(confirm ? QMessageBox::Yes : QMessageBox::Cancel)->click();
        });
        action.start();
        control<QPushButton>(window, command)->click();
    }

    QTemporaryDir m_settings;
    dingobench::logging::Logger m_logger;

private slots:
    void initTestCase()
    {
        QVERIFY(m_settings.isValid());
        QCoreApplication::setOrganizationName("DingoBenchTest");
        QCoreApplication::setApplicationName("WorkspaceTest");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        dingobench::ui::installTheme(*qApp);
    }

    void init()
    {
        QSettings().clear();
        m_logger.setCapacity(2000);
        m_logger.clear();
    }

    void dependencyDiagnostics()
    {
        MainWindow window(m_logger);
        QString text;
        bool aboutContentValid = false;
        bool captured = true;
        QTimer::singleShot(0, &window, [&] {
            auto *about = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!about)
                return;
            const auto *wordmark = about->findChild<QLabel *>("aboutWordmark");
            const auto *mascot = about->findChild<QLabel *>("aboutMascot");
            const auto *links = about->findChild<QLabel *>("aboutLinks");
            aboutContentValid = wordmark && !wordmark->pixmap().isNull()
                && mascot && !mascot->pixmap().isNull() && links && links->openExternalLinks()
                && links->text().contains("href='https://dingodicom.com/'")
                && links->text().contains("href='https://github.com/jpdevoto/dingo-bench'");
            const auto target = qEnvironmentVariable("DINGOBENCH_CAPTURE_DIR");
            if (!target.isEmpty())
                captured = QDir().mkpath(target) && about->grab().save(QDir(target).filePath("about.png"));
            auto *button = about->findChild<QPushButton *>("showDependencies");
            if (button) {
                QTimer::singleShot(0, about, [&] {
                    auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                    if (!dialog || dialog == about)
                        return;
                    text = dialog->text();
                    dialog->done(QMessageBox::Close);
                });
                button->click();
            }
            about->accept();
        });
        control<QPushButton>(window, "aboutButton")->click();
        QVERIFY(aboutContentValid);
        QVERIFY(captured);
        QVERIFY(text.contains("DCMTK 3.7.1"));
        QVERIFY(text.contains("dcmnet, dcmdata, ofstd, oflog"));
        QVERIFY(text.contains("Association parameter probe: passed"));
        QVERIFY(text.contains(QString::fromLatin1(qVersion())));
    }

    void navigationProfilesAndFreshStart()
    {
        MainWindow window(m_logger);
        window.show();
        auto *stack = control<QStackedWidget>(window, "workspaces");
        QCOMPARE(stack->count(), 9);
        QElapsedTimer clock;
        clock.start();
        for (int index = 0; index < stack->count(); ++index) {
            navigate(window, index);
            QApplication::processEvents();
            QCOMPARE(stack->currentIndex(), index);
            const auto *nav = control<QPushButton>(window, qPrintable(QString("nav%1").arg(index)));
            QVERIFY(nav->isChecked());
            QVERIFY(!nav->icon().pixmap(18, 18, QIcon::Normal, QIcon::On).isNull());
        }
        qInfo() << "Nine workspace switches, including paint:" << clock.elapsed() << "ms";
        QVERIFY2(clock.elapsed() < 2500, "Nine workspace switches exceeded 2.5 seconds.");
        auto *source = control<QComboBox>(window, "sourceProfile");
        auto *destination = control<QComboBox>(window, "destinationProfile");
        QCOMPARE(source->count(), 1);
        QCOMPARE(source->currentText(), QString("Bench Workstation"));
        QCOMPARE(destination->count(), 0);
        QVERIFY(!window.findChild<QWidget *>("previewBar"));
        QVERIFY(!window.findChild<QComboBox *>("previewScenario"));
        QVERIFY(m_logger.snapshot().isEmpty());
        navigate(window, 0);
        QCOMPARE(control<QTableWidget>(window, "recentOperations")->rowCount(), 0);
        QVERIFY(control<QLabel>(window, "recentOperationsEmpty")->isVisible());
        navigate(window, 1);
        QVERIFY(!control<QPushButton>(window, "sendEcho")->isEnabled());
        navigate(window, 4);
        QVERIFY(!control<QPushButton>(window, "sendFind")->isEnabled());
        navigate(window, 5);
        QVERIFY(!control<QPushButton>(window, "sendWorklist")->isEnabled());
        navigate(window, 0);
        saveProfile(window, "dashboardCreateDestination", "QA archive");
        QCOMPARE(destination->count(), 1);
        QCOMPARE(destination->currentText(), QString("QA archive"));
        QVERIFY(control<QPushButton>(window, "sendEcho")->isEnabled());
        QVERIFY(control<QPushButton>(window, "sendFind")->isEnabled());
        QVERIFY(control<QPushButton>(window, "sendWorklist")->isEnabled());
        navigate(window, 5);
        control<QLineEdit>(window, "worklistDate")->setText("20260230");
        control<QPushButton>(window, "sendWorklist")->click();
        QVERIFY(control<QLabel>(window, "worklistSummary")->text().contains("valid scheduled date"));
        QCOMPARE(control<QTableWidget>(window, "worklistResults")->rowCount(), 0);
        navigate(window, 4);
        control<QComboBox>(window, "queryLevel")->setCurrentIndex(1);
        QVERIFY(!control<QLineEdit>(window, "queryPatientId")->isEnabled());
        control<QPushButton>(window, "sendFind")->click();
        QVERIFY(control<QLabel>(window, "querySummary")->text().contains("Study Instance UID is required"));
        control<QComboBox>(window, "queryLevel")->setCurrentIndex(2);
        control<QLineEdit>(window, "queryStudyUid")->setText("1.2.3");
        control<QPushButton>(window, "sendFind")->click();
        QVERIFY(control<QLabel>(window, "querySummary")->text().contains("Series Instance UID is required"));
        QCOMPARE(control<QTableWidget>(window, "queryResults")->rowCount(), 0);
        window.close();
    }

    void persistenceAndSplitter()
    {
        QSize size;
        int consoleHeight = 0;
        {
            MainWindow window(m_logger);
            const QSize desktop = QApplication::primaryScreen()->availableGeometry().size();
            window.resize(qMin(1000, desktop.width() - 8), qMin(680, desktop.height() - 8));
            window.show();
            QApplication::processEvents();
            navigate(window, 6);
            if (control<QWidget>(window, "console")->isHidden())
                control<QPushButton>(window, "toggleConsole")->click();
            auto *splitter = control<QSplitter>(window, "workspaceSplitter");
            const int old = splitter->sizes().at(1);
            splitter->setSizes({220, 290});
            QApplication::processEvents();
            consoleHeight = splitter->sizes().at(1);
            QVERIFY(consoleHeight != old);
            auto *toggle = control<QPushButton>(window, "toggleConsole");
            toggle->click();
            QVERIFY(control<QWidget>(window, "console")->isHidden());
            toggle->click();
            QVERIFY(!control<QWidget>(window, "console")->isHidden());
            QVERIFY(qAbs(consoleHeight - splitter->sizes().at(1)) <= 6);
            size = window.size();
            window.close();
        }
        MainWindow window(m_logger);
        window.show();
        QApplication::processEvents();
        QCOMPARE(control<QStackedWidget>(window, "workspaces")->currentIndex(), 6);
        QCOMPARE(window.size(), size);
        QVERIFY(qAbs(consoleHeight - control<QSplitter>(window, "workspaceSplitter")->sizes().at(1)) <= 6);
        window.close();
    }

    void liveVerificationCancellationAndLogs()
    {
        QTcpServer peer;
        QVERIFY(peer.listen(QHostAddress::LocalHost));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("profiles.json");
        dingobench::profiles::Store profiles(path);
        QVERIFY(profiles.save(dingobench::profiles::Kind::Destination,
            {0, "Loopback fixture", "LOCAL_SCP", "127.0.0.1", peer.serverPort(), {}}).isEmpty());

        MainWindow window(m_logger, false, path);
        window.show();
        navigate(window, 1);
        auto *send = control<QPushButton>(window, "sendEcho");
        auto *cancel = control<QPushButton>(window, "cancelEcho");
        auto *result = control<QLabel>(window, "echoResult");
        control<QSpinBox>(window, "echoAssociationTimeout")->setValue(10);
        send->click();
        QVERIFY(!send->isEnabled());
        QVERIFY(cancel->isEnabled());
        QTRY_VERIFY(peer.hasPendingConnections());
        auto *socket = peer.nextPendingConnection();
        navigate(window, 2);
        navigate(window, 1);
        QVERIFY(result->text().contains("in progress"));
        cancel->click();
        QTRY_COMPARE_WITH_TIMEOUT(result->text(), QString("Verification cancelled."), 2000);
        QVERIFY(send->isEnabled());
        QVERIFY(!cancel->isEnabled());
        QVERIFY(control<QLabel>(window, "echoElapsed")->text().endsWith(" ms"));
        QTRY_COMPARE(socket->state(), QAbstractSocket::UnconnectedState);
        control<QPushButton>(window, "echoOpenLogs")->click();
        const auto id = control<QLineEdit>(window, "logOperation")->text();
        QVERIFY(!id.isEmpty());
        const auto events = m_logger.snapshot();
        QVERIFY(std::ranges::any_of(events, [&](const auto &event) {
            return event.operation == id && event.component == "Verification" &&
                   !event.synthetic && event.message.contains("cancelled", Qt::CaseInsensitive);
        }));

        navigate(window, 1);
        auto *protocol = control<QPushButton>(window, "verificationProtocolDetails");
        auto *sessions = control<QComboBox>(window, "verificationProtocolSessions");
        QCOMPARE(sessions->count(), 1);
        QVERIFY(protocol->isEnabled());
        control<QSpinBox>(window, "echoAssociationTimeout")->setValue(1);
        send->click();
        QTRY_VERIFY(peer.hasPendingConnections());
        auto *stalled = peer.nextPendingConnection();
        QTRY_VERIFY_WITH_TIMEOUT(result->text().contains("timed out"), 2500);
        QTRY_COMPARE(stalled->state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(sessions->count(), 2);
        auto *recent = control<QTableWidget>(window, "recentOperations");
        QCOMPARE(recent->rowCount(), 2);
        QCOMPARE(recent->item(0, 3)->text(), QString("Failed"));
        QCOMPARE(recent->item(1, 3)->text(), QString("Cancelled"));
        navigate(window, 0);
        QVERIFY(recent->isVisible());
        control<QPushButton>(window, "dashboardDiagnosticSessions")->click();
        QCOMPARE(control<QStackedWidget>(window, "workspaces")->currentIndex(), 7);
        navigate(window, 1);
        QString raw;
        QTimer::singleShot(0, &window, [&] {
            if (auto *dialog = window.findChild<QDialog *>("protocolInspector")) {
                dialog->findChild<QTabWidget *>("protocolTabs")->setCurrentIndex(3);
                raw = dialog->findChild<QPlainTextEdit *>("protocolRaw")->toPlainText();
                dialog->reject();
            }
        });
        protocol->click();
        QVERIFY(raw.contains("DICOM association negotiation timed out"));
        QVERIFY(raw.contains("No DIMSE messages were captured."));
        QTimer::singleShot(0, &window, [&] {
            if (auto *clear = window.findChild<QPushButton *>("clearProtocolHistory")) {
                clear->click();
                qobject_cast<QDialog *>(clear->window())->reject();
            }
        });
        control<QPushButton>(window, "settingsButton")->click();
        QCOMPARE(sessions->count(), 0);
        QVERIFY(!protocol->isEnabled());
        QCOMPARE(recent->rowCount(), 0);
        navigate(window, 0);
        QVERIFY(control<QLabel>(window, "recentOperationsEmpty")->isVisible());
        navigate(window, 1);

        send->click();
        QTRY_VERIFY(peer.hasPendingConnections());
        auto *onClose = peer.nextPendingConnection();
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 2000);
        QTRY_COMPARE(onClose->state(), QAbstractSocket::UnconnectedState);
    }

    void liveStorageReceiveStopsOnWindowClose()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QTcpServer reserve;
        QVERIFY(reserve.listen(QHostAddress::LocalHost));
        const auto port = reserve.serverPort();
        reserve.close();
        const auto profilePath = directory.filePath("profiles.json");
        dingobench::profiles::Store profiles(profilePath);
        QVERIFY(profiles.save(dingobench::profiles::Kind::Source,
            {0, "Local receiver fixture", "TEST_SCP", "127.0.0.1", port, directory.path()}).isEmpty());
        MainWindow window(m_logger, false, profilePath);
        window.show();
        navigate(window, 3);
        QCOMPARE(control<QComboBox>(window, "receiveSourceProfile")->currentText(), QString("Local receiver fixture"));
        QCOMPARE(control<QLineEdit>(window, "receiveDirectory")->text(), directory.path());
        control<QPushButton>(window, "receiveStart")->click();
        QTRY_VERIFY(control<QLabel>(window, "receiveStatus")->text().startsWith("Listening on 127.0.0.1:"));
        QVERIFY(control<QPushButton>(window, "receiveStop")->isEnabled());
        QVERIFY(!control<QLineEdit>(window, "receiveDirectory")->isEnabled());
        navigate(window, 0);
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 4000);
        QVERIFY(reserve.listen(QHostAddress::LocalHost, port));
    }

    void localFilesAndDirectories()
    {
        QTemporaryDir local;
        QVERIFY(local.isValid());
        for (int index = 0; index < 28; ++index) {
            QFile fixture(local.filePath(QString("fixture-%1.dcm").arg(index)));
            QVERIFY(fixture.open(QIODevice::WriteOnly));
            QCOMPARE(fixture.write("Local dialog test fixture"), 25);
        }
        MainWindow window(m_logger, false);
        window.resize(1150, 740);
        window.show();
        navigate(window, 2);
        auto *table = control<QTableWidget>(window, "sendFiles");
        const auto path = local.filePath("fixture-0.dcm");
        chooseFile(window, "sendAddFiles", path, false);
        QTRY_COMPARE(table->rowCount(), 1);
        QTRY_VERIFY(control<QPushButton>(window, "sendAddDirectory")->isEnabled());
        QCOMPARE(table->item(0, 6)->text(), QString("Invalid DICOM Part 10 file."));
        QVERIFY(!control<QPushButton>(window, "sendStorage")->isEnabled());
        chooseFile(window, "sendAddDirectory", local.path(), true);
        QTRY_COMPARE(table->rowCount(), 28);
        QTRY_VERIFY(control<QPushButton>(window, "sendAddDirectory")->isEnabled());
        QTimer menuCheck;
        menuCheck.setInterval(25);
        bool contextIcons = false;
        connect(&menuCheck, &QTimer::timeout, &window, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            const auto actions = menu->actions();
            contextIcons = actions.size() == 2 && !actions[0]->icon().pixmap(18, 18).isNull()
                && !actions[1]->icon().pixmap(18, 18).isNull();
            menu->close();
            menuCheck.stop();
        });
        menuCheck.start();
        table->customContextMenuRequested({10, 10});
        QVERIFY(contextIcons);
        QElapsedTimer elapsed;
        elapsed.start();
        auto *scrollbar = table->verticalScrollBar();
        for (int step = 1; step <= 12; ++step) {
            scrollbar->setValue(scrollbar->maximum() * step / 12);
            QApplication::processEvents();
        }
        QCOMPARE(table->verticalScrollBar()->value(), table->verticalScrollBar()->maximum());
        qInfo() << "Twelve scroll steps through the 28-row local fixture:" << elapsed.elapsed() << "ms";
        QVERIFY(elapsed.elapsed() < 750);
        table->selectRow(0);
        control<QPushButton>(window, "sendRemove")->click();
        QCOMPARE(table->rowCount(), 27);
        control<QPushButton>(window, "sendClear")->click();
        QCOMPARE(table->rowCount(), 0);
        navigate(window, 3);
        chooseFile(window, "receiveBrowse", local.path(), true);
        QCOMPARE(QDir::fromNativeSeparators(control<QLineEdit>(window, "receiveDirectory")->text()), local.path());
        navigate(window, 6);
        chooseFile(window, "openDicomFile", path, false);
        auto *tree = control<QTreeView>(window, "dicomTagTree");
        auto *fileStatus = control<QLabel>(window, "dicomFileStatus");
        QTRY_COMPARE(fileStatus->property("tone").toString(), QString("error"));
        QCOMPARE(tree->model()->rowCount(), 0);
        window.close();
    }

    void profileManagementAndLogFilters()
    {
        MainWindow window(m_logger, false);
        window.show();
        navigate(window, 8);
        auto *selector = control<QComboBox>(window, "sourceProfile");
        saveProfile(window, "newSource", "QA local source");
        QCOMPARE(selector->count(), 2);
        QCOMPARE(selector->currentText(), QString("QA local source"));
        auto *table = control<QTableWidget>(window, "sourceProfilesTable");
        table->selectRow(1);
        saveProfile(window, "editSource", "QA edited source");
        QCOMPARE(selector->currentText(), QString("QA edited source"));
        saveProfile(window, "copySource", "QA duplicate source");
        QCOMPARE(selector->count(), 3);
        table->selectRow(2);
        deleteProfile(window, "deleteSource", false);
        QCOMPARE(selector->count(), 3);
        deleteProfile(window, "deleteSource", true);
        QCOMPARE(selector->count(), 2);
        QCOMPARE(selector->currentText(), QString("Bench Workstation"));
        navigate(window, 7);
        m_logger.log(Severity::Info, "Fixture", "Test log initialized", "TEST-01", {}, true);
        auto *logs = control<QTableView>(window, "logTable");
        QTRY_VERIFY(logs->model()->rowCount() > 3);
        const int count = logs->model()->rowCount();
        QVERIFY(count > 3);
        auto *search = control<QLineEdit>(window, "logSearch");
        search->setText("initialized");
        QCOMPARE(logs->model()->rowCount(), 1);
        search->clear();
        QCOMPARE(logs->model()->rowCount(), count);
        search->setText("initialized");
        const auto exportPath = m_settings.filePath("filtered-export.jsonl");
        chooseFile(window, "logExport", exportPath, false);
        QList<dingobench::logging::Event> imported;
        QString error;
        QVERIFY2(dingobench::archive::load(exportPath, imported, error), qPrintable(error));
        QCOMPARE(imported.size(), 1);
        QVERIFY(imported.first().message.contains("initialized"));
        QVERIFY(imported.first().synthetic);
        QVERIFY(std::ranges::any_of(m_logger.snapshot(), [](const auto &event) {
            return event.component == "Profiles" && event.message == "Connection profile created.";
        }));
        window.close();
    }

    void persistentDestinationProfileUi()
    {
        const auto path = m_settings.filePath("gui-profiles.json");
        int id = 0;
        {
            MainWindow window(m_logger, false, path);
            window.show();
            navigate(window, 8);
            control<QTabWidget>(window, "profileTabs")->setCurrentIndex(1);
            auto *table = control<QTableWidget>(window, "destinationProfilesTable");
            auto *selector = control<QComboBox>(window, "destinationProfile");
            bool aeRejected = false, hostRejected = false, portRejected = false;
            QTimer attempt;
            attempt.setInterval(30);
            connect(&attempt, &QTimer::timeout, &window, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                attempt.stop();
                control<QLineEdit>(*dialog, "profileName")->setText("Invalid");
                control<QLineEdit>(*dialog, "profileAe")->setText(QString(17, 'A'));
                control<QLineEdit>(*dialog, "profileAddress")->setText("example.org");
                control<QPushButton>(*dialog, "saveProfile")->click();
                aeRejected = dialog->isVisible() &&
                    control<QLabel>(*dialog, "profileError")->text().contains("AE title");
                control<QLineEdit>(*dialog, "profileAe")->setText("VALID_AE");
                control<QLineEdit>(*dialog, "profileAddress")->setText("http://example.org");
                control<QPushButton>(*dialog, "saveProfile")->click();
                hostRejected = dialog->isVisible() &&
                    control<QLabel>(*dialog, "profileError")->text().contains("hostname");
                control<QLineEdit>(*dialog, "profileAddress")->setText("example.org");
                control<QLineEdit>(*dialog, "profilePort")->setText("65536");
                control<QPushButton>(*dialog, "saveProfile")->click();
                portRejected = dialog->isVisible() &&
                    control<QLabel>(*dialog, "profileError")->text().contains("65535");
                dialog->reject();
            });
            attempt.start();
            control<QPushButton>(window, "newDestination")->click();
            QVERIFY(aeRejected && hostRejected && portRejected);
            QCOMPARE(selector->count(), 0);
            QVERIFY(!QFile::exists(path));
            saveProfile(window, "newDestination", QStringLiteral("Nœud — 検査"));
            QCOMPARE(selector->count(), 1);
            id = selector->currentData().toInt();
            table->selectRow(0);
            saveProfile(window, "editDestination", QStringLiteral("Nœud — 検査 edited"));
            QCOMPARE(selector->currentData().toInt(), id);
            QCOMPARE(selector->currentText(), QStringLiteral("Nœud — 検査 edited"));
            saveProfile(window, "copyDestination", "Temporary duplicate");
            QVERIFY(selector->currentData().toInt() != id);
            table->selectRow(1);
            deleteProfile(window, "deleteDestination", true);
            QCOMPARE(selector->count(), 1);
            QCOMPARE(selector->currentText(), QStringLiteral("Nœud — 検査 edited"));
            selector->setCurrentIndex(selector->findData(id));
            navigate(window, 1);
            QCOMPARE(selector->currentData().toInt(), id);
            window.close();
        }
        MainWindow restarted(m_logger, false, path);
        auto *destination = control<QComboBox>(restarted, "destinationProfile");
        QCOMPARE(destination->currentData().toInt(), id);
        QCOMPARE(destination->currentText(), QStringLiteral("Nœud — 検査 edited"));
        navigate(restarted, 1);
        const auto children = control<QStackedWidget>(restarted, "workspaces")->currentWidget()
            ->findChildren<QComboBox *>();
        QVERIFY(std::ranges::any_of(children, [id](const auto *combo) {
            return combo->accessibleName() == "Destination Profile" && combo->currentData().toInt() == id;
        }));
        navigate(restarted, 8);
        control<QTabWidget>(restarted, "profileTabs")->setCurrentIndex(1);
        deleteProfile(restarted, "deleteDestination", true);
        QCOMPARE(destination->count(), 0);
        QVERIFY(!control<QPushButton>(restarted, "sendEcho")->isEnabled());
        restarted.close();
    }

    void workerLogDeliveryAndClear()
    {
        using dingobench::logging::Severity;
        MainWindow window(m_logger, false);
        navigate(window, 7);
        auto *logs = control<QTableView>(window, "logTable");
        auto *console = control<QTableView>(window, "consoleTable");
        auto *model = logs->model();
        const int initialRows = model->rowCount();
        bool updatesOnGui = true;
        connect(model, &QAbstractItemModel::rowsInserted, &window, [&] {
            updatesOnGui &= QThread::currentThread() == qApp->thread();
        }, Qt::DirectConnection);
        std::thread producer([this] {
            m_logger.log(Severity::Error, "Worker", "Synthetic test message", "OP-1", "Context ID: 3");
        });
        producer.join();
        QCOMPARE(model->rowCount(), initialRows);
        QTRY_COMPARE(model->rowCount(), initialRows + 1);
        QCOMPARE(console->model()->rowCount(), model->rowCount());
        QVERIFY(updatesOnGui);
        QCOMPARE(model->index(initialRows, 1).data().toString(), QString("ERROR"));
        QCOMPARE(model->index(initialRows, 2).data().toString(), QString("Worker"));
        QCOMPARE(model->index(initialRows, 3).data().toString(), QString("OP-1"));
        const auto time = model->index(initialRows, 0).data().toString();
        QVERIFY(QTime::fromString(time, "HH:mm:ss.zzz").isValid());
        logs->setCurrentIndex(model->index(initialRows, 0));
        QVERIFY(control<QPlainTextEdit>(window, "logDetails")->toPlainText().contains("Context ID: 3"));
        m_logger.log(Severity::Info, "Fixture", "First subsequent event");
        m_logger.log(Severity::Info, "Fixture", "Second subsequent event");
        m_logger.setCapacity(2);
        QTRY_COMPARE(model->rowCount(), 2);
        QCOMPARE(model->index(1, 0).siblingAtColumn(4).data().toString(), QString("Second subsequent event"));
        control<QPushButton>(window, "consoleClear")->click();
        QTRY_COMPARE(model->rowCount(), 0);
        QCOMPARE(console->model()->rowCount(), 0);
        QVERIFY(m_logger.snapshot().isEmpty());
        m_logger.setCapacity(2000);
    }

    void logTailingFiltersAndPause()
    {
        using dingobench::logging::Severity;
        MainWindow window(m_logger, false);
        window.resize(1000, 650);
        window.show();
        navigate(window, 7);
        auto *view = control<QTableView>(window, "logTable");
        auto *model = view->model();
        auto *scroll = view->verticalScrollBar();
        auto *follow = control<QCheckBox>(window, "logFollow");
        auto *pause = control<QPushButton>(window, "logPause");
        using dingobench::ui::Symbol;
        const auto iconImage = [pause](Symbol symbol) {
            return dingobench::ui::icon(symbol).pixmap(pause->iconSize()).toImage();
        };
        QCOMPARE(pause->icon().pixmap(pause->iconSize()).toImage(), iconImage(Symbol::Pause));
        auto *status = control<QLabel>(window, "logStatus");
        auto *search = control<QLineEdit>(window, "logSearch");
        auto *component = control<QLineEdit>(window, "logComponent");
        auto *operation = control<QLineEdit>(window, "logOperation");
        m_logger.clear();
        QTRY_COMPARE(model->rowCount(), 0);
        for (int i = 0; i < 50; ++i)
            m_logger.log(Severity(1 + i % 3), i % 2 ? "Query" : "Storage",
                         QString("Association %1").arg(i), i % 2 ? "OP-B" : "OP-A", {}, true);
        QTRY_COMPARE(model->rowCount(), 50);
        follow->setChecked(false);
        follow->setChecked(true);
        QTRY_VERIFY(scroll->maximum() > 0);
        QCOMPARE(scroll->value(), scroll->maximum());
        scroll->setValue(5);
        QVERIFY(!follow->isChecked());
        const auto anchor = model->index(view->rowAt(0), 4).data().toString();
        m_logger.log(Severity::Error, "Query", "Additional Association", "OP-B");
        QTRY_COMPARE(model->rowCount(), 51);
        QCOMPARE(model->index(view->rowAt(0), 4).data().toString(), anchor);
        pause->click();
        QCOMPARE(pause->text(), QStringLiteral("Resume"));
        QCOMPARE(pause->icon().pixmap(pause->iconSize()).toImage(), iconImage(Symbol::Run));
        navigate(window, 0);
        m_logger.log(Severity::Warning, "Query", "Paused Association", "OP-B");
        QTRY_VERIFY(status->text().contains("1 received"));
        QCOMPARE(model->rowCount(), 51);
        QTRY_COMPARE(control<QTableView>(window, "consoleTable")->model()->rowCount(), 52);
        navigate(window, 7);
        pause->click();
        QCOMPARE(pause->text(), QStringLiteral("Pause"));
        QCOMPARE(pause->icon().pixmap(pause->iconSize()).toImage(), iconImage(Symbol::Pause));
        QTRY_COMPARE(model->rowCount(), 52);
        for (int level = 1; level <= 3; ++level) {
            int matches = 0;
            for (const auto &event : m_logger.snapshot())
                matches += int(event.severity) == level;
            for (int i = 0; i < 4; ++i)
                control<QAction>(window, qPrintable(QString("logLevel%1").arg(i)))->setChecked(i == level);
            QCOMPARE(model->rowCount(), matches);
        }
        control<QAction>(window, "logLevel2")->setChecked(true);
        component->setText("QUERY");
        operation->setText("op-b");
        search->setText("paused ASSOCIATION");
        QCOMPARE(model->rowCount(), 1);
        QVERIFY(status->text().contains("1 visible / 52 retained"));
        control<QCheckBox>(window, "logRegex")->setChecked(true);
        search->setText("[");
        QVERIFY(status->text().contains("Invalid regex"));
        search->setText("^Paused\\s");
        QCOMPARE(model->rowCount(), 1);
        control<QPushButton>(window, "logReset")->click();
        QCOMPARE(model->rowCount(), 52);
        m_logger.setCapacity(10);
        QTRY_COMPARE(model->rowCount(), 10);
        QVERIFY(status->text().contains("evicted"));
        search->setText("paused");
        QCOMPARE(model->rowCount(), 1);
        control<QPushButton>(window, "logClear")->click();
        QTRY_COMPARE(model->rowCount(), 0);
        QVERIFY(status->text().contains("0 visible / 0 retained"));
        window.close();
    }

    void logCopyAndArchive()
    {
        using dingobench::logging::Severity;
        using namespace dingobench::archive;
        MainWindow window(m_logger, false);
        window.show();
        navigate(window, 7);
        auto *view = control<QTableView>(window, "logTable");
        auto *model = view->model();
        m_logger.clear();
        const QString message = QStringLiteral("Données — 検査\n\t") + QString(3000, 'x');
        m_logger.log(Severity::Warning, "Fixture", message, "OP-01", "Optional details", true);
        m_logger.log(Severity::Error, "Other", "Second event", "OP-02");
        QTRY_COMPARE(model->index(0, 3).data().toString(), QString("OP-01"));
        QCOMPARE(model->rowCount(), 2);
        view->selectRow(0);
        view->setCurrentIndex(model->index(0, 0));
        QCOMPARE(control<QPlainTextEdit>(window, "logDetails")->toPlainText().contains("Optional details"), true);
        control<QAction>(window, "logCopyMessage")->trigger();
        QCOMPARE(QApplication::clipboard()->text(), message);
        control<QAction>(window, "logCopy")->trigger();
        QVERIFY(QApplication::clipboard()->text().contains("OP-01"));
        view->selectAll();
        control<QAction>(window, "logCopy")->trigger();
        QVERIFY(QApplication::clipboard()->text().contains("Second event"));
        QVERIFY(!QApplication::clipboard()->text().contains("Optional details"));
        control<QAction>(window, "logCopyVisible")->trigger();
        QVERIFY(QApplication::clipboard()->text().contains(message));

        const auto textPath = m_settings.filePath("all-logs.txt");
        chooseFile(window, "logExportAll", textPath, false);
        QList<dingobench::logging::Event> records;
        QString error;
        QVERIFY2(load(textPath, records, error), qPrintable(error));
        QCOMPARE(records.size(), 2);
        QCOMPARE(records.first().message, message);
        QCOMPARE(records.first().timestampUtc, m_logger.snapshot().first().timestampUtc);
        QCOMPARE(records.first().severity, Severity::Warning);
        QCOMPARE(records.first().component, QString("Fixture"));
        QCOMPARE(records.first().operation, QString("OP-01"));
        QVERIFY(records.first().synthetic);
        QVERIFY(records.first().details.isEmpty());

        QVERIFY(QDir(m_settings.path()).mkdir("GitHub"));
        for (const auto format : {Format::Text, Format::JsonLines}) {
            const auto blocked = m_settings.filePath("GitHub/blocked-log");
            QVERIFY(!save(blocked, records, format, false, error));
            QVERIFY(error.contains("outside Git checkouts"));
            QVERIFY(!QFile::exists(blocked));
        }
        QVERIFY(QDir(m_settings.path()).rmdir("GitHub"));
        const auto jsonPath = m_settings.filePath("details.jsonl");
        QVERIFY2(save(jsonPath, m_logger.snapshot(), Format::JsonLines, true, error), qPrintable(error));
        chooseFile(window, "logImport", jsonPath, false);
        QVERIFY(control<QLabel>(window, "logStatus")->text().contains("Saved session"));
        m_logger.log(Severity::Info, "Live", "New live record");
        QTRY_COMPARE(control<QTableView>(window, "consoleTable")->model()->rowCount(), 3);
        QCOMPARE(model->rowCount(), 2);
        view->setCurrentIndex(model->index(0, 0));
        QVERIFY(control<QPlainTextEdit>(window, "logDetails")->toPlainText().contains("Optional details"));
        control<QAction>(window, "logLive")->trigger();
        QCOMPARE(model->rowCount(), 3);
        const auto invalid = m_settings.filePath("invalid-session.jsonl");
        QFile corrupt(invalid);
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        QVERIFY(corrupt.write("{\"format\":\"DingoBench log v1\"}\n{}\n") > 0);
        corrupt.close();
        chooseFile(window, "logImport", invalid, false);
        QVERIFY(control<QLabel>(window, "logStatus")->text().contains("Could not open saved session"));
        QCOMPARE(model->rowCount(), 3);
        window.close();
    }

    void logExportRejectsRedirectedCheckout()
    {
        using namespace dingobench::archive;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir("GitHub"));
        const auto checkout = directory.filePath("GitHub");
        const auto alias = directory.filePath("redirect");
#ifdef Q_OS_WIN
        // Directory junctions exercise redirection without requiring symlink privileges.
        QProcess junction;
        junction.setProcessChannelMode(QProcess::MergedChannels);
        junction.setProgram(QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/cmd.exe"));
        junction.setNativeArguments(QString("/d /c mklink /J \"%1\" \"%2\"")
            .arg(QDir::toNativeSeparators(alias), QDir::toNativeSeparators(checkout)));
        junction.start();
        QVERIFY(junction.waitForFinished(5000));
        QVERIFY2(junction.exitStatus() == QProcess::NormalExit && junction.exitCode() == 0,
                 junction.readAll().constData());
#else
        std::error_code error;
        std::filesystem::create_directory_symlink(QFile::encodeName(checkout).constData(),
                                                 QFile::encodeName(alias).constData(), error);
        QVERIFY2(!error, error.message().c_str());
#endif
        QString message;
        for (const auto format : {Format::Text, Format::JsonLines}) {
            const auto target = QDir(alias).filePath("events");
            QVERIFY(!save(target, {}, format, false, message));
            QVERIFY(message.contains("outside Git checkouts"));
            QVERIFY(!QFile::exists(QDir(checkout).filePath("events")));
        }
    }

    void logDisplayPreferences()
    {
        {
            MainWindow window(m_logger);
            control<QCheckBox>(window, "logFollow")->setChecked(false);
            control<QLineEdit>(window, "logOperation")->setText("TEST-01");
            control<QAction>(window, "logLevel3")->setChecked(false);
            control<QTableView>(window, "logTable")->setColumnWidth(0, 145);
            window.close();
        }
        MainWindow window(m_logger);
        QVERIFY(!control<QCheckBox>(window, "logFollow")->isChecked());
        QVERIFY(control<QLineEdit>(window, "logOperation")->text().isEmpty());
        QVERIFY(!control<QAction>(window, "logLevel3")->isChecked());
        QCOMPARE(control<QTableView>(window, "logTable")->columnWidth(0), 145);
        QCOMPARE(control<QTableView>(window, "logTable")->model()->rowCount(), 0);
        window.close();
    }

    void invalidWorkspacePreferencesAreLogged()
    {
        QSettings().setValue("workspace/geometry", QByteArray("invalid geometry"));
        MainWindow window(m_logger);
        QVERIFY(std::ranges::any_of(m_logger.snapshot(), [](const auto &event) {
            return event.severity == dingobench::logging::Severity::Warning
                && event.component == "Workspace"
                && event.message == "Stored window geometry could not be restored.";
        }));
    }

    void bundledIconsAndIdlePaint()
    {
        QString failure;
        QVERIFY2(dingobench::ui::validateIcons(&failure), qPrintable(failure));
        QVERIFY(QFile::exists(":/material/LICENSE.txt"));
        QVERIFY(QFile::exists(":/material/NOTICE.txt"));
        MainWindow window(m_logger, false);
        window.show();
        struct PaintCounter : QObject {
            QWidget *window;
            int count = 0;
            bool eventFilter(QObject *watched, QEvent *event) override
            {
                if (event->type() == QEvent::Paint) {
                    auto *widget = qobject_cast<QWidget *>(watched);
                    if (widget && widget->window() == window)
                        ++count;
                }
                return false;
            }
        } counter;
        counter.window = &window;
        qApp->installEventFilter(&counter);
        QTest::qWait(120);
        counter.count = 0;
        QTest::qWait(220);
        qApp->removeEventFilter(&counter);
        qInfo() << "Idle workspace paint events over 220 ms:" << counter.count;
        QCOMPARE(counter.count, 0);
        window.close();
    }

    void fileDialogToolbarIcons()
    {
        using dingobench::ui::Symbol;
        QFileDialog dialog;
        dialog.setOption(QFileDialog::DontUseNativeDialog);
        dialog.setFileMode(QFileDialog::Directory);
        dialog.setLayoutDirection(Qt::LeftToRight);
        dialog.show();
        const std::pair<const char *, Symbol> buttons[] = {
            {"backButton", Symbol::Back}, {"forwardButton", Symbol::Forward},
            {"toParentButton", Symbol::ParentFolder}, {"newFolderButton", Symbol::NewFolder},
            {"listModeButton", Symbol::FileList}, {"detailModeButton", Symbol::FileDetails},
        };
        for (const auto &[name, symbol] : buttons) {
            const auto *button = control<QToolButton>(dialog, name);
            const QSize size = button->iconSize();
            QCOMPARE(size, QSize(18, 18));
            QCOMPARE(button->icon().pixmap(size).toImage(),
                     dingobench::ui::icon(symbol).pixmap(size).toImage());
        }
    }

    void capture()
    {
        const QString target = qEnvironmentVariable("DINGOBENCH_CAPTURE_DIR");
        if (target.isEmpty())
            return;
        QVERIFY(QDir().mkpath(target));
        const int width = qEnvironmentVariableIntValue("DINGOBENCH_CAPTURE_WIDTH");
        const int height = qEnvironmentVariableIntValue("DINGOBENCH_CAPTURE_HEIGHT");
        QVERIFY(width > 0 && height > 0);
        MainWindow window(m_logger, false);
        window.setWindowFlag(Qt::FramelessWindowHint);
        window.show();
        const qreal scale = window.devicePixelRatioF();
        window.resize(QSize(qRound(width / scale), qRound(height / scale)));
        auto *toggle = control<QPushButton>(window, "toggleConsole");
        if (toggle->isChecked() != (window.height() >= 660))
            toggle->click();
        const auto take = [&](const QString &name) {
            QApplication::processEvents();
            const QPixmap image = window.grab();
            QVERIFY(qAbs(image.width() - width) <= 2 && qAbs(image.height() - height) <= 2);
            QVERIFY(image.save(QDir(target).filePath(name + ".png")));
        };
        const QStringList names{"dashboard", "verification", "storage-send", "storage-receive",
                                "query-retrieve", "worklist-query", "dicom-files", "logs", "profiles"};
        for (int index = 0; index < names.size(); ++index) {
            navigate(window, index);
            take(names.at(index));
        }
        navigate(window, 7);
        auto *pause = control<QPushButton>(window, "logPause");
        pause->click();
        take("logs-paused");
        pause->click();
        QFileDialog picker(&window, "Choose the worklist record directory", target);
        picker.setOptions(QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly);
        picker.setFileMode(QFileDialog::Directory);
        picker.show();
        QApplication::processEvents();
        QVERIFY(picker.grab().save(QDir(target).filePath("directory-picker.png")));
        picker.close();
        navigate(window, 5);
        control<QTabWidget>(window, "worklistTabs")->setCurrentIndex(1);
        take("worklist-server");
        navigate(window, 8);
        control<QTabWidget>(window, "profileTabs")->setCurrentIndex(1);
        take("destination-profiles");
        navigate(window, 1);
        navigate(window, 0);
        if (!toggle->isChecked())
            toggle->click();
        take("diagnostic-console");
        window.close();
    }
};

QTEST_MAIN(WorkspaceTest)
#include "WorkspaceTest.moc"
