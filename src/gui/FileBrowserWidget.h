#pragma once

#include "RemoteFileModel.h"

#include <QWidget>
#include <QString>
#include <QStringList>

class QPushButton;
class QLineEdit;
class QTableView;
class QProgressBar;
class QLabel;
class QFileSystemModel;
class QIdentityProxyModel;

namespace zb {

class PathComboBox;
class RemotePathComboBox;

// A file browser panel resembling Xftp / Explorer:
//   - navigation bar (back, up, path editor)
//   - file table (name / size / type / modified)
//
// In Local mode the panel browses the local filesystem directly via
// QFileSystemModel. In Remote mode it displays entries supplied by the
// caller through setRemoteEntries() and emits navigateRequested() when the
// user wants to enter a directory (the caller performs the network request
// and then calls setRemoteEntries()).
class FileBrowserWidget : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Local, Remote };

    explicit FileBrowserWidget(Mode mode, QWidget* parent = nullptr);

    // Local: set current root path and navigate.
    // Remote: update displayed path text (does not trigger network).
    void setPath(const QString& path);
    QString path() const;

    // Remote mode: populate the table with directory entries.
    void setRemoteEntries(const QString& path, const QVector<RemoteDirEntry>& entries);
    void showRemoteError(const QString& path, const QString& message);
    // Clear remote table without changing the path text (used on reconnect).
    void clearRemoteEntries();
    // Remote mode: populate the path dropdown tree (cache for browsing).
    void setRemoteTreeEntries(const QString& path, const QVector<RemoteDirEntry>& entries);
    // Clear the remote path tree cache.
    void clearRemoteTree();

    // When read-only, the path editor cannot be edited and selection is
    // disabled (used when disconnected).
    void setReadOnly(bool ro);

    // Get currently selected file/folder paths (full paths).
    QStringList selectedPaths() const;
    // Get full paths for selected items (remote mode prepends current dir).
    QStringList selectedFullPaths() const;

    // Progress bar: show/hide and update for this side's transfer.
    void showProgress(const QString& label);
    void updateProgress(int pct, const QString& label);
    void hideProgress();

signals:
    // Emitted in Local mode when the user changes the current directory.
    void pathChanged(const QString& path);
    // Emitted when the user wants to navigate to a directory.
    void navigateRequested(const QString& path);
    // Emitted when the remote path tree needs to load a directory's contents.
    void fetchRequested(const QString& path);
    // Emitted when the user double-clicks one or more files (not folders).
    void filesActivated(const QStringList& paths);
    // Emitted when the user right-clicks and selects "传输".
    void transferRequested();
    // Emitted when the user right-clicks and selects "刷新".
    void refreshRequested();
    // Emitted when the user right-clicks and selects "删除" (local only).
    void deleteRequested();
    // Emitted when the user right-clicks and selects "重命名" (local only).
    void renameRequested();
    // Emitted when the user right-clicks and selects "新建文件夹" (local only).
    void newFolderRequested();

private slots:
    void onUp();
    void onBack();
    void onDoubleClicked(const QModelIndex& index);
    void onPathSubmitted();

private:
    void navigateLocal(const QString& dir);
    void navigateRemote(const QString& dir);
    QString parentPath(const QString& p) const;
    void updateNavButtons();

    Mode mode_;
    QPushButton* backBtn_ = nullptr;
    QPushButton* upBtn_ = nullptr;
    PathComboBox* pathCombo_ = nullptr;       // local mode only
    RemotePathComboBox* remotePathCombo_ = nullptr; // remote mode only
    QTableView* table_ = nullptr;

    // Progress bar for this panel's transfers.
    QProgressBar* progressBar_ = nullptr;
    QLabel* progressLabel_ = nullptr;

    // Local
    QFileSystemModel* localModel_ = nullptr;
    QIdentityProxyModel* localProxy_ = nullptr;
    QModelIndex localRoot_;

    // Remote
    RemoteFileModel* remoteModel_ = nullptr;
    QString currentPath_;

    // Navigation history
    QStringList history_;
    int historyPos_ = -1;
    bool navigatingHistory_ = false;
};

} // namespace zb
