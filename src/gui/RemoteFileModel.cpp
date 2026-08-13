#include "RemoteFileModel.h"

#include <QApplication>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QIcon>
#include <QLocale>
#include <QStyle>

#include <algorithm>

namespace zb {

RemoteFileModel::RemoteFileModel(QObject* parent)
    : QAbstractTableModel(parent), iconProvider_(new QFileIconProvider) {}

RemoteFileModel::~RemoteFileModel() {
    delete iconProvider_;
}

int RemoteFileModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : entries_.size();
}

int RemoteFileModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColCount;
}

QVariant RemoteFileModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= entries_.size())
        return {};

    const auto& e = entries_[index.row()];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case ColName:
                return e.name;
            case ColSize:
                if (e.isDirectory) return QString();
                return QLocale::system().formattedDataSize(
                    static_cast<qint64>(e.size), 2, QLocale::DataSizeTraditionalFormat);
            case ColType:
                if (e.isDrive) return QStringLiteral("驱动器");
                if (e.isDirectory) return QStringLiteral("文件夹");
                {
                    QFileInfo fi(e.name);
                    QString suffix = fi.suffix().toUpper();
                    return suffix.isEmpty() ? QStringLiteral("文件") : suffix + QStringLiteral(" 文件");
                }
            case ColModified:
                return e.modified.toString(QStringLiteral("yyyy/MM/dd HH:mm"));
        }
    } else if (role == Qt::DecorationRole && index.column() == ColName) {
        // Use QFileIconProvider for proper Windows system icons.
        if (e.isDrive) {
            return iconProvider_->icon(QFileIconProvider::Drive);
        }
        if (e.isDirectory) {
            return iconProvider_->icon(QFileIconProvider::Folder);
        }
        // For files, use the extension to get a type-specific icon.
        QFileInfo fi(e.name);
        return iconProvider_->icon(fi);
    } else if (role == Qt::UserRole) {
        // Return true if directory, for the view to know.
        return e.isDirectory;
    } else if (role == Qt::UserRole + 1) {
        return e.name;
    }
    return {};
}

QVariant RemoteFileModel::headerData(int section, Qt::Orientation orientation,
                                     int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
        case ColName:     return QStringLiteral("名称");
        case ColSize:     return QStringLiteral("大小");
        case ColType:     return QStringLiteral("类型");
        case ColModified: return QStringLiteral("修改时间");
    }
    return {};
}

void RemoteFileModel::setEntries(const QVector<RemoteDirEntry>& entries) {
    beginResetModel();
    entries_ = entries;
    // Directories first, then files; each group sorted by name.
    std::sort(entries_.begin(), entries_.end(),
              [](const RemoteDirEntry& a, const RemoteDirEntry& b) {
                  if (a.isDirectory != b.isDirectory) return a.isDirectory;
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });
    endResetModel();
}

void RemoteFileModel::clear() {
    beginResetModel();
    entries_.clear();
    endResetModel();
}

const RemoteDirEntry* RemoteFileModel::entryAt(int row) const {
    if (row < 0 || row >= entries_.size()) return nullptr;
    return &entries_[row];
}

} // namespace zb
