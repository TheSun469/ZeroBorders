#pragma once

#include <QComboBox>
#include <QPointer>

class QFileSystemModel;
class QTreeView;
class QFrame;

namespace zb {

// Xftp 风格路径选择器：可编辑输入 + 下拉文件系统树。
// 单击目录：更新路径文本但不关闭下拉（允许继续浏览展开子目录）
// 双击目录：确认选择并关闭下拉
class PathComboBox : public QComboBox {
    Q_OBJECT
public:
    explicit PathComboBox(QWidget* parent = nullptr);
    ~PathComboBox() override;

    void setPath(const QString& path);
    QString path() const;
    void setReadOnly(bool ro);

signals:
    void pathChanged(const QString& path);

private:
    void onReturnPressed();
    void showPopup() override;

    QFileSystemModel* model_ = nullptr;
    QFrame* popup_ = nullptr;
    QTreeView* tree_ = nullptr;
    bool readOnly_ = false;
};

} // namespace zb
