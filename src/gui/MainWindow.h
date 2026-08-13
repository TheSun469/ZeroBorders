#pragma once

#include "../App.h"
#include "../config/AppConfig.h"
#include "DeviceLayoutWidget.h"
#include "FileBrowserWidget.h"

#include <QMainWindow>
#include <QSystemTrayIcon>

#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QComboBox;
class QProgressBar;
class QPlainTextEdit;
class QListWidget;
class QGroupBox;
class QMenu;
class QAction;
class QToolButton;
class QSplitter;

namespace zb {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onStartStop();
    void onSendSelected();
    void onDownloadSelected();
    void onRefreshRemote();
    void onLocalPathChanged(const QString& path);
    void onNavigateRemote(const QString& path);
    void onRemoteDirListed(const QString& path, bool ok, const QVariantList& entries);
    void onToggleAutoStart(bool enabled);
    void onLayoutChanged(ScreenLayout layout);
    void onRemoteLayoutChanged(ScreenLayout layout);
    void onRemoteReceiveDirChanged(const QString& dir);
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onShowWindow();
    void onQuitApp();

    void onStatusChanged(const QString& status);
    void onConnectionChanged(bool connected, const QString& peerName,
                             quint32 peerW, quint32 peerH);
    void onRoleDetermined(bool isServer);
    void onTransferProgress(quint64 id, quint64 transferred, quint64 total);
    void onTransferComplete(quint64 id, bool ok, const QString& msg);
    void onIncomingOffer(quint64 id, const QStringList& files, quint64 total);
    void onLogMessage(const QString& level, const QString& line);
    void onSessionStopped();

private:
    void buildUi();
    void buildTrayIcon();
    void loadConfig();
    void saveConfig();
    AppConfig collectConfig() const;
    void setRunningUi(bool running);
    void appendLog(const QString& line, const QString& tag);
    void updateProgress(quint64 id, quint64 transferred, quint64 total);
    void toggleVisible();
    void setLayoutCollapsed(bool collapsed);

    App app_;
    AppConfig config_;

    // --- Connection group ---
    QGroupBox* connGroup_ = nullptr;
    QLineEdit* codeEdit_ = nullptr;
    QLineEdit* usernameEdit_ = nullptr;
    QComboBox* rolePrefCombo_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* peerLabel_ = nullptr;
    QLabel* roleLabel_ = nullptr;

    // --- Settings group ---
    QGroupBox* settingsGroup_ = nullptr;
    QCheckBox* autoStartCheck_ = nullptr;
    QCheckBox* minimizeToTrayCheck_ = nullptr;

    // --- Screen layout group ---
    QGroupBox* layoutGroup_ = nullptr;
    DeviceLayoutWidget* layoutWidget_ = nullptr;
    QToolButton* layoutCollapseBtn_ = nullptr;
    bool layoutCollapsed_ = false;
    int savedLayoutHeight_ = 140;  // remembered height for restore

    // --- File transfer group ---
    QGroupBox* transferGroup_ = nullptr;
    // Left = local filesystem browser, right = remote receive directory.
    FileBrowserWidget* localBrowser_ = nullptr;
    FileBrowserWidget* remoteBrowser_ = nullptr;
    QPushButton* refreshRemoteBtn_ = nullptr;

    // Track which side initiated the current transfer.
    // 0 = none, 1 = local (upload), 2 = remote (download)
    int activeSide_ = 0;

    // --- Log ---
    QPlainTextEdit* logView_ = nullptr;
    QPushButton* clearLogBtn_ = nullptr;

    // --- System tray ---
    QSystemTrayIcon* trayIcon_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    QAction* toggleWindowAction_ = nullptr;
    QAction* startStopAction_ = nullptr;
    QAction* quitAction_ = nullptr;
    bool quitting_ = false;

    QSplitter* mainSplitter_ = nullptr;

    quint64 currentTransferId_ = 0;
    bool transferIncoming_ = false;  // true = incoming transfer (show on local)
};

} // namespace zb
