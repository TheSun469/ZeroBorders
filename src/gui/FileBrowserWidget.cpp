#include "FileBrowserWidget.h"
#include "PathComboBox.h"
#include "RemotePathComboBox.h"
#include "../core/Log.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIdentityProxyModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSize>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace zb {

namespace {

// Proxy that translates QFileSystemModel column headers to Chinese.
class LocalizedFileModelProxy : public QIdentityProxyModel {
public:
    using QIdentityProxyModel::QIdentityProxyModel;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            switch (section) {
                case 0: return QStringLiteral("名称");
                case 1: return QStringLiteral("大小");
                case 2: return QStringLiteral("类型");
                case 3: return QStringLiteral("修改日期");
            }
        }
        return QIdentityProxyModel::headerData(section, orientation, role);
    }
};

// On Windows, std::filesystem treats narrow strings as ANSI; convert UTF-8.
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

} // namespace

FileBrowserWidget::FileBrowserWidget(Mode mode, QWidget* parent)
    : QWidget(parent), mode_(mode) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(4);

    // --- Navigation bar ---
    auto* navRow = new QHBoxLayout();
    navRow->setSpacing(4);

    backBtn_ = new QPushButton(QStringLiteral("←"), this);
    backBtn_->setFixedWidth(24);
    backBtn_->setFixedHeight(24);
    backBtn_->setStyleSheet(QStringLiteral("padding: 0px; font-size: 12pt;"));
    backBtn_->setToolTip(QStringLiteral("后退"));
    upBtn_ = new QPushButton(QStringLiteral("↑"), this);
    upBtn_->setFixedWidth(24);
    upBtn_->setFixedHeight(24);
    upBtn_->setStyleSheet(QStringLiteral("padding: 0px; font-size: 12pt;"));
    upBtn_->setToolTip(QStringLiteral("上一级"));

    navRow->addWidget(backBtn_);
    navRow->addWidget(upBtn_);

    if (mode == Mode::Local) {
        pathCombo_ = new PathComboBox(this);
        pathCombo_->setPlaceholderText(QStringLiteral("选择本地文件夹..."));
        connect(pathCombo_, &PathComboBox::pathChanged,
                this, [this](const QString& p) { navigateLocal(p); });
        navRow->addWidget(pathCombo_, 1);
    } else {
        remotePathCombo_ = new RemotePathComboBox(this);
        // 单击/双击树中的目录：导航到该目录
        connect(remotePathCombo_, &RemotePathComboBox::pathChanged,
                this, [this](const QString& p) { navigateRemote(p); });
        // 树需要懒加载某个目录：转发给上层发起网络请求
        connect(remotePathCombo_, &RemotePathComboBox::fetchRequested,
                this, &FileBrowserWidget::fetchRequested);
        navRow->addWidget(remotePathCombo_, 1);
    }
    mainLayout->addLayout(navRow);

    // --- File table ---
    table_ = new QTableView(this);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSortingEnabled(true);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionsMovable(true);
    table_->horizontalHeader()->setSectionsClickable(true);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setIconSize(QSize(14, 14));
    table_->verticalHeader()->setDefaultSectionSize(22);
    table_->verticalHeader()->setMinimumSectionSize(18);

    if (mode == Mode::Local) {
        localModel_ = new QFileSystemModel(this);
        localModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs);
        localModel_->setRootPath(QString());
        localProxy_ = new LocalizedFileModelProxy(this);
        localProxy_->setSourceModel(localModel_);
        table_->setModel(localProxy_);
        // QFileSystemModel columns: 0=Name, 1=Size, 2=Type, 3=Date modified
        // Show the four standard columns and hide everything else.
        for (int c = 0; c < localModel_->columnCount(); ++c) {
            table_->setColumnHidden(c, c > 3);
        }
    } else {
        remoteModel_ = new RemoteFileModel(this);
        table_->setModel(remoteModel_);
    }

    // Allow user to resize and reorder all columns.
    for (int c = 0; c < 4; ++c) {
        table_->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Interactive);
    }
    // Give the name column a reasonable default width.
    table_->horizontalHeader()->resizeSection(0, 200);
    table_->horizontalHeader()->resizeSection(1, 80);
    table_->horizontalHeader()->resizeSection(2, 100);
    table_->horizontalHeader()->resizeSection(3, 140);

    mainLayout->addWidget(table_, 1);

    // --- Progress bar (hidden by default, shown during transfer) ---
    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setVisible(false);
    progressLabel_ = new QLabel("", this);
    progressLabel_->setVisible(false);
    mainLayout->addWidget(progressBar_);
    mainLayout->addWidget(progressLabel_);

    // --- Connections ---
    connect(backBtn_, &QPushButton::clicked, this, &FileBrowserWidget::onBack);
    connect(upBtn_, &QPushButton::clicked, this, &FileBrowserWidget::onUp);
    connect(table_, &QTableView::doubleClicked,
            this, &FileBrowserWidget::onDoubleClicked);

    // Right-click context menu.
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableView::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        QModelIndex index = table_->indexAt(pos);

        QMenu menu(this);

        // File operations on selected rows.
        if (index.isValid()) {
            if (!table_->selectionModel()->isRowSelected(index.row(), index.parent())) {
                table_->selectRow(index.row());
            }
            if (!selectedPaths().isEmpty()) {
                menu.addAction(QStringLiteral("传输"), [this] { emit transferRequested(); });
                menu.addAction(QStringLiteral("复制文件地址"), [this] {
                    QStringList paths = selectedFullPaths();
                    if (!paths.isEmpty()) {
                        QApplication::clipboard()->setText(paths.join(QStringLiteral("\n")));
                    }
                });
                // Local-only file operations.
                if (mode_ == Mode::Local) {
                    menu.addSeparator();
                    menu.addAction(QStringLiteral("删除"), [this] { emit deleteRequested(); });
                    menu.addAction(QStringLiteral("重命名"), [this] { emit renameRequested(); });
                }
                menu.addSeparator();
            }
        }

        // "新建文件夹" and "刷新" always available (local new folder only).
        if (mode_ == Mode::Local) {
            menu.addAction(QStringLiteral("新建文件夹"), [this] { emit newFolderRequested(); });
        }
        menu.addAction(QStringLiteral("刷新"), [this] { emit refreshRequested(); });

        menu.exec(table_->viewport()->mapToGlobal(pos));
    });

    updateNavButtons();
}

void FileBrowserWidget::setPath(const QString& path) {
    if (mode_ == Mode::Local) {
        navigateLocal(path);
    } else {
        currentPath_ = path;
        if (remotePathCombo_) remotePathCombo_->setPath(path);
    }
}

QString FileBrowserWidget::path() const {
    if (mode_ == Mode::Local) {
        return pathCombo_ ? pathCombo_->path() : QString();
    }
    // Remote mode: currentPath_ is always the actual path (empty = root).
    return currentPath_;
}

void FileBrowserWidget::setRemoteEntries(const QString& path,
                                         const QVector<RemoteDirEntry>& entries) {
    currentPath_ = path;
    if (remotePathCombo_) remotePathCombo_->setPath(path);
    if (remoteModel_) remoteModel_->setEntries(entries);
    updateNavButtons();
}

void FileBrowserWidget::showRemoteError(const QString& path,
                                        const QString& message) {
    currentPath_ = path;
    if (remotePathCombo_) remotePathCombo_->setPath(path);
    if (remoteModel_) remoteModel_->clear();
    ZB_LOG_WARN("Remote list error for [{}]: {}",
                path.toStdString(), message.toStdString());
}

void FileBrowserWidget::clearRemoteEntries() {
    if (remoteModel_) remoteModel_->clear();
}

void FileBrowserWidget::setRemoteTreeEntries(const QString& path,
                                             const QVector<RemoteDirEntry>& entries) {
    if (remotePathCombo_) remotePathCombo_->setEntries(path, entries);
}

void FileBrowserWidget::clearRemoteTree() {
    if (remotePathCombo_) remotePathCombo_->clearTree();
}

void FileBrowserWidget::setReadOnly(bool ro) {
    if (pathCombo_) pathCombo_->setEnabled(!ro);
    if (remotePathCombo_) remotePathCombo_->setReadOnly(ro);
    table_->setEnabled(!ro);
}

QStringList FileBrowserWidget::selectedPaths() const {
    QStringList result;
    if (!table_ || !table_->selectionModel()) return result;

    const auto indexes = table_->selectionModel()->selectedRows();
    for (const auto& idx : indexes) {
        if (mode_ == Mode::Local && localModel_ && localProxy_) {
            QModelIndex srcIdx = localProxy_->mapToSource(idx);
            result << localModel_->filePath(srcIdx);
        } else if (mode_ == Mode::Remote && remoteModel_) {
            const auto* e = remoteModel_->entryAt(idx.row());
            if (e) result << e->name;
        }
    }
    return result;
}

QStringList FileBrowserWidget::selectedFullPaths() const {
    QStringList result;
    if (mode_ == Mode::Local) {
        result = selectedPaths();  // 本地模式已是完整路径
    } else if (mode_ == Mode::Remote) {
        const QString dir = currentPath_;
        const auto names = selectedPaths();
        for (const QString& name : names) {
            if (dir.isEmpty()) {
                result << name;  // 根目录下条目（如 "C:\\"）本身就是完整路径
            } else {
                QString full = dir;
                if (!full.endsWith(QLatin1Char('\\')) && !full.endsWith(QLatin1Char('/')))
                    full += QLatin1Char('\\');
                full += name;
                result << full;
            }
        }
    }
    return result;
}

void FileBrowserWidget::navigateLocal(const QString& dir) {
    if (dir.isEmpty() || !localModel_) return;

    // Verify directory exists (handle UTF-8 paths correctly).
    std::error_code ec;
    fs::path fp(utf8ToWide(dir.toStdString()));
    if (!fs::is_directory(fp, ec)) {
        ZB_LOG_WARN("Local path not a directory: {}", dir.toStdString());
        return;
    }

    if (!navigatingHistory_) {
        // Truncate forward history and append.
        while (history_.size() - 1 > historyPos_) history_.removeLast();
        history_.append(dir);
        historyPos_ = history_.size() - 1;
    }

    localRoot_ = localModel_->setRootPath(dir);
    table_->setRootIndex(localProxy_ ? localProxy_->mapFromSource(localRoot_) : localRoot_);
    if (pathCombo_) pathCombo_->setPath(dir);
    emit pathChanged(dir);
    updateNavButtons();
}

void FileBrowserWidget::navigateRemote(const QString& dir) {
    if (!navigatingHistory_) {
        while (history_.size() - 1 > historyPos_) history_.removeLast();
        history_.append(dir);
        historyPos_ = history_.size() - 1;
    }
    currentPath_ = dir;
    if (remotePathCombo_) remotePathCombo_->setPath(dir);
    // Immediately clear old content so the user sees instant feedback.
    if (remoteModel_) remoteModel_->clear();
    emit navigateRequested(dir);
    updateNavButtons();
}

void FileBrowserWidget::onUp() {
    QString p = path();
    QString parent = parentPath(p);
    if (parent == p) return;  // already at root

    if (mode_ == Mode::Local) {
        navigateLocal(parent);
    } else {
        // In remote mode, empty path means "root" (list all drives).
        navigateRemote(parent);
    }
}

void FileBrowserWidget::onBack() {
    if (historyPos_ <= 0) return;
    navigatingHistory_ = true;
    --historyPos_;
    QString target = history_[historyPos_];
    if (mode_ == Mode::Local) {
        navigateLocal(target);
    } else {
        navigateRemote(target);
    }
    navigatingHistory_ = false;
}

void FileBrowserWidget::onDoubleClicked(const QModelIndex& index) {
    if (!index.isValid()) return;

    if (mode_ == Mode::Local && localModel_) {
        QModelIndex srcIdx = localProxy_ ? localProxy_->mapToSource(index) : index;
        QString filePath = localModel_->filePath(srcIdx);
        if (localModel_->isDir(srcIdx)) {
            navigateLocal(filePath);
        } else {
            emit filesActivated(QStringList{filePath});
        }
    } else if (mode_ == Mode::Remote && remoteModel_) {
        const auto* e = remoteModel_->entryAt(index.row());
        if (!e) return;
        if (e->isDirectory) {
            // At root level, drive names like "C:\" are already full paths.
            if (currentPath_.isEmpty()) {
                navigateRemote(e->name);
            } else {
                // Use backslash on Windows for consistent path display.
                QString child = currentPath_;
                if (!child.endsWith(QLatin1Char('\\')))
                    child += QLatin1Char('\\');
                child += e->name;
                navigateRemote(child);
            }
        } else {
            emit filesActivated(QStringList{e->name});
        }
    }
}

void FileBrowserWidget::onPathSubmitted() {
    // Currently the path editor is read-only in remote mode; local mode uses
    // PathComboBox which handles its own submission.
}

QString FileBrowserWidget::parentPath(const QString& p) const {
    if (p.isEmpty()) return p;
    // Check if this is a drive root like "C:\" or "C:/".
    if (p.length() == 3 && p[1] == QLatin1Char(':') &&
        (p[2] == QLatin1Char('\\') || p[2] == QLatin1Char('/'))) {
        return QString();  // Parent of a drive root is the "computer" root (empty).
    }
    // Try QDir first (handles drive roots and Unix paths).
    QDir dir(p);
    if (dir.cdUp()) {
        QString up = dir.absolutePath();
        // If cdUp returns the same path, we're at a root.
        if (up == p || up.isEmpty()) return QString();
        return up;
    }
    // Fallback: strip last path segment.
    int slash = p.lastIndexOf(QLatin1Char('/'));
    int bslash = p.lastIndexOf(QLatin1Char('\\'));
    int pos = qMax(slash, bslash);
    if (pos <= 0) return p;
    // Preserve root like "C:/"
    if (pos == 2 && p[1] == QLatin1Char(':')) return p.left(3);
    return p.left(pos);
}

void FileBrowserWidget::updateNavButtons() {
    bool canBack = historyPos_ > 0;
    bool canUp = false;
    QString p = path();
    if (!p.isEmpty()) {
        canUp = (parentPath(p) != p);
    }
    if (backBtn_) backBtn_->setEnabled(canBack);
    if (upBtn_) upBtn_->setEnabled(canUp);
}

void FileBrowserWidget::showProgress(const QString& label) {
    progressBar_->setValue(0);
    progressBar_->setVisible(true);
    progressLabel_->setVisible(true);
    progressLabel_->setText(label);
}

void FileBrowserWidget::updateProgress(int pct, const QString& label) {
    progressBar_->setValue(pct);
    progressLabel_->setText(label);
}

void FileBrowserWidget::hideProgress() {
    // Delay hiding so user can see the final state.
    QTimer::singleShot(3000, this, [this] {
        progressBar_->setVisible(false);
        progressLabel_->setVisible(false);
    });
}

} // namespace zb
