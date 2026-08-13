#pragma once

#include <QAbstractTableModel>
#include <QDateTime>
#include <QString>
#include <QVector>

class QFileIconProvider;

namespace zb {

// One entry in a remote directory listing (mirrors protocol DirEntry but
// uses Qt types for direct model use).
struct RemoteDirEntry {
    QString name;
    qulonglong size = 0;
    bool isDirectory = false;
    bool isDrive = false;
    QDateTime modified;
};

// Table model backing the remote file browser. Populated from ListDirResponse.
class RemoteFileModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColName = 0,
        ColSize,
        ColType,
        ColModified,
        ColCount
    };

    explicit RemoteFileModel(QObject* parent = nullptr);
    ~RemoteFileModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setEntries(const QVector<RemoteDirEntry>& entries);
    void clear();
    const RemoteDirEntry* entryAt(int row) const;

private:
    QVector<RemoteDirEntry> entries_;
    QFileIconProvider* iconProvider_ = nullptr;
};

} // namespace zb
