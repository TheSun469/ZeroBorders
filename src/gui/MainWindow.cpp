#include "MainWindow.h"
#include "../config/ConfigManager.h"
#include "../core/Log.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QGroupBox>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QScreen>
#include <QGuiApplication>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace zb {

static QIcon makeTrayIcon(bool connected);

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("零界 ZeroBorders - 局域网键鼠共享"));
    resize(720, 760);

    buildUi();
    loadConfig();

    // Wire App signals to GUI slots (auto/queued connections cross threads).
    connect(&app_, &App::statusChanged, this, &MainWindow::onStatusChanged);
    connect(&app_, &App::connectionChanged, this, &MainWindow::onConnectionChanged);
    connect(&app_, &App::roleDetermined, this, &MainWindow::onRoleDetermined);
    connect(&app_, &App::transferProgress, this, &MainWindow::onTransferProgress);
    connect(&app_, &App::transferComplete, this, &MainWindow::onTransferComplete);
    connect(&app_, &App::incomingOffer, this, &MainWindow::onIncomingOffer);
    connect(&app_, &App::logMessage, this, &MainWindow::onLogMessage);
    connect(&app_, &App::sessionStopped, this, &MainWindow::onSessionStopped);
    connect(&app_, &App::layoutChanged, this, &MainWindow::onRemoteLayoutChanged);
    connect(&app_, &App::remoteReceiveDirChanged, this, &MainWindow::onRemoteReceiveDirChanged);
    connect(&app_, &App::remoteDirListed, this, &MainWindow::onRemoteDirListed);

    app_.installLogSink();
    buildTrayIcon();
    roleLabel_->setText(QStringLiteral("本机角色：未连接"));
    ZB_LOG_INFO("ZeroBorders GUI ready");
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // ---- Connection group ------------------------------------------------
    connGroup_ = new QGroupBox(QStringLiteral("连接"), central);
    auto* connLayout = new QGridLayout(connGroup_);

    auto* codeLabel = new QLabel(QStringLiteral("识别码："), connGroup_);
    codeEdit_ = new QLineEdit(connGroup_);
    codeEdit_->setPlaceholderText(QStringLiteral("两台电脑输入相同的识别码即可自动配对"));
    codeEdit_->setEchoMode(QLineEdit::Password);

    auto* usernameLabel = new QLabel(QStringLiteral("用户名："), connGroup_);
    usernameEdit_ = new QLineEdit(connGroup_);
    usernameEdit_->setPlaceholderText(QStringLiteral("两台电脑输入相同的用户名（双重保险）"));

    auto* rolePrefLabel = new QLabel(QStringLiteral("控制方式："), connGroup_);
    rolePrefCombo_ = new QComboBox(connGroup_);
    rolePrefCombo_->addItem(QStringLiteral("自动选择"), 0);
    rolePrefCombo_->addItem(QStringLiteral("本机控制对端"), 1);
    rolePrefCombo_->addItem(QStringLiteral("对端控制本机"), 2);
    rolePrefCombo_->setToolTip(QStringLiteral("选择鼠标键盘在哪台电脑上操作。冲突时自动选举。"));

    startBtn_ = new QPushButton(QStringLiteral("开始连接"), connGroup_);
    startBtn_->setObjectName(QStringLiteral("startBtn_"));
    statusLabel_ = new QLabel(QStringLiteral("空闲"), connGroup_);
    statusLabel_->setStyleSheet("color: #666;");
    peerLabel_ = new QLabel("", connGroup_);
    roleLabel_ = new QLabel(QStringLiteral("本机角色：未连接"), connGroup_);
    roleLabel_->setStyleSheet("color: #236; font-weight: bold;");

    connLayout->addWidget(codeLabel, 0, 0);
    connLayout->addWidget(codeEdit_, 0, 1, 1, 3);
    connLayout->addWidget(usernameLabel, 1, 0);
    connLayout->addWidget(usernameEdit_, 1, 1, 1, 3);
    connLayout->addWidget(rolePrefLabel, 2, 0);
    connLayout->addWidget(rolePrefCombo_, 2, 1, 1, 3);
    connLayout->addWidget(startBtn_, 3, 0);
    connLayout->addWidget(statusLabel_, 3, 1, 1, 2);
    connLayout->addWidget(peerLabel_, 3, 3);
    connLayout->addWidget(roleLabel_, 4, 0, 1, 4);

    // ---- Settings group --------------------------------------------------
    settingsGroup_ = new QGroupBox(QStringLiteral("设置"), central);
    auto* setLayout = new QGridLayout(settingsGroup_);

    autoStartCheck_ = new QCheckBox(QStringLiteral("开机自动启动"), settingsGroup_);
    minimizeToTrayCheck_ = new QCheckBox(QStringLiteral("关闭窗口时最小化到托盘"), settingsGroup_);
    setLayout->addWidget(autoStartCheck_, 0, 0);
    setLayout->addWidget(minimizeToTrayCheck_, 0, 1);

    // Put connection + settings in a fixed top container.
    auto* topContainer = new QWidget(central);
    auto* topLayout = new QVBoxLayout(topContainer);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(6);
    topLayout->addWidget(connGroup_);
    topLayout->addWidget(settingsGroup_);
    root->addWidget(topContainer);

    // ---- Vertical splitter for resizable sections ------------------------
    mainSplitter_ = new QSplitter(Qt::Vertical, central);
    mainSplitter_->setChildrenCollapsible(false);
    mainSplitter_->setHandleWidth(6);

    // ---- Screen layout group (collapsible) -------------------------------
    layoutGroup_ = new QGroupBox(mainSplitter_);
    auto* layOuter = new QVBoxLayout(layoutGroup_);

    // Title bar with collapse button.
    auto* layTitleRow = new QHBoxLayout();
    layTitleRow->setContentsMargins(0, 0, 0, 0);
    auto* layTitleLabel = new QLabel(
        QStringLiteral("设备位置（拖拽对端矩形设置相对位置）"), layoutGroup_);
    QFont titleFont = layTitleLabel->font();
    titleFont.setBold(true);
    layTitleLabel->setFont(titleFont);
    layoutCollapseBtn_ = new QToolButton(layoutGroup_);
    layoutCollapseBtn_->setArrowType(Qt::DownArrow);
    layoutCollapseBtn_->setToolTip(QStringLiteral("收起/展开"));
    layoutCollapseBtn_->setAutoRaise(true);
    layTitleRow->addWidget(layTitleLabel);
    layTitleRow->addStretch();
    layTitleRow->addWidget(layoutCollapseBtn_);
    layOuter->addLayout(layTitleRow);

    layoutWidget_ = new DeviceLayoutWidget(layoutGroup_);
    layOuter->addWidget(layoutWidget_);
    mainSplitter_->addWidget(layoutGroup_);

    // ---- File transfer group ---------------------------------------------
    transferGroup_ = new QGroupBox(QStringLiteral("文件传输"), mainSplitter_);
    auto* transLayout = new QVBoxLayout(transferGroup_);

    // Two browser panels side by side in a horizontal splitter so the user
    // can drag the divider to adjust the gap between local and remote.
    auto* browserSplitter = new QSplitter(Qt::Horizontal, transferGroup_);
    browserSplitter->setChildrenCollapsible(false);
    browserSplitter->setHandleWidth(5);

    auto* localPanel = new QWidget(browserSplitter);
    auto* localPanelLayout = new QVBoxLayout(localPanel);
    localPanelLayout->setContentsMargins(0, 0, 0, 0);
    localPanelLayout->setSpacing(4);
    auto* localLabel = new QLabel(QStringLiteral("本地"), localPanel);
    QFont labelFont = localLabel->font();
    labelFont.setBold(true);
    localLabel->setFont(labelFont);
    localBrowser_ = new FileBrowserWidget(FileBrowserWidget::Mode::Local, localPanel);
    localPanelLayout->addWidget(localLabel);
    localPanelLayout->addWidget(localBrowser_, 1);
    browserSplitter->addWidget(localPanel);

    auto* remotePanel = new QWidget(browserSplitter);
    auto* remotePanelLayout = new QVBoxLayout(remotePanel);
    remotePanelLayout->setContentsMargins(0, 0, 0, 0);
    remotePanelLayout->setSpacing(4);
    auto* remoteLabel = new QLabel(QStringLiteral("远程"), remotePanel);
    remoteLabel->setFont(labelFont);
    remoteBrowser_ = new FileBrowserWidget(FileBrowserWidget::Mode::Remote, remotePanel);
    remotePanelLayout->addWidget(remoteLabel);
    remotePanelLayout->addWidget(remoteBrowser_, 1);
    browserSplitter->addWidget(remotePanel);

    browserSplitter->setStretchFactor(0, 1);
    browserSplitter->setStretchFactor(1, 1);
    browserSplitter->setSizes({300, 300});

    transLayout->addWidget(browserSplitter, 1);

    // Button row: refresh remote only (upload/download via right-click).
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    refreshRemoteBtn_ = new QPushButton(QStringLiteral("刷新远程"), transferGroup_);
    refreshRemoteBtn_->setEnabled(false);
    btnRow->addWidget(refreshRemoteBtn_);
    transLayout->addLayout(btnRow);

    mainSplitter_->addWidget(transferGroup_);

    // ---- Log --------------------------------------------------------------
    auto* logGroup = new QGroupBox(QStringLiteral("日志"), mainSplitter_);
    auto* logLayout = new QVBoxLayout(logGroup);
    logView_ = new QPlainTextEdit(logGroup);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(1000);
    logView_->setStyleSheet(
        "QPlainTextEdit { font-family: Consolas, 'Courier New', monospace; font-size: 12px; }");
    clearLogBtn_ = new QPushButton(QStringLiteral("清空日志"), logGroup);
    auto* logBtnRow = new QHBoxLayout();
    logBtnRow->addStretch();
    logBtnRow->addWidget(clearLogBtn_);
    logLayout->addWidget(logView_);
    logLayout->addLayout(logBtnRow);
    mainSplitter_->addWidget(logGroup);

    // Splitter stretch: layout (small), transfer (medium), log (medium).
    mainSplitter_->setStretchFactor(0, 0);
    mainSplitter_->setStretchFactor(1, 3);
    mainSplitter_->setStretchFactor(2, 2);
    mainSplitter_->setSizes({120, 320, 200});

    root->addWidget(mainSplitter_, 1);

    setCentralWidget(central);

    // ---- Connections ------------------------------------------------------
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStartStop);
    connect(refreshRemoteBtn_, &QPushButton::clicked, this, &MainWindow::onRefreshRemote);
    connect(clearLogBtn_, &QPushButton::clicked, logView_, &QPlainTextEdit::clear);
    connect(layoutCollapseBtn_, &QToolButton::clicked, this, [this] {
        setLayoutCollapsed(!layoutCollapsed_);
    });
    connect(localBrowser_, &FileBrowserWidget::pathChanged,
            this, &MainWindow::onLocalPathChanged);
    connect(remoteBrowser_, &FileBrowserWidget::navigateRequested,
            this, &MainWindow::onNavigateRemote);
    // 远程路径下拉树懒加载：请求对端返回目录内容（仅填充树，不切换主表）
    connect(remoteBrowser_, &FileBrowserWidget::fetchRequested,
            this, [this](const QString& p) {
        if (app_.isRunning() && app_.isConnected())
            app_.requestRemoteDirList(p.toStdString());
    });
    // Right-click transfer: local → upload, remote → download.
    connect(localBrowser_, &FileBrowserWidget::transferRequested,
            this, &MainWindow::onSendSelected);
    connect(remoteBrowser_, &FileBrowserWidget::transferRequested,
            this, &MainWindow::onDownloadSelected);
    connect(autoStartCheck_, &QCheckBox::toggled, this, &MainWindow::onToggleAutoStart);

    // Layout widget: persist and push live while connected.
    connect(layoutWidget_, &DeviceLayoutWidget::layoutChanged,
            this, &MainWindow::onLayoutChanged);
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void MainWindow::loadConfig() {
    config_ = AppConfig{};
    ConfigManager::instance().load(config_);

    codeEdit_->setText(QString::fromStdString(config_.pairingCode));
    usernameEdit_->setText(QString::fromStdString(config_.username));

    int prefIdx = rolePrefCombo_->findData(config_.rolePreference);
    rolePrefCombo_->setCurrentIndex(prefIdx >= 0 ? prefIdx : 0);

    layoutWidget_->setLayout(layoutFromString(config_.layout));

    // Initialize local screen resolution for the layout canvas.
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        QSize sz = screen->size();
        layoutWidget_->setLocalResolution(
            static_cast<uint32_t>(sz.width()),
            static_cast<uint32_t>(sz.height()));
    }

    // Initialize local receive directory. If none configured, default to
    // TEMP/ZeroBorders.
    QString localDir = QString::fromStdString(config_.receiveDir);
    if (localDir.isEmpty()) {
        localDir = QDir::toNativeSeparators(QDir::tempPath())
                   + QStringLiteral("\\ZeroBorders");
    }
    localBrowser_->setPath(localDir);
    remoteBrowser_->setReadOnly(true);

    minimizeToTrayCheck_->setChecked(config_.startMinimized);
    autoStartCheck_->blockSignals(true);
    autoStartCheck_->setChecked(ConfigManager::instance().isAutoStartEnabled());
    autoStartCheck_->blockSignals(false);
}

void MainWindow::saveConfig() {
    config_ = collectConfig();
    ConfigManager::instance().save(config_);
}

AppConfig MainWindow::collectConfig() const {
    AppConfig c = config_;
    c.pairingCode = codeEdit_->text().toStdString();
    c.username = usernameEdit_->text().toStdString();
    c.rolePreference = rolePrefCombo_->currentData().toInt();

    c.layout = layoutToString(layoutWidget_->layout());

    c.receiveDir = localBrowser_->path().toStdString();
    c.startMinimized = minimizeToTrayCheck_->isChecked();
    return c;
}

// ---------------------------------------------------------------------------
// UI state helpers
// ---------------------------------------------------------------------------

void MainWindow::setRunningUi(bool running) {
    codeEdit_->setEnabled(!running);
    usernameEdit_->setEnabled(!running);
    rolePrefCombo_->setEnabled(!running);
    // Local browser stays enabled so user can browse/select files while connected.
    localBrowser_->setReadOnly(false);
    remoteBrowser_->setReadOnly(!running || !app_.isConnected());
    startBtn_->setText(running ? QStringLiteral("断开连接") : QStringLiteral("开始连接"));
    refreshRemoteBtn_->setEnabled(running && app_.isConnected());
    if (startStopAction_) startStopAction_->setText(running ? QStringLiteral("断开") : QStringLiteral("开始连接"));
}

void MainWindow::setLayoutCollapsed(bool collapsed) {
    layoutCollapsed_ = collapsed;
    if (!mainSplitter_) return;

    QList<int> sizes = mainSplitter_->sizes();
    if (sizes.size() < 3) return;

    if (collapsed) {
        // Remember current height before collapsing (but only if it has real size).
        if (sizes[0] > 40) savedLayoutHeight_ = sizes[0];
        if (layoutWidget_) layoutWidget_->hide();
        if (layoutCollapseBtn_) layoutCollapseBtn_->setArrowType(Qt::RightArrow);
        // Force the group box to shrink to its title-only size by capping max height.
        QApplication::processEvents();
        int titleH = layoutGroup_->minimumSizeHint().height();
        // minimumSizeHint may still include the hidden widget; estimate from style.
        if (titleH > 60) titleH = qMax(36, layoutGroup_->fontMetrics().height() + 16);
        layoutGroup_->setMaximumHeight(titleH);
        int freed = sizes[0] - titleH;
        sizes[0] = titleH;
        // Give freed space proportionally to the other two sections.
        int other = sizes[1] + sizes[2];
        if (other > 0) {
            sizes[1] += freed * sizes[1] / other;
            sizes[2] += freed * sizes[2] / other;
        } else {
            sizes[1] += freed;
        }
        mainSplitter_->setSizes(sizes);
    } else {
        layoutGroup_->setMaximumHeight(QWIDGETSIZE_MAX);
        if (layoutWidget_) layoutWidget_->show();
        if (layoutCollapseBtn_) layoutCollapseBtn_->setArrowType(Qt::DownArrow);
        QApplication::processEvents();
        // Restore saved height, taking it from the other sections.
        int targetH = savedLayoutHeight_;
        int available = sizes[1] + sizes[2];
        if (targetH > available + sizes[0]) targetH = available + sizes[0] - 40;
        if (targetH < 80) targetH = 120;
        int delta = targetH - sizes[0];
        sizes[0] = targetH;
        int other = sizes[1] + sizes[2];
        if (other > 0) {
            sizes[1] -= delta * sizes[1] / other;
            sizes[2] -= delta * sizes[2] / other;
        } else {
            sizes[1] -= delta;
        }
        mainSplitter_->setSizes(sizes);
    }
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

void MainWindow::onStartStop() {
    if (app_.isRunning()) {
        app_.stop();
        setRunningUi(false);
        roleLabel_->setText(QStringLiteral("本机角色：未连接"));
        return;
    }

    AppConfig cfg = collectConfig();
    if (cfg.pairingCode.empty()) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
                             QStringLiteral("请输入识别码。"));
        return;
    }
    if (cfg.username.empty()) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
                             QStringLiteral("请输入用户名。"));
        return;
    }
    saveConfig();

    setRunningUi(true);
    activeSide_ = 0;
    transferIncoming_ = false;
    roleLabel_->setText(QStringLiteral("本机角色：正在自动选举..."));

    // Automatic mode: discover a peer with the same pairing code and elect
    // roles automatically — no manual server/client selection needed.
    app_.startAuto(cfg);
}

void MainWindow::onRoleDetermined(bool isServer) {
    roleLabel_->setText(isServer ? QStringLiteral("本机角色：控制端（键鼠在此）")
                                 : QStringLiteral("本机角色：被控端"));
}

void MainWindow::onSessionStopped() {
    // Called when a session ends on its own (e.g. connection failure/timeout).
    QMetaObject::invokeMethod(this, [this] {
        setRunningUi(false);
        roleLabel_->setText(QStringLiteral("本机角色：未连接"));
    }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// File transfer
// ---------------------------------------------------------------------------

void MainWindow::onSendSelected() {
    if (!app_.isRunning() || !app_.isConnected()) {
        QMessageBox::information(this, QStringLiteral("零界 ZeroBorders"),
                                 QStringLiteral("尚未连接。"));
        return;
    }
    QStringList selected = localBrowser_->selectedPaths();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("零界 ZeroBorders"),
                                 QStringLiteral("请在左侧本地文件列表中选择要发送的文件或文件夹。"));
        return;
    }

    std::vector<std::string> paths;
    paths.reserve(selected.size());
    for (const QString& p : selected) paths.push_back(p.toStdString());

    uint64_t id = app_.sendFiles(paths);
    if (id == 0) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
                             QStringLiteral("启动文件传输失败。"));
        return;
    }
    currentTransferId_ = id;
    activeSide_ = 1;  // local side (upload)
    transferIncoming_ = false;
    localBrowser_->showProgress(QStringLiteral("正在上传 %1 个项目...").arg(selected.size()));
}

void MainWindow::onDownloadSelected() {
    if (!app_.isRunning() || !app_.isConnected()) {
        QMessageBox::information(this, QStringLiteral("零界 ZeroBorders"),
                                 QStringLiteral("尚未连接。"));
        return;
    }
    QStringList selected = remoteBrowser_->selectedPaths();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("零界 ZeroBorders"),
                                 QStringLiteral("请在右侧远程文件列表中选择要下载的文件或文件夹。"));
        return;
    }

    // Build full remote paths from current directory + selected names.
    QString remoteDir = remoteBrowser_->path();

    std::vector<std::string> remotePaths;
    remotePaths.reserve(selected.size());
    for (const QString& name : selected) {
        // At root level, entries like "C:\" are already full paths.
        if (remoteDir.isEmpty()) {
            remotePaths.push_back(name.toStdString());
        } else {
            QString full = remoteDir;
            if (!full.endsWith(QLatin1Char('/')) && !full.endsWith(QLatin1Char('\\')))
                full += QLatin1Char('/');
            full += name;
            remotePaths.push_back(full.toStdString());
        }
    }

    QString localDir = localBrowser_->path();
    if (localDir.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
                             QStringLiteral("无法确定本地保存目录。"));
        return;
    }

    app_.requestRemoteFiles(remotePaths, localDir.toStdString());
    currentTransferId_ = 0;
    activeSide_ = 2;  // remote side (download)
    transferIncoming_ = false;
    remoteBrowser_->showProgress(QStringLiteral("正在下载 %1 个项目...").arg(selected.size()));
}

void MainWindow::onRefreshRemote() {
    if (!app_.isRunning() || !app_.isConnected()) return;
    QString p = remoteBrowser_->path();
    app_.requestRemoteDirList(p.toStdString());
}

void MainWindow::onNavigateRemote(const QString& path) {
    if (!app_.isRunning() || !app_.isConnected()) return;
    app_.requestRemoteDirList(path.toStdString());
}

void MainWindow::onRemoteDirListed(const QString& path, bool ok,
                                   const QVariantList& entries) {
    if (!ok) {
        // 仅当失败的是当前浏览路径时才在主表显示错误
        if (path == remoteBrowser_->path())
            remoteBrowser_->showRemoteError(path, QStringLiteral("无法访问目录"));
        return;
    }
    QVector<RemoteDirEntry> dirEntries;
    dirEntries.reserve(entries.size());
    const bool isRoot = path.isEmpty();
    for (const QVariant& v : entries) {
        QVariantMap m = v.toMap();
        RemoteDirEntry e;
        e.name = m[QStringLiteral("name")].toString();
        e.size = m[QStringLiteral("size")].toULongLong();
        e.isDirectory = m[QStringLiteral("isDir")].toBool();
        e.isDrive = isRoot && e.isDirectory;  // 根目录下的目录均为驱动器
        e.modified = m[QStringLiteral("mtime")].toDateTime();
        dirEntries.push_back(e);
    }
    // 始终把结果填充到下拉树缓存，方便用户在树中展开浏览
    remoteBrowser_->setRemoteTreeEntries(path, dirEntries);
    // 仅当返回的是当前正在浏览的路径时才更新主表
    if (path == remoteBrowser_->path()) {
        remoteBrowser_->setRemoteEntries(path, dirEntries);
    }
}

void MainWindow::onTransferProgress(quint64 id, quint64 transferred, quint64 total) {
    updateProgress(id, transferred, total);
}

void MainWindow::onTransferComplete(quint64 id, bool ok, const QString& msg) {
    if (id == currentTransferId_ || currentTransferId_ == 0) {
        QString label = ok ? QStringLiteral("传输完成")
                           : (QStringLiteral("失败：") + msg);
        FileBrowserWidget* side = transferIncoming_ ? localBrowser_ :
                                  (activeSide_ == 1 ? localBrowser_ : remoteBrowser_);
        if (side) {
            side->updateProgress(ok ? 100 : 0, label);
            side->hideProgress();
        }
        currentTransferId_ = 0;
        activeSide_ = 0;
        transferIncoming_ = false;
    }
}

void MainWindow::updateProgress(quint64 id, quint64 transferred, quint64 total) {
    if (total == 0) return;
    // Track this transfer if none is currently tracked (covers downloads).
    if (currentTransferId_ == 0) {
        currentTransferId_ = id;
        // Auto-detect: incoming transfer shows on local side.
        if (activeSide_ == 0) {
            transferIncoming_ = true;
            activeSide_ = 1;
        }
    }
    if (id != currentTransferId_) return;

    int pct = static_cast<int>((transferred * 100) / total);
    double mbXfer = transferred / (1024.0 * 1024.0);
    double mbTotal = total / (1024.0 * 1024.0);
    QString label = QString::asprintf("%.2f / %.2f MB", mbXfer, mbTotal);

    FileBrowserWidget* side = transferIncoming_ ? localBrowser_ :
                              (activeSide_ == 1 ? localBrowser_ : remoteBrowser_);
    if (side && !side->isVisible()) {
        side->showProgress(label);
    }
    if (side) side->updateProgress(pct, label);
}

// ---------------------------------------------------------------------------
// Status / connection
// ---------------------------------------------------------------------------

void MainWindow::onStatusChanged(const QString& status) {
    statusLabel_->setText(status);
}

void MainWindow::onConnectionChanged(bool connected, const QString& peerName,
                                     quint32 peerW, quint32 peerH) {
    if (connected) {
        statusLabel_->setStyleSheet("color: #2a7; font-weight: bold;");
        if (peerW && peerH) {
            peerLabel_->setText(QString("对端：%1x%2").arg(peerW).arg(peerH));
            layoutWidget_->setPeerResolution(peerW, peerH);
        }
        layoutWidget_->setConnected(true);
        layoutWidget_->setPeerName(peerName);
        refreshRemoteBtn_->setEnabled(true);
        remoteBrowser_->setReadOnly(false);
        // Remote dir list is requested by App on connection; clear stale
        // content so the user sees immediate feedback.
        remoteBrowser_->clearRemoteEntries();
        if (trayIcon_) {
            trayIcon_->setIcon(makeTrayIcon(true));
            trayIcon_->setToolTip(QStringLiteral("零界 ZeroBorders（已连接）"));
        }
    } else {
        statusLabel_->setStyleSheet("color: #666;");
        peerLabel_->setText("");
        refreshRemoteBtn_->setEnabled(false);
        layoutWidget_->setConnected(false);
        layoutWidget_->setPeerName(QString());
        remoteBrowser_->setReadOnly(true);
        remoteBrowser_->setPath(QString());
        remoteBrowser_->clearRemoteEntries();
        remoteBrowser_->clearRemoteTree();
        if (trayIcon_) {
            trayIcon_->setIcon(makeTrayIcon(false));
            trayIcon_->setToolTip(QStringLiteral("零界 ZeroBorders（未连接）"));
        }
    }
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

void MainWindow::onLogMessage(const QString& level, const QString& line) {
    appendLog(line, level);
}

void MainWindow::appendLog(const QString& line, const QString& tag) {
    // Choose color by level.
    QString color = "#333";
    if (tag == "WRN") color = "#b8860b";
    else if (tag == "ERR") color = "#c33";
    else if (tag == "INF") color = "#236";

    QString trimmed = line;
    trimmed.chop(1); // trailing newline from logger

    logView_->appendHtml(QString("<span style='color:%1'>%2</span>")
                             .arg(color, trimmed.toHtmlEscaped()));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (quitting_) {
        saveConfig();
        event->accept();
        return;
    }

    // If minimize-to-tray is enabled and a session is active, hide instead
    // of quitting so the KVM session keeps running in the background.
    if (minimizeToTrayCheck_->isChecked() && app_.isRunning() &&
        QSystemTrayIcon::isSystemTrayAvailable()) {
        event->ignore();
        hide();
        trayIcon_->showMessage(QStringLiteral("零界 ZeroBorders"),
            QStringLiteral("程序仍在后台运行，右键托盘图标可退出。"),
            QSystemTrayIcon::Information, 3000);
        return;
    }

    if (app_.isRunning()) {
        auto reply = QMessageBox::question(
            this, QStringLiteral("零界 ZeroBorders"),
            QStringLiteral("当前有活动连接，确定断开并退出吗？"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        app_.stop();
    }
    saveConfig();
    event->accept();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void MainWindow::onLocalPathChanged(const QString& path) {
    config_.receiveDir = path.toStdString();
    saveConfig();
    if (app_.isRunning() && app_.isConnected()) {
        app_.notifyPathSync();
    }
}

void MainWindow::onToggleAutoStart(bool enabled) {
    if (!ConfigManager::instance().setAutoStart(enabled)) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
            QStringLiteral("无法") + (enabled ? QStringLiteral("启用") : QStringLiteral("禁用")) +
            QStringLiteral("开机自启，请尝试以管理员身份运行。"));
        // Revert checkbox state.
        autoStartCheck_->blockSignals(true);
        autoStartCheck_->setChecked(ConfigManager::instance().isAutoStartEnabled());
        autoStartCheck_->blockSignals(false);
    }
}

void MainWindow::onLayoutChanged(ScreenLayout layout) {
    config_.layout = layoutToString(layout);
    saveConfig();
    // User is actively changing layout — expand the section so they can see it.
    setLayoutCollapsed(false);

    // 任意一端都可以调整相对位置，App::setLayout 会把变更同步给对端。
    if (app_.isRunning() && app_.isConnected()) {
        app_.setLayout(layout);
    }
}

void MainWindow::onRemoteLayoutChanged(ScreenLayout layout) {
    // 对端推送过来的布局：更新本地画布与配置，但不再回发（App 内部已防回环）。
    layoutWidget_->setLayout(layout);
    config_.layout = layoutToString(layout);
    saveConfig();
    // Peer changed layout — expand to show the new position.
    setLayoutCollapsed(false);
}

void MainWindow::onRemoteReceiveDirChanged(const QString& dir) {
    if (dir.isEmpty()) {
        remoteBrowser_->setPath(QStringLiteral("%TEMP%\\ZeroBorders（对端默认）"));
    } else {
        remoteBrowser_->setPath(dir);
    }
    // Request a listing of the peer's receive directory to populate the
    // remote browser.
    if (app_.isRunning() && app_.isConnected()) {
        app_.requestRemoteDirList(dir.toStdString());
    }
}

// ---------------------------------------------------------------------------
// System tray
// ---------------------------------------------------------------------------

static QIcon makeTrayIcon(bool connected) {
    // Use the embedded application icon, then overlay a small status dot.
    QPixmap pm(":/icons/app.png");
    if (pm.isNull()) {
        // Fallback: simple procedural icon if the resource is missing.
        pm = QPixmap(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(connected ? QColor(0x2e, 0x8b, 0x57) : QColor(0x55, 0x55, 0x55));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(2, 2, 28, 28, 6, 6);
        p.setPen(Qt::white);
        QFont f = p.font();
        f.setBold(true);
        f.setPointSize(12);
        p.setFont(f);
        p.drawText(pm.rect(), Qt::AlignCenter, "ZB");
        return QIcon(pm);
    }

    pm = pm.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    // Status badge in the bottom-right corner.
    QColor badge = connected ? QColor(0x22, 0xc5, 0x5e)  // green
                             : QColor(0x9c, 0x9c, 0x9c); // gray
    p.setBrush(badge);
    p.drawEllipse(20, 20, 10, 10);
    // Thin dark ring around the badge for contrast.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0x1e, 0x29, 0x3b), 1.5));
    p.drawEllipse(20, 20, 10, 10);
    return QIcon(pm);
}

void MainWindow::buildTrayIcon() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        ZB_LOG_WARN("System tray not available; tray icon disabled");
        return;
    }

    trayIcon_ = new QSystemTrayIcon(makeTrayIcon(false), this);
    trayIcon_->setToolTip(QStringLiteral("零界 ZeroBorders（未连接）"));

    trayMenu_ = new QMenu(this);
    toggleWindowAction_ = trayMenu_->addAction(QStringLiteral("显示主窗口"), this, &MainWindow::onShowWindow);
    startStopAction_ = trayMenu_->addAction(QStringLiteral("开始连接"), this, &MainWindow::onStartStop);
    trayMenu_->addSeparator();
    quitAction_ = trayMenu_->addAction(QStringLiteral("退出"), this, &MainWindow::onQuitApp);

    trayIcon_->setContextMenu(trayMenu_);
    connect(trayIcon_, &QSystemTrayIcon::activated,
            this, &MainWindow::onTrayActivated);
    trayIcon_->show();
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger ||
        reason == QSystemTrayIcon::DoubleClick) {
        toggleVisible();
    }
}

void MainWindow::onShowWindow() {
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::onQuitApp() {
    quitting_ = true;
    if (app_.isRunning()) app_.stop();
    saveConfig();
    if (trayIcon_) trayIcon_->hide();
    QApplication::quit();
}

void MainWindow::toggleVisible() {
    if (isVisible()) {
        hide();
    } else {
        onShowWindow();
    }
}

// ---------------------------------------------------------------------------
// Incoming file offer (confirmation dialog)
// ---------------------------------------------------------------------------

void MainWindow::onIncomingOffer(quint64 id, const QStringList& files, quint64 total) {
    double mb = total / (1024.0 * 1024.0);
    QString dirText = localBrowser_->path().isEmpty()
        ? QStringLiteral("%TEMP%\\ZeroBorders")
        : localBrowser_->path();
    QString text = QStringLiteral("收到文件传输请求：\n\n%1 个文件，共 %2 MB\n\n"
                                  "保存到：%3\n\n是否接收？")
        .arg(files.size())
        .arg(mb, 0, 'f', 2)
        .arg(dirText);

    auto reply = QMessageBox::question(this, QStringLiteral("收到文件传输请求"), text,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (reply == QMessageBox::Yes) {
        std::string dir = config_.receiveDir;
        app_.acceptTransfer(static_cast<uint64_t>(id), dir);
        // Show progress on local side for the incoming transfer.
        currentTransferId_ = static_cast<quint64>(id);
        activeSide_ = 1;
        transferIncoming_ = true;
        localBrowser_->showProgress(QStringLiteral("正在接收 %1 个项目...").arg(files.size()));
    } else {
        app_.rejectTransfer(static_cast<uint64_t>(id));
    }
}

} // namespace zb
