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
class QStatusBar;

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
    void onRefreshLocal();
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
    void onOpenSettings();
    void onNewFolder();
    void onNewFile();
    void onDeleteSelected();
    void onRenameSelected();
    void onCopyPath();

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
    void buildMenuBar();
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

    // --- Connection (start button only, rest moved to status bar) ---
    QGroupBox* connGroup_ = nullptr;
    QPushButton* startBtn_ = nullptr;

    // --- Settings dialog widgets (created on demand) ---
    // Stored in the dialog, not here.

    // --- Screen layout group ---
    QGroupBox* layoutGroup_ = nullptr;
    DeviceLayoutWidget* layoutWidget_ = nullptr;
    QToolButton* layoutCollapseBtn_ = nullptr;
    bool layoutCollapsed_ = false;
    int savedLayoutHeight_ = 260;

    // --- File transfer group ---
    QGroupBox* transferGroup_ = nullptr;
    FileBrowserWidget* localBrowser_ = nullptr;
    FileBrowserWidget* remoteBrowser_ = nullptr;

    int activeSide_ = 0;

    // --- Log ---
    QPlainTextEdit* logView_ = nullptr;

    // --- Status bar widgets ---
    QLabel* statusLabel_ = nullptr;
    QLabel* peerLabel_ = nullptr;
    QLabel* roleLabel_ = nullptr;

    // --- System tray ---
    QSystemTrayIcon* trayIcon_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    QAction* toggleWindowAction_ = nullptr;
    QAction* startStopAction_ = nullptr;
    QAction* quitAction_ = nullptr;
    bool quitting_ = false;

    QSplitter* mainSplitter_ = nullptr;

    quint64 currentTransferId_ = 0;
    bool transferIncoming_ = false;
};

} // namespace zb
