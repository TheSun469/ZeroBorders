#include "PathComboBox.h"

#include <QLineEdit>
#include <QFileSystemModel>
#include <QTreeView>
#include <QHeaderView>
#include <QDir>
#include <QFrame>
#include <QVBoxLayout>
#include <QGuiApplication>
#include <QScreen>

namespace zb {

PathComboBox::PathComboBox(QWidget* parent) : QComboBox(parent) {
    setEditable(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // 文件系统模型：仅目录，不含文件
    model_ = new QFileSystemModel(this);
    model_->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);
    model_->setRootPath(QString());  // 整个文件系统

    // 回车提交手输路径
    connect(lineEdit(), &QLineEdit::returnPressed, this, &PathComboBox::onReturnPressed);
}

PathComboBox::~PathComboBox() {
    if (popup_) {
        popup_->hide();
        popup_->deleteLater();
    }
}

void PathComboBox::setPath(const QString& path) {
    lineEdit()->setText(path);
}

QString PathComboBox::path() const {
    return lineEdit()->text().trimmed();
}

void PathComboBox::setReadOnly(bool ro) {
    readOnly_ = ro;
    lineEdit()->setReadOnly(ro);
}

void PathComboBox::onReturnPressed() {
    emit pathChanged(lineEdit()->text().trimmed());
}

void PathComboBox::showPopup() {
    if (readOnly_) return;

    // 首次调用时创建 popup（自定义 QFrame，不用 QComboBox 的 view 机制）
    // 这样单击不会关闭弹出窗口，只有双击或按 ESC / 点击外部才关闭
    if (!popup_) {
        popup_ = new QFrame(nullptr, Qt::Popup);
        popup_->setFrameShape(QFrame::StyledPanel);
        popup_->setLineWidth(1);
        auto* layout = new QVBoxLayout(popup_);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        tree_ = new QTreeView(popup_);
        tree_->setModel(model_);
        tree_->setRootIndex(model_->index(QString()));  // 根 = 所有驱动器
        tree_->setHeaderHidden(true);
        tree_->setRootIsDecorated(true);
        tree_->setItemsExpandable(true);
        tree_->setSortingEnabled(true);
        tree_->sortByColumn(0, Qt::AscendingOrder);
        tree_->setUniformRowHeights(true);
        tree_->setExpandsOnDoubleClick(false);  // 双击我们自己处理
        for (int c = 1; c < model_->columnCount(); ++c)
            tree_->setColumnHidden(c, true);

        // 单击目录：更新路径文本，不关闭 popup（允许继续展开浏览）
        connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex& idx) {
            if (!idx.isValid()) return;
            QString p = model_->filePath(idx);
            lineEdit()->setText(p);
            emit pathChanged(p);
        });

        // 双击目录：确认选择并关闭 popup
        connect(tree_, &QTreeView::doubleClicked, this, [this](const QModelIndex& idx) {
            if (!idx.isValid()) return;
            QString p = model_->filePath(idx);
            lineEdit()->setText(p);
            emit pathChanged(p);
            popup_->hide();
        });

        layout->addWidget(tree_);
    }

    // 确保根目录已加载（QFileSystemModel 是懒加载的）
    QModelIndex rootIdx = model_->index(QString());
    if (model_->canFetchMore(rootIdx)) {
        model_->fetchMore(rootIdx);
    }

    // 滚动到当前路径并展开父链
    QString current = lineEdit()->text().trimmed();
    if (!current.isEmpty()) {
        QModelIndex idx = model_->index(current);
        if (idx.isValid()) {
            tree_->setCurrentIndex(idx);
            tree_->scrollTo(idx, QAbstractItemView::PositionAtCenter);
            QModelIndex parent = idx.parent();
            while (parent.isValid()) {
                tree_->expand(parent);
                parent = parent.parent();
            }
        }
    }

    // 定位 popup 到控件下方
    QRect btnRect = this->rect();
    QPoint bottomLeft = this->mapToGlobal(btnRect.bottomLeft());
    int popupWidth = qMax(this->width(), 420);
    int popupHeight = 320;

    // 避免超出屏幕
    QScreen* screen = QGuiApplication::screenAt(bottomLeft);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect screenGeom = screen->availableGeometry();
        if (bottomLeft.x() + popupWidth > screenGeom.right())
            bottomLeft.setX(screenGeom.right() - popupWidth);
        if (bottomLeft.y() + popupHeight > screenGeom.bottom())
            bottomLeft.setY(this->mapToGlobal(btnRect.topLeft()).y() - popupHeight);
    }

    popup_->setGeometry(bottomLeft.x(), bottomLeft.y(), popupWidth, popupHeight);
    popup_->show();
    popup_->raise();
    tree_->setFocus();
}

} // namespace zb
