#pragma once

#include "../App.h"
#include "../config/AppConfig.h"
#include "DeviceLayoutWidget.h"

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
    void onBrowseFiles();
    void onSendFiles();
    void onBrowseReceiveDir();
    void onToggleAutoStart(bool enabled);
    void onLayoutChanged(ScreenLayout layout);
    void onRemoteLayoutChanged(ScreenLayout layout);
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

    App app_;
    AppConfig config_;

    // --- Connection group ---
    QGroupBox* connGroup_ = nullptr;
    QLineEdit* codeEdit_ = nullptr;
    QComboBox* rolePrefCombo_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* peerLabel_ = nullptr;
    QLabel* roleLabel_ = nullptr;

    // --- Settings group ---
    QGroupBox* settingsGroup_ = nullptr;
    QLineEdit* receiveDirEdit_ = nullptr;
    QPushButton* browseReceiveBtn_ = nullptr;
    QCheckBox* autoStartCheck_ = nullptr;
    QCheckBox* minimizeToTrayCheck_ = nullptr;

    // --- Screen layout group ---
    QGroupBox* layoutGroup_ = nullptr;
    DeviceLayoutWidget* layoutWidget_ = nullptr;

    // --- File transfer group ---
    QGroupBox* transferGroup_ = nullptr;
    QListWidget* fileList_ = nullptr;
    QPushButton* addFilesBtn_ = nullptr;
    QPushButton* clearFilesBtn_ = nullptr;
    QPushButton* sendBtn_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* progressLabel_ = nullptr;

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

    quint64 currentTransferId_ = 0;
};

} // namespace zb
