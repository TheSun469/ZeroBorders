#include "MainWindow.h"
#include "../config/ConfigManager.h"
#include "../core/Log.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QScreen>
#include <QGuiApplication>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QFileSystemModel>

#include <filesystem>

namespace fs = std::filesystem;

namespace zb {

static QIcon makeTrayIcon(bool connected);

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("零界 ZeroBorders - 局域网键鼠共享"));
    resize(720, 760);

    buildUi();
    buildMenuBar();
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
    roleLabel_->setText(QStringLiteral("角色：未连接"));
    ZB_LOG_INFO("ZeroBorders GUI ready");
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 4);
    root->setSpacing(6);

    // ---- Connection bar (compact: just start button) --------------------
    connGroup_ = new QGroupBox(QStringLiteral("连接"), central);
    auto* connLayout = new QHBoxLayout(connGroup_);
    startBtn_ = new QPushButton(QStringLiteral("开始连接"), connGroup_);
    startBtn_->setObjectName(QStringLiteral("startBtn_"));
    connLayout->addWidget(startBtn_);
    connLayout->addStretch();
    root->addWidget(connGroup_);

    // ---- Vertical splitter for resizable sections ------------------------
    mainSplitter_ = new QSplitter(Qt::Vertical, central);
    mainSplitter_->setChildrenCollapsible(false);
    mainSplitter_->setHandleWidth(6);

    // ---- Screen layout group (collapsible) -------------------------------
    layoutGroup_ = new QGroupBox(mainSplitter_);
    auto* layOuter = new QVBoxLayout(layoutGroup_);

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

    mainSplitter_->addWidget(transferGroup_);

    // ---- Log --------------------------------------------------------------
    auto* logGroup = new QGroupBox(QStringLiteral("日志"), mainSplitter_);
    auto* logLayout = new QVBoxLayout(logGroup);
    logView_ = new QPlainTextEdit(logGroup);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(1000);
    logView_->setStyleSheet(
        "QPlainTextEdit { font-family: Consolas, 'Courier New', monospace; font-size: 12px; }");
    logLayout->addWidget(logView_);
    mainSplitter_->addWidget(logGroup);

    mainSplitter_->setStretchFactor(0, 0);
    mainSplitter_->setStretchFactor(1, 3);
    mainSplitter_->setStretchFactor(2, 2);
    mainSplitter_->setSizes({260, 300, 140});

    root->addWidget(mainSplitter_, 1);

    setCentralWidget(central);

    // ---- Status bar -------------------------------------------------------
    auto* sb = statusBar();
    statusLabel_ = new QLabel(QStringLiteral("空闲"), sb);
    statusLabel_->setStyleSheet("color: #666; padding: 0 8px;");
    roleLabel_ = new QLabel(QStringLiteral("角色：未连接"), sb);
    roleLabel_->setStyleSheet("color: #236; font-weight: bold; padding: 0 8px;");
    peerLabel_ = new QLabel("", sb);
    peerLabel_->setStyleSheet("color: #555; padding: 0 8px;");
    sb->addWidget(statusLabel_);
    sb->addWidget(roleLabel_);
    sb->addPermanentWidget(peerLabel_);

    // ---- Connections ------------------------------------------------------
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStartStop);
    connect(layoutCollapseBtn_, &QToolButton::clicked, this, [this] {
        setLayoutCollapsed(!layoutCollapsed_);
    });
    connect(localBrowser_, &FileBrowserWidget::pathChanged,
            this, &MainWindow::onLocalPathChanged);
    connect(remoteBrowser_, &FileBrowserWidget::navigateRequested,
            this, &MainWindow::onNavigateRemote);
    connect(remoteBrowser_, &FileBrowserWidget::fetchRequested,
            this, [this](const QString& p) {
        if (app_.isRunning() && app_.isConnected())
            app_.requestRemoteDirList(p.toStdString());
    });
    connect(localBrowser_, &FileBrowserWidget::transferRequested,
            this, &MainWindow::onSendSelected);
    connect(remoteBrowser_, &FileBrowserWidget::transferRequested,
            this, &MainWindow::onDownloadSelected);
    connect(localBrowser_, &FileBrowserWidget::refreshRequested,
            this, &MainWindow::onRefreshLocal);
    connect(remoteBrowser_, &FileBrowserWidget::refreshRequested,
            this, &MainWindow::onRefreshRemote);
    connect(localBrowser_, &FileBrowserWidget::deleteRequested,
            this, &MainWindow::onDeleteSelected);
    connect(localBrowser_, &FileBrowserWidget::renameRequested,
            this, &MainWindow::onRenameSelected);
    connect(localBrowser_, &FileBrowserWidget::newFolderRequested,
            this, &MainWindow::onNewFolder);

    connect(layoutWidget_, &DeviceLayoutWidget::layoutChanged,
            this, &MainWindow::onLayoutChanged);
}

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------

void MainWindow::buildMenuBar() {
    auto* mb = menuBar();

    // ---- 文件菜单 ----
    auto* fileMenu = mb->addMenu(QStringLiteral("文件"));

    auto* newFolderAct = fileMenu->addAction(QStringLiteral("新建文件夹"));
    newFolderAct->setShortcut(QKeySequence::New);
    connect(newFolderAct, &QAction::triggered, this, &MainWindow::onNewFolder);

    auto* newFileAct = fileMenu->addAction(QStringLiteral("新建文件"));
    connect(newFileAct, &QAction::triggered, this, &MainWindow::onNewFile);

    fileMenu->addSeparator();

    auto* deleteAct = fileMenu->addAction(QStringLiteral("删除"));
    deleteAct->setShortcut(QKeySequence::Delete);
    connect(deleteAct, &QAction::triggered, this, &MainWindow::onDeleteSelected);

    auto* renameAct = fileMenu->addAction(QStringLiteral("重命名"));
    renameAct->setShortcut(QKeySequence("F2"));
    connect(renameAct, &QAction::triggered, this, &MainWindow::onRenameSelected);

    fileMenu->addSeparator();

    auto* copyPathAct = fileMenu->addAction(QStringLiteral("复制文件地址"));
    connect(copyPathAct, &QAction::triggered, this, &MainWindow::onCopyPath);

    fileMenu->addSeparator();

    auto* quitAct = fileMenu->addAction(QStringLiteral("退出"));
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, this, &MainWindow::onQuitApp);

    // ---- 设置菜单 ----
    auto* settingsMenu = mb->addMenu(QStringLiteral("设置"));
    auto* openSettingsAct = settingsMenu->addAction(QStringLiteral("配置..."));
    connect(openSettingsAct, &QAction::triggered, this, &MainWindow::onOpenSettings);
}

// ---------------------------------------------------------------------------
// Settings dialog
// ---------------------------------------------------------------------------

void MainWindow::onOpenSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("设置"));
    dlg.setMinimumWidth(420);

    auto* layout = new QVBoxLayout(&dlg);

    // ---- 认证分类 ----
    auto* authGroup = new QGroupBox(QStringLiteral("认证"), &dlg);
    auto* authForm = new QFormLayout(authGroup);
    auto* codeEdit = new QLineEdit(authGroup);
    codeEdit->setEchoMode(QLineEdit::Password);
    codeEdit->setPlaceholderText(QStringLiteral("两台电脑输入相同的识别码"));
    codeEdit->setText(QString::fromStdString(config_.pairingCode));
    auto* usernameEdit = new QLineEdit(authGroup);
    usernameEdit->setPlaceholderText(QStringLiteral("两台电脑输入相同的用户名"));
    usernameEdit->setText(QString::fromStdString(config_.username));
    authForm->addRow(QStringLiteral("识别码："), codeEdit);
    authForm->addRow(QStringLiteral("用户名："), usernameEdit);
    layout->addWidget(authGroup);

    // ---- 连接分类 ----
    auto* connGroup = new QGroupBox(QStringLiteral("连接"), &dlg);
    auto* connForm = new QFormLayout(connGroup);
    auto* rolePrefCombo = new QComboBox(connGroup);
    rolePrefCombo->addItem(QStringLiteral("自动选择"), 0);
    rolePrefCombo->addItem(QStringLiteral("本机控制对端"), 1);
    rolePrefCombo->addItem(QStringLiteral("对端控制本机"), 2);
    int prefIdx = rolePrefCombo->findData(config_.rolePreference);
    rolePrefCombo->setCurrentIndex(prefIdx >= 0 ? prefIdx : 0);
    connForm->addRow(QStringLiteral("控制方式："), rolePrefCombo);
    layout->addWidget(connGroup);

    // ---- 系统分类 ----
    auto* sysGroup = new QGroupBox(QStringLiteral("系统"), &dlg);
    auto* sysForm = new QFormLayout(sysGroup);
    auto* autoStartCheck = new QCheckBox(QStringLiteral("开机自动启动"), sysGroup);
    autoStartCheck->setChecked(ConfigManager::instance().isAutoStartEnabled());
    auto* minimizeCheck = new QCheckBox(QStringLiteral("关闭窗口时最小化到托盘"), sysGroup);
    minimizeCheck->setChecked(config_.startMinimized);
    sysForm->addRow(autoStartCheck);
    sysForm->addRow(minimizeCheck);
    layout->addWidget(sysGroup);

    // ---- Buttons ----
    auto* btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(btnBox);

    if (dlg.exec() == QDialog::Accepted) {
        config_.pairingCode = codeEdit->text().toStdString();
        config_.username = usernameEdit->text().toStdString();
        config_.rolePreference = rolePrefCombo->currentData().toInt();
        config_.startMinimized = minimizeCheck->isChecked();

        // Auto-start toggle
        bool wantAuto = autoStartCheck->isChecked();
        bool hasAuto = ConfigManager::instance().isAutoStartEnabled();
        if (wantAuto != hasAuto) {
            if (!ConfigManager::instance().setAutoStart(wantAuto)) {
                QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
                    QStringLiteral("无法修改开机自启，请尝试以管理员身份运行。"));
            }
        }

        saveConfig();

        // If running, update role label text
        if (app_.isRunning()) {
            setRunningUi(true);
        }
    }
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void MainWindow::loadConfig() {
    config_ = AppConfig{};
    ConfigManager::instance().load(config_);

    layoutWidget_->setLayout(layoutFromString(config_.layout));

    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        QSize sz = screen->size();
        layoutWidget_->setLocalResolution(
            static_cast<uint32_t>(sz.width()),
            static_cast<uint32_t>(sz.height()));
    }

    QString localDir = QString::fromStdString(config_.receiveDir);
    if (localDir.isEmpty()) {
        localDir = QDir::toNativeSeparators(QDir::tempPath())
                   + QStringLiteral("\\ZeroBorders");
    }
    localBrowser_->setPath(localDir);
    remoteBrowser_->setReadOnly(true);
}

void MainWindow::saveConfig() {
    config_ = collectConfig();
    ConfigManager::instance().save(config_);
}

AppConfig MainWindow::collectConfig() const {
    AppConfig c = config_;
    c.layout = layoutToString(layoutWidget_->layout());
    c.receiveDir = QDir::toNativeSeparators(localBrowser_->path()).toStdString();
    return c;
}

// ---------------------------------------------------------------------------
// UI state helpers
// ---------------------------------------------------------------------------

void MainWindow::setRunningUi(bool running) {
    startBtn_->setText(running ? QStringLiteral("断开连接") : QStringLiteral("开始连接"));
    if (startStopAction_) startStopAction_->setText(running ? QStringLiteral("断开") : QStringLiteral("开始连接"));
}

void MainWindow::setLayoutCollapsed(bool collapsed) {
    layoutCollapsed_ = collapsed;
    if (!mainSplitter_) return;

    QList<int> sizes = mainSplitter_->sizes();
    if (sizes.size() < 3) return;

    if (collapsed) {
        // Remember current height before collapsing.
        if (sizes[0] > 40) savedLayoutHeight_ = sizes[0];
        if (layoutWidget_) layoutWidget_->setVisible(false);
        if (layoutCollapseBtn_) layoutCollapseBtn_->setArrowType(Qt::RightArrow);

        // Collapse to minimal height (just title bar).
        int titleH = layoutGroup_->fontMetrics().height() + 24;
        int freed = sizes[0] - titleH;
        if (freed < 0) freed = 0;
        sizes[0] = titleH;
        int other = sizes[1] + sizes[2];
        if (other > 0) {
            sizes[1] += freed * sizes[1] / other;
            sizes[2] += freed * sizes[2] / other;
        } else {
            sizes[1] += freed;
        }
        mainSplitter_->setSizes(sizes);
    } else {
        if (layoutWidget_) layoutWidget_->setVisible(true);
        if (layoutCollapseBtn_) layoutCollapseBtn_->setArrowType(Qt::DownArrow);

        int targetH = savedLayoutHeight_;
        int available = sizes[1] + sizes[2];
        if (targetH > available + sizes[0]) targetH = available + sizes[0] - 40;
        if (targetH < 200) targetH = 260;
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
        roleLabel_->setText(QStringLiteral("角色：未连接"));
        return;
    }

    // Pull latest config from settings dialog fields
    if (config_.pairingCode.empty()) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
                             QStringLiteral("请先在 设置 →配置 中输入识别码。"));
        return;
    }
    if (config_.username.empty()) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
                             QStringLiteral("请先在 设置 →配置 中输入用户名。"));
        return;
    }
    saveConfig();

    setRunningUi(true);
    activeSide_ = 0;
    transferIncoming_ = false;
    roleLabel_->setText(QStringLiteral("角色：正在选举..."));

    app_.startAuto(config_);
}

void MainWindow::onRoleDetermined(bool isServer) {
    roleLabel_->setText(isServer ? QStringLiteral("角色：控制端")
                                 : QStringLiteral("角色：被控端"));
}

void MainWindow::onSessionStopped() {
    QMetaObject::invokeMethod(this, [this] {
        setRunningUi(false);
        roleLabel_->setText(QStringLiteral("角色：未连接"));
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
                                 QStringLiteral("请在本地文件列表中选择要上传的文件。"));
        return;
    }

    QString remoteDir = QDir::toNativeSeparators(remoteBrowser_->path());

    std::vector<std::string> paths;
    paths.reserve(selected.size());
    for (const QString& p : selected) paths.push_back(p.toStdString());

    uint64_t id = app_.sendFiles(paths, 0, remoteDir.toStdString());
    if (id == 0) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
                             QStringLiteral("启动文件传输失败。"));
        return;
    }
    currentTransferId_ = id;
    activeSide_ = 1;
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
                                 QStringLiteral("请在远程文件列表中选择要下载的文件。"));
        return;
    }

    QString remoteDir = remoteBrowser_->path();

    std::vector<std::string> remotePaths;
    remotePaths.reserve(selected.size());
    for (const QString& name : selected) {
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
    activeSide_ = 2;
    transferIncoming_ = false;
    remoteBrowser_->showProgress(QStringLiteral("正在下载 %1 个项目...").arg(selected.size()));
}

void MainWindow::onRefreshLocal() {
    // QFileSystemModel auto-refreshes, but force a re-read of the current directory.
    QString p = localBrowser_->path();
    if (!p.isEmpty()) localBrowser_->setPath(p);
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
        e.isDrive = isRoot && e.isDirectory;
        e.modified = m[QStringLiteral("mtime")].toDateTime();
        dirEntries.push_back(e);
    }
    remoteBrowser_->setRemoteTreeEntries(path, dirEntries);
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
    if (currentTransferId_ == 0) {
        currentTransferId_ = id;
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
// File operations (local)
// ---------------------------------------------------------------------------

void MainWindow::onNewFolder() {
    QString dir = localBrowser_->path();
    if (dir.isEmpty()) return;

    bool ok = false;
    QString name = QInputDialog::getText(this,
        QStringLiteral("新建文件夹"),
        QStringLiteral("文件夹名称："),
        QLineEdit::Normal, QStringLiteral("新建文件夹"), &ok);
    if (!ok || name.isEmpty()) return;

    QString fullPath = dir;
    if (!fullPath.endsWith(QLatin1Char('\\')) && !fullPath.endsWith(QLatin1Char('/')))
        fullPath += QLatin1Char('\\');
    fullPath += name;

    QDir d;
    if (!d.mkpath(fullPath)) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
                             QStringLiteral("无法创建文件夹。"));
    }
}

void MainWindow::onNewFile() {
    QString dir = localBrowser_->path();
    if (dir.isEmpty()) return;

    bool ok = false;
    QString name = QInputDialog::getText(this,
        QStringLiteral("新建文件"),
        QStringLiteral("文件名称："),
        QLineEdit::Normal, QStringLiteral("新建文件.txt"), &ok);
    if (!ok || name.isEmpty()) return;

    QString fullPath = dir;
    if (!fullPath.endsWith(QLatin1Char('\\')) && !fullPath.endsWith(QLatin1Char('/')))
        fullPath += QLatin1Char('\\');
    fullPath += name;

    QFile f(fullPath);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
                             QStringLiteral("无法创建文件。"));
    }
    f.close();
}

void MainWindow::onDeleteSelected() {
    QStringList selected = localBrowser_->selectedPaths();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("零界 ZeroBorders"),
                                 QStringLiteral("请先选择要删除的文件。"));
        return;
    }

    auto reply = QMessageBox::question(this,
        QStringLiteral("确认删除"),
        QStringLiteral("确定要删除 %1 个项目吗？此操作不可恢复。").arg(selected.size()),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    for (const QString& path : selected) {
        QFileInfo fi(path);
        if (fi.isDir()) {
            QDir d(path);
            d.removeRecursively();
        } else {
            QFile::remove(path);
        }
    }
}

void MainWindow::onRenameSelected() {
    QStringList selected = localBrowser_->selectedPaths();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("零界 ZeroBorders"),
                                 QStringLiteral("请先选择要重命名的文件。"));
        return;
    }
    if (selected.size() > 1) {
        QMessageBox::information(this, QStringLiteral("零界 ZeroBorders"),
                                 QStringLiteral("一次只能重命名一个文件。"));
        return;
    }

    QString oldPath = selected.first();
    QFileInfo fi(oldPath);
    QString oldName = fi.fileName();

    bool ok = false;
    QString newName = QInputDialog::getText(this,
        QStringLiteral("重命名"),
        QStringLiteral("新名称："),
        QLineEdit::Normal, oldName, &ok);
    if (!ok || newName.isEmpty() || newName == oldName) return;

    QString newPath = fi.path();
    if (!newPath.endsWith(QLatin1Char('\\')) && !newPath.endsWith(QLatin1Char('/')))
        newPath += QLatin1Char('\\');
    newPath += newName;

    QFile f(oldPath);
    if (!f.rename(newPath)) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
                             QStringLiteral("重命名失败。"));
    }
}

void MainWindow::onCopyPath() {
    QStringList selected = localBrowser_->selectedPaths();
    if (selected.isEmpty()) {
        // Copy current directory path if nothing selected
        QString p = localBrowser_->path();
        if (!p.isEmpty()) QApplication::clipboard()->setText(p);
        return;
    }
    QApplication::clipboard()->setText(selected.join(QLatin1Char('\n')));
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
        statusLabel_->setStyleSheet("color: #2a7; font-weight: bold; padding: 0 8px;");
        if (peerW && peerH) {
            peerLabel_->setText(QString("对端：%1x%2").arg(peerW).arg(peerH));
            layoutWidget_->setPeerResolution(peerW, peerH);
        }
        layoutWidget_->setConnected(true);
        layoutWidget_->setPeerName(peerName);
        remoteBrowser_->setReadOnly(false);
        remoteBrowser_->clearRemoteEntries();
        if (trayIcon_) {
            trayIcon_->setIcon(makeTrayIcon(true));
            trayIcon_->setToolTip(QStringLiteral("零界 ZeroBorders（已连接）"));
        }
    } else {
        statusLabel_->setStyleSheet("color: #666; padding: 0 8px;");
        peerLabel_->setText("");
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
    QString color = "#333";
    if (tag == "WRN") color = "#b8860b";
    else if (tag == "ERR") color = "#c33";

    QString trimmed = line;
    trimmed.chop(1);

    logView_->appendHtml(QString("<span style='color:%1'>%2</span>")
                             .arg(color, trimmed.toHtmlEscaped()));
}

// ---------------------------------------------------------------------------
// Close behavior: X button minimizes to tray
// ---------------------------------------------------------------------------

void MainWindow::closeEvent(QCloseEvent* event) {
    if (quitting_) {
        saveConfig();
        event->accept();
        return;
    }

    // Always minimize to tray instead of quitting.
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        event->ignore();
        hide();
        trayIcon_->showMessage(QStringLiteral("零界 ZeroBorders"),
            QStringLiteral("程序已最小化到托盘，右键托盘图标可选择退出。"),
            QSystemTrayIcon::Information, 3000);
        return;
    }

    // No tray available: ask before quitting if session active.
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
// Settings callbacks
// ---------------------------------------------------------------------------

void MainWindow::onLocalPathChanged(const QString& path) {
    config_.receiveDir = QDir::toNativeSeparators(path).toStdString();
    saveConfig();
    if (app_.isRunning() && app_.isConnected()) {
        app_.notifyPathSync();
    }
}

void MainWindow::onToggleAutoStart(bool enabled) {
    if (!ConfigManager::instance().setAutoStart(enabled)) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
            QStringLiteral("无法修改开机自启，请尝试以管理员身份运行。"));
    }
}

void MainWindow::onLayoutChanged(ScreenLayout layout) {
    config_.layout = layoutToString(layout);
    saveConfig();
    setLayoutCollapsed(false);
    if (app_.isRunning() && app_.isConnected()) {
        app_.setLayout(layout);
    }
}

void MainWindow::onRemoteLayoutChanged(ScreenLayout layout) {
    layoutWidget_->setLayout(layout);
    config_.layout = layoutToString(layout);
    saveConfig();
    setLayoutCollapsed(false);
}

void MainWindow::onRemoteReceiveDirChanged(const QString& dir) {
    if (dir.isEmpty()) {
        remoteBrowser_->setPath(QStringLiteral("%TEMP%\\ZeroBorders（对端默认）"));
    } else {
        remoteBrowser_->setPath(dir);
    }
    if (app_.isRunning() && app_.isConnected()) {
        app_.requestRemoteDirList(dir.toStdString());
    }
}

// ---------------------------------------------------------------------------
// System tray
// ---------------------------------------------------------------------------

static QIcon makeTrayIcon(bool connected) {
    QPixmap pm(":/icons/app.png");
    if (pm.isNull()) {
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
    QColor badge = connected ? QColor(0x22, 0xc5, 0x5e)
                             : QColor(0x9c, 0x9c, 0x9c);
    p.setBrush(badge);
    p.drawEllipse(20, 20, 10, 10);
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
// Incoming file offer (auto-accepted, no dialog)
// ---------------------------------------------------------------------------

void MainWindow::onIncomingOffer(quint64 id, const QStringList& files, quint64 total) {
    Q_UNUSED(id)
    Q_UNUSED(files)
    Q_UNUSED(total)
}

} // namespace zb
