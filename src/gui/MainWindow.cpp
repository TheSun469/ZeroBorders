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

    app_.installLogSink();
    buildTrayIcon();
    roleLabel_->setText(QStringLiteral("本机角色：未连接"));
    ZB_LOG_INFO("ZeroBorders GUI ready");
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    // ---- Connection group ------------------------------------------------
    connGroup_ = new QGroupBox(QStringLiteral("连接"), central);
    auto* connLayout = new QGridLayout(connGroup_);

    auto* codeLabel = new QLabel(QStringLiteral("识别码："), connGroup_);
    codeEdit_ = new QLineEdit(connGroup_);
    codeEdit_->setPlaceholderText(QStringLiteral("两台电脑输入相同的识别码即可自动配对"));
    codeEdit_->setEchoMode(QLineEdit::Password);

    auto* rolePrefLabel = new QLabel(QStringLiteral("控制方式："), connGroup_);
    rolePrefCombo_ = new QComboBox(connGroup_);
    rolePrefCombo_->addItem(QStringLiteral("自动选择"), 0);
    rolePrefCombo_->addItem(QStringLiteral("本机控制对端"), 1);
    rolePrefCombo_->addItem(QStringLiteral("对端控制本机"), 2);
    rolePrefCombo_->setToolTip(QStringLiteral("选择鼠标键盘在哪台电脑上操作。冲突时自动选举。"));

    startBtn_ = new QPushButton(QStringLiteral("开始连接"), connGroup_);
    statusLabel_ = new QLabel(QStringLiteral("空闲"), connGroup_);
    statusLabel_->setStyleSheet("color: #666;");
    peerLabel_ = new QLabel("", connGroup_);
    roleLabel_ = new QLabel(QStringLiteral("本机角色：未连接"), connGroup_);
    roleLabel_->setStyleSheet("color: #236; font-weight: bold;");

    connLayout->addWidget(codeLabel, 0, 0);
    connLayout->addWidget(codeEdit_, 0, 1, 1, 3);
    connLayout->addWidget(rolePrefLabel, 1, 0);
    connLayout->addWidget(rolePrefCombo_, 1, 1, 1, 3);
    connLayout->addWidget(startBtn_, 2, 0);
    connLayout->addWidget(statusLabel_, 2, 1, 1, 2);
    connLayout->addWidget(peerLabel_, 2, 3);
    connLayout->addWidget(roleLabel_, 3, 0, 1, 4);

    root->addWidget(connGroup_);

    // ---- Settings group --------------------------------------------------
    settingsGroup_ = new QGroupBox(QStringLiteral("设置"), central);
    auto* setLayout = new QGridLayout(settingsGroup_);

    autoStartCheck_ = new QCheckBox(QStringLiteral("开机自动启动"), settingsGroup_);
    minimizeToTrayCheck_ = new QCheckBox(QStringLiteral("关闭窗口时最小化到托盘"), settingsGroup_);
    setLayout->addWidget(autoStartCheck_, 0, 0);
    setLayout->addWidget(minimizeToTrayCheck_, 0, 1);

    root->addWidget(settingsGroup_);

    // ---- Screen layout group ---------------------------------------------
    // Windows-Settings-style drag-to-arrange device layout canvas.
    layoutGroup_ = new QGroupBox(QStringLiteral("设备位置（拖拽对端矩形设置相对位置）"), central);
    auto* layLayout = new QVBoxLayout(layoutGroup_);
    layoutWidget_ = new DeviceLayoutWidget(layoutGroup_);
    layLayout->addWidget(layoutWidget_);
    root->addWidget(layoutGroup_);

    // ---- File transfer group ---------------------------------------------
    transferGroup_ = new QGroupBox(QStringLiteral("文件传输"), central);
    auto* transLayout = new QVBoxLayout(transferGroup_);

    // Path row: left = local receive dir, right = remote receive dir.
    auto* pathRow = new QGridLayout();
    pathRow->setColumnStretch(1, 1);
    pathRow->setColumnStretch(3, 1);

    auto* localPathLabel = new QLabel(QStringLiteral("本地路径："), transferGroup_);
    localPathCombo_ = new PathComboBox(transferGroup_);
    localPathCombo_->setPlaceholderText(QStringLiteral("默认：%TEMP%\\ZeroBorders"));

    auto* remotePathLabel = new QLabel(QStringLiteral("远程路径："), transferGroup_);
    remotePathCombo_ = new PathComboBox(transferGroup_);
    remotePathCombo_->setReadOnly(true);
    remotePathCombo_->setPlaceholderText(QStringLiteral("未连接"));

    pathRow->addWidget(localPathLabel, 0, 0);
    pathRow->addWidget(localPathCombo_, 0, 1);
    pathRow->addWidget(remotePathLabel, 0, 2);
    pathRow->addWidget(remotePathCombo_, 0, 3);
    transLayout->addLayout(pathRow);

    fileList_ = new QListWidget(transferGroup_);
    fileList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    transLayout->addWidget(fileList_);

    auto* btnRow = new QHBoxLayout();
    addFilesBtn_ = new QPushButton(QStringLiteral("添加文件..."), transferGroup_);
    clearFilesBtn_ = new QPushButton(QStringLiteral("清空列表"), transferGroup_);
    sendBtn_ = new QPushButton(QStringLiteral("发送"), transferGroup_);
    sendBtn_->setEnabled(false);
    btnRow->addWidget(addFilesBtn_);
    btnRow->addWidget(clearFilesBtn_);
    btnRow->addStretch();
    btnRow->addWidget(sendBtn_);
    transLayout->addLayout(btnRow);

    progressBar_ = new QProgressBar(transferGroup_);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressLabel_ = new QLabel("", transferGroup_);
    transLayout->addWidget(progressBar_);
    transLayout->addWidget(progressLabel_);

    root->addWidget(transferGroup_);

    // ---- Log --------------------------------------------------------------
    auto* logGroup = new QGroupBox(QStringLiteral("日志"), central);
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
    root->addWidget(logGroup, 1);

    setCentralWidget(central);

    // ---- Connections ------------------------------------------------------
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStartStop);
    connect(addFilesBtn_, &QPushButton::clicked, this, &MainWindow::onBrowseFiles);
    connect(clearFilesBtn_, &QPushButton::clicked, fileList_, &QListWidget::clear);
    connect(sendBtn_, &QPushButton::clicked, this, &MainWindow::onSendFiles);
    connect(clearLogBtn_, &QPushButton::clicked, logView_, &QPlainTextEdit::clear);
    connect(localPathCombo_, &PathComboBox::pathChanged, this, &MainWindow::onLocalPathChanged);
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

    localPathCombo_->setPath(QString::fromStdString(config_.receiveDir));
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
    c.rolePreference = rolePrefCombo_->currentData().toInt();

    c.layout = layoutToString(layoutWidget_->layout());

    c.receiveDir = localPathCombo_->path().toStdString();
    c.startMinimized = minimizeToTrayCheck_->isChecked();
    return c;
}

// ---------------------------------------------------------------------------
// UI state helpers
// ---------------------------------------------------------------------------

void MainWindow::setRunningUi(bool running) {
    codeEdit_->setEnabled(!running);
    rolePrefCombo_->setEnabled(!running);
    localPathCombo_->setEnabled(!running);
    startBtn_->setText(running ? QStringLiteral("断开连接") : QStringLiteral("开始连接"));
    sendBtn_->setEnabled(running && app_.isConnected());
    if (startStopAction_) startStopAction_->setText(running ? QStringLiteral("断开") : QStringLiteral("开始连接"));
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
    saveConfig();

    setRunningUi(true);
    progressBar_->setValue(0);
    progressLabel_->clear();
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

void MainWindow::onBrowseFiles() {
    QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择要发送的文件"), QString(),
        QStringLiteral("所有文件 (*.*)"));
    for (const QString& f : files) {
        fileList_->addItem(f);
    }
}

void MainWindow::onSendFiles() {
    if (!app_.isRunning() || !app_.isConnected()) {
        QMessageBox::information(this, QStringLiteral("零界 ZeroBorders"),
                                 QStringLiteral("尚未连接。"));
        return;
    }
    if (fileList_->count() == 0) {
        QMessageBox::information(this, QStringLiteral("零界 ZeroBorders"),
                                 QStringLiteral("请先添加要发送的文件。"));
        return;
    }

    std::vector<std::string> paths;
    for (int i = 0; i < fileList_->count(); ++i) {
        paths.push_back(fileList_->item(i)->text().toStdString());
    }

    uint64_t id = app_.sendFiles(paths);
    if (id == 0) {
        QMessageBox::warning(this, QStringLiteral("零界 ZeroBorders"),
                             QStringLiteral("启动文件传输失败。"));
        return;
    }
    currentTransferId_ = id;
    progressBar_->setValue(0);
    progressLabel_->setText(QStringLiteral("正在发送..."));
}

void MainWindow::onTransferProgress(quint64 id, quint64 transferred, quint64 total) {
    updateProgress(id, transferred, total);
}

void MainWindow::onTransferComplete(quint64 id, bool ok, const QString& msg) {
    if (id == currentTransferId_) {
        progressBar_->setValue(ok ? 100 : 0);
        progressLabel_->setText(ok ? QStringLiteral("传输完成")
                                   : (QStringLiteral("失败：") + msg));
    }
}

void MainWindow::updateProgress(quint64 id, quint64 transferred, quint64 total) {
    if (id != currentTransferId_ || total == 0) return;
    int pct = static_cast<int>((transferred * 100) / total);
    progressBar_->setValue(pct);
    double mbXfer = transferred / (1024.0 * 1024.0);
    double mbTotal = total / (1024.0 * 1024.0);
    progressLabel_->setText(QString::asprintf("%.2f / %.2f MB", mbXfer, mbTotal));
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
        sendBtn_->setEnabled(true);
        if (trayIcon_) {
            trayIcon_->setIcon(makeTrayIcon(true));
            trayIcon_->setToolTip(QStringLiteral("零界 ZeroBorders（已连接）"));
        }
    } else {
        statusLabel_->setStyleSheet("color: #666;");
        peerLabel_->setText("");
        sendBtn_->setEnabled(false);
        layoutWidget_->setConnected(false);
        layoutWidget_->setPeerName(QString());
        remotePathCombo_->setPath(QString());
        remotePathCombo_->setPlaceholderText(QStringLiteral("未连接"));
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
}

void MainWindow::onRemoteReceiveDirChanged(const QString& dir) {
    if (dir.isEmpty()) {
        remotePathCombo_->setPath(QStringLiteral("%TEMP%\\ZeroBorders（对端默认）"));
    } else {
        remotePathCombo_->setPath(dir);
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
    QString dirText = localPathCombo_->path().isEmpty()
        ? QStringLiteral("%TEMP%\\ZeroBorders")
        : localPathCombo_->path();
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
    } else {
        app_.rejectTransfer(static_cast<uint64_t>(id));
    }
}

} // namespace zb
