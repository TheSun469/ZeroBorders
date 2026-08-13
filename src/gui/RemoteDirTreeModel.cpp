#include "RemoteDirTreeModel.h"

#include <QApplication>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QIcon>
#include <QRegularExpression>
#include <QStyle>

namespace zb {

RemoteDirTreeModel::RemoteDirTreeModel(QObject* parent)
    : QAbstractItemModel(parent),
      root_(new Node),
      iconProvider_(new QFileIconProvider) {
    root_->name = QStringLiteral("我的电脑");
    root_->fullPath = QString();  // 空路径 = 根
}

RemoteDirTreeModel::~RemoteDirTreeModel() {
    clearNode(root_);
    delete root_;
    delete iconProvider_;
}

QModelIndex RemoteDirTreeModel::index(int row, int column,
                                      const QModelIndex& parent) const {
    if (!hasIndex(row, column, parent)) return {};
    Node* p = nodeForIndex(parent);
    if (row < 0 || row >= p->children.size()) return {};
    return createIndex(row, column, p->children[row]);
}

QModelIndex RemoteDirTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) return {};
    Node* n = static_cast<Node*>(child.internalPointer());
    if (!n || n->parent == root_ || n->parent == nullptr) return {};
    return indexForNode(n->parent);
}

int RemoteDirTreeModel::rowCount(const QModelIndex& parent) const {
    Node* p = nodeForIndex(parent);
    return p->children.size();
}

int RemoteDirTreeModel::columnCount(const QModelIndex&) const {
    return 1;
}

QVariant RemoteDirTreeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    Node* n = static_cast<Node*>(index.internalPointer());
    if (!n) return {};

    if (role == Qt::DisplayRole) {
        return n->name;
    } else if (role == Qt::DecorationRole) {
        if (n->isDrive) {
            return iconProvider_->icon(QFileIconProvider::Drive);
        }
        return iconProvider_->icon(QFileIconProvider::Folder);
    } else if (role == Qt::UserRole) {
        return n->fullPath;
    }
    return {};
}

Qt::ItemFlags RemoteDirTreeModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

bool RemoteDirTreeModel::hasChildren(const QModelIndex& parent) const {
    Node* p = nodeForIndex(parent);
    if (p == root_) return true;  // 根节点始终显示可展开（有驱动器）
    // 驱动器或目录：未加载时也显示展开箭头
    return p->children.size() > 0 || !p->loaded;
}

bool RemoteDirTreeModel::canFetchMore(const QModelIndex& parent) const {
    Node* p = nodeForIndex(parent);
    return !p->loaded && !p->loading;
}

void RemoteDirTreeModel::fetchMore(const QModelIndex& parent) {
    Node* p = nodeForIndex(parent);
    if (p->loaded || p->loading) return;
    p->loading = true;
    emit fetchRequested(p->fullPath);
}

void RemoteDirTreeModel::setEntries(const QString& path,
                                    const QVector<RemoteDirEntry>& entries) {
    Node* node = nodeForPath(path);
    if (!node) return;

    // 如果已有子节点，先移除
    if (!node->children.isEmpty()) {
        beginRemoveRows(indexForNode(node), 0, node->children.size() - 1);
        qDeleteAll(node->children);
        node->children.clear();
        endRemoveRows();
    }

    // 只添加目录（树中不显示文件，与本地 PathComboBox 一致）
    QVector<Node*> newChildren;
    for (const auto& e : entries) {
        if (!e.isDirectory) continue;
        auto* child = new Node;
        child->name = e.name;
        // 驱动器名称如 "C:\\" 本身就是完整路径
        child->fullPath = path.isEmpty() ? e.name : joinPath(path, e.name);
        child->isDrive = path.isEmpty();
        child->parent = node;
        newChildren.append(child);
    }

    if (!newChildren.isEmpty()) {
        beginInsertRows(indexForNode(node), 0, newChildren.size() - 1);
        node->children = newChildren;
        endInsertRows();
    }

    node->loaded = true;
    node->loading = false;
}

QString RemoteDirTreeModel::filePath(const QModelIndex& index) const {
    Node* n = nodeForIndex(index);
    return n->fullPath;
}

QModelIndex RemoteDirTreeModel::indexForPath(const QString& path) const {
    Node* n = nodeForPath(path);
    if (!n || n == root_) return {};
    return indexForNode(n);
}

void RemoteDirTreeModel::clear() {
    beginResetModel();
    clearNode(root_);
    root_->children.clear();
    root_->loaded = false;
    root_->loading = false;
    endResetModel();
}

// ---- private ----

RemoteDirTreeModel::Node* RemoteDirTreeModel::nodeForIndex(const QModelIndex& idx) const {
    if (!idx.isValid()) return root_;
    return static_cast<Node*>(idx.internalPointer());
}

QModelIndex RemoteDirTreeModel::indexForNode(Node* node) const {
    if (!node || node == root_ || !node->parent) return {};
    int row = node->parent->children.indexOf(node);
    if (row < 0) return {};
    return createIndex(row, 0, node);
}

RemoteDirTreeModel::Node* RemoteDirTreeModel::nodeForPath(const QString& path) const {
    if (path.isEmpty()) return root_;
    QStringList parts = splitPath(path);
    Node* current = root_;
    for (const QString& part : parts) {
        Node* next = findChild(current, part);
        if (!next) return nullptr;
        current = next;
    }
    return current;
}

RemoteDirTreeModel::Node* RemoteDirTreeModel::findChild(Node* parent,
                                                       const QString& name) const {
    for (Node* child : parent->children) {
        if (child->name.compare(name, Qt::CaseInsensitive) == 0)
            return child;
    }
    return nullptr;
}

void RemoteDirTreeModel::clearNode(Node* node) {
    for (Node* child : node->children) {
        clearNode(child);
        delete child;
    }
    node->children.clear();
}

QStringList RemoteDirTreeModel::splitPath(const QString& path) {
    // 把 "C:\\Documents\\Sub" 拆成 ["C:\\", "Documents", "Sub"]
    // 注意：驱动器根保留原始反斜杠格式（如 "C:\\"），以匹配节点 name。
    QStringList result;
    QString p = path;
    if (p.size() >= 2 && p[1] == QLatin1Char(':')) {
        if (p.size() >= 3) {
            result.append(p.left(3));  // "C:\\" 或 "C:/"（保留原斜杠方向）
            p = p.mid(3);
        } else {
            result.append(p);  // 仅 "C:"
            p.clear();
        }
    }
    // 剩余部分按 \\ 或 / 分割
    const QStringList parts = p.split(QRegularExpression(QStringLiteral("[\\\\/]")),
                                      Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        result.append(part);
    }
    return result;
}

QString RemoteDirTreeModel::joinPath(const QString& parent, const QString& name) {
    if (parent.isEmpty()) return name;
    QString p = parent;
    if (!p.endsWith(QLatin1Char('\\')) && !p.endsWith(QLatin1Char('/')))
        p += QLatin1Char('\\');
    return p + name;
}

} // namespace zb
