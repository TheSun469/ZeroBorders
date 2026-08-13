#pragma once

#include "RemoteDirTreeModel.h"

#include <QComboBox>
#include <QPointer>
#include <QString>

class QFrame;
class QTreeView;

namespace zb {

// Xftp 风格的远程路径选择器：只读文本框 + 下拉远程目录树。
// 与本地 PathComboBox 外观一致，但目录树数据来自对端（懒加载）。
// 单击目录：更新路径文本但不关闭下拉；双击：确认选择并关闭。
class RemotePathComboBox : public QComboBox {
    Q_OBJECT
public:
    explicit RemotePathComboBox(QWidget* parent = nullptr);
    ~RemotePathComboBox() override;

    void setPath(const QString& path);
    QString path() const;
    void setReadOnly(bool ro);

    // 由调用方在收到对端目录列表后填充树节点。
    void setEntries(const QString& path, const QVector<RemoteDirEntry>& entries);
    // 清空树缓存（断开连接时）。
    void clearTree();

signals:
    // 用户在树中单击/双击了某个目录（双击会随后关闭下拉）。
    void pathChanged(const QString& path);
    // 树需要加载某个目录的子项（转发自 RemoteDirTreeModel）。
    void fetchRequested(const QString& path);

private:
    void showPopup() override;

    RemoteDirTreeModel* model_ = nullptr;
    QFrame* popup_ = nullptr;
    QTreeView* tree_ = nullptr;
    bool readOnly_ = false;
};

} // namespace zb
