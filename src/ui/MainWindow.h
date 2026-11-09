// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "Ui.h"
#include "dingobench/dicom/OperationRunner.h"
#include "dingobench/dicom/StorageReceive.h"
#include "dingobench/dicom/WorklistServer.h"
#include "dingobench/infrastructure/Logger.h"
#include "dingobench/infrastructure/ProfileStore.h"

#include <QMainWindow>
#include <functional>

class QAbstractButton;
class QComboBox;
class QDockWidget;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QScrollArea;
class QTableWidget;
class QVBoxLayout;

namespace dingobench {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(logging::Logger &logger, bool rememberLayout = true, QString profilePath = {});

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    enum Page { Dashboard, Verification, Send, Receive, Query, Worklist, Files, Logs, Profiles, PageCount };

    void selectPage(int page);
    void refreshOperationStatus();
    void watch(std::function<void()> refresh);
    QPushButton *createDestinationButton();
    void refreshDashboardHistory();
    QWidget *page(const QString &title, const QString &description, QVBoxLayout **body);
    QWidget *connectionRow(bool showSource = true, bool showDestination = true);
    QComboBox *profileSelector(bool source, const char *objectName = nullptr);
    const profiles::Profile &selectedProfile(bool source) const;
    void reportProfileError(const QString &error);
    void refreshProfiles();
    void selectProfile(bool source, int id);
    void editProfile(bool source, int id, bool duplicate = false);
    void showSettings();
    void showAbout();
    void addProtocolControls(QHBoxLayout *layout, QString operation, const char *comboName, const char *buttonName);
    void retainProtocol(const dicom::DiagnosticSession &session);
    void refreshProtocolControls(QUuid newest = {});
    void closeWhenOperationsFinish();
    QWidget *logExplorer(bool compact);
    void toggleConsole(bool visible);

    QWidget *dashboard();
    QWidget *verification();
    QWidget *storageSend();
    QWidget *storageReceive();
    QWidget *queryRetrieve();
    struct RetrievalSelection {
        QString studyUid, seriesUid;
        profiles::Profile archive;
    };
    void buildMovePanel(QVBoxLayout *body, QScrollArea *scroll, QTableWidget *results,
                        std::function<std::optional<RetrievalSelection>()> selected);
    void buildGetPanel(QVBoxLayout *body, QScrollArea *scroll, QTableWidget *results,
                       std::function<std::optional<RetrievalSelection>()> selected);
    QWidget *worklist();
    void buildWorklistQuery(QVBoxLayout *body, QScrollArea *scroll);
    void buildWorklistServer(QVBoxLayout *body, QWidget *page);
    QWidget *dicomFiles();
    QWidget *logs();
    QWidget *profiles();

    QStackedWidget *m_pages = nullptr;
    QSplitter *m_splitter = nullptr;
    QWidget *m_console = nullptr;
    QAbstractButton *m_consoleToggle = nullptr;
    QTableWidget *m_recentOperations = nullptr;
    QLabel *m_recentEmpty = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_stateLabel = nullptr;
    QLabel *m_statusIcon = nullptr;
    logging::Logger &m_logger;
    dicom::OperationRunner m_verificationRunner;
    dicom::OperationRunner m_storageRunner;
    dicom::OperationRunner m_queryRunner;
    dicom::OperationRunner m_moveRunner;
    dicom::OperationRunner m_getRunner;
    dicom::OperationRunner m_worklistRunner;
    dicom::OperationRunner m_fileRunner;
    dicom::StorageReceiver m_receiver;
    dicom::WorklistServer m_worklistServer;
    QUuid m_verificationId;
    QUuid m_storageId;
    QUuid m_queryId;
    QUuid m_moveId;
    QUuid m_getId;
    QUuid m_worklistId;
    QUuid m_fileId;
    QList<dicom::DiagnosticSession> m_protocolHistory;
    struct ProtocolControls {
        QString operation;
        QComboBox *sessions;
        QPushButton *details;
    };
    QList<ProtocolControls> m_protocolControls;
    bool m_closeAfterOperations = false;
    QList<QComboBox *> m_sourceSelectors;
    QList<QComboBox *> m_destinationSelectors;
    profiles::Store m_profiles;
    QList<std::function<void()>> m_refresh;
    int m_consoleHeight = 150;
    bool m_rememberLayout;
};

}  // namespace dingobench
