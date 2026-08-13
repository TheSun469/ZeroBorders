#pragma once

#include "RemoteFileModel.h"  // RemoteDirEntry

#include <QAbstractItemModel>
#include <QHash>
#include <QString>
#include <QVector>

class QFileIconProvider;

namespace zb {

// 懒加载的远程目录树模型，用于 RemotePathComboBox 的下拉树。
// 根节点为"我的电脑"，其子节点为驱动器；展开某个目录时通过 fetchRequested
// 信号请求对端返回该目录内容，调用 setEntries() 填充。
class RemoteDirTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit RemoteDirTreeModel(QObject* parent = nullptr);
    ~RemoteDirTreeModel() override;

    // QAbstractItemModel interface
    QModelIndex index(int row, int column,
                      const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool hasChildren(const QModelIndex& parent = QModelIndex()) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    // 填充某个路径的子目录条目（由对端 ListDirResponse 触发）。
    void setEntries(const QString& path, const QVector<RemoteDirEntry>& entries);

    // 返回 index 对应节点的完整远程路径。
    QString filePath(const QModelIndex& index) const;

    // 根据路径查找对应的 model index（用于展开/定位当前路径）。
    QModelIndex indexForPath(const QString& path) const;

    // 清空所有缓存（断开连接时调用）。
    void clear();

signals:
    // 某个目录节点需要加载子目录时发出。
    void fetchRequested(const QString& path);

private:
    struct Node {
        QString name;         // 显示名称（如 "C:" 或 "Documents"）
        QString fullPath;     // 完整远程路径（如 "C:\\" 或 "C:\\Documents"）
        bool isDrive = false; // 驱动器节点
        bool loaded = false;  // 是否已加载过子目录
        bool loading = false; // 正在请求中
        Node* parent = nullptr;
        QVector<Node*> children;
    };

    Node* nodeForIndex(const QModelIndex& idx) const;
    QModelIndex indexForNode(Node* node) const;
    Node* nodeForPath(const QString& path) const;
    Node* findChild(Node* parent, const QString& name) const;
    void clearNode(Node* node);
    // 把 "C:\Documents" 拆成 ["C:\\", "Documents"]
    static QStringList splitPath(const QString& path);
    // 拼接子路径，统一使用反斜杠
    static QString joinPath(const QString& parent, const QString& name);

    Node* root_;
    QFileIconProvider* iconProvider_ = nullptr;
};

} // namespace zb
