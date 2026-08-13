#include "RemotePathComboBox.h"

#include <QFrame>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLineEdit>
#include <QScreen>
#include <QSize>
#include <QTreeView>
#include <QVBoxLayout>

namespace zb {

RemotePathComboBox::RemotePathComboBox(QWidget* parent) : QComboBox(parent) {
    setEditable(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    lineEdit()->setReadOnly(true);
    setPlaceholderText(QStringLiteral("未连接"));

    model_ = new RemoteDirTreeModel(this);
    connect(model_, &RemoteDirTreeModel::fetchRequested,
            this, &RemotePathComboBox::fetchRequested);
}

RemotePathComboBox::~RemotePathComboBox() {
    if (popup_) {
        popup_->hide();
        popup_->deleteLater();
    }
}

void RemotePathComboBox::setPath(const QString& path) {
    QString display = path.isEmpty() ? QStringLiteral("我的电脑") : path;
    lineEdit()->setText(display);
}

QString RemotePathComboBox::path() const {
    QString text = lineEdit()->text().trimmed();
    if (text == QStringLiteral("我的电脑")) return QString();
    return text;
}

void RemotePathComboBox::setReadOnly(bool ro) {
    readOnly_ = ro;
    lineEdit()->setReadOnly(true);  // 远程路径始终不可手动输入
    setEnabled(!ro);
}

void RemotePathComboBox::setEntries(const QString& path,
                                    const QVector<RemoteDirEntry>& entries) {
    model_->setEntries(path, entries);
}

void RemotePathComboBox::clearTree() {
    model_->clear();
}

void RemotePathComboBox::showPopup() {
    if (readOnly_) return;

    if (!popup_) {
        popup_ = new QFrame(nullptr, Qt::Popup);
        popup_->setFrameShape(QFrame::StyledPanel);
        popup_->setLineWidth(1);
        auto* layout = new QVBoxLayout(popup_);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        tree_ = new QTreeView(popup_);
        tree_->setModel(model_);
        tree_->setHeaderHidden(true);
        tree_->setRootIsDecorated(true);
        tree_->setItemsExpandable(true);
        tree_->setUniformRowHeights(true);
        tree_->setExpandsOnDoubleClick(false);
        tree_->setIconSize(QSize(14, 14));
        tree_->setRootIsDecorated(true);
        tree_->setIndentation(14);

        // 单击目录：更新路径文本，不关闭 popup
        connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex& idx) {
            if (!idx.isValid()) return;
            QString p = model_->filePath(idx);
            setPath(p);
            emit pathChanged(p);
        });

        // 双击目录：确认选择并关闭 popup
        connect(tree_, &QTreeView::doubleClicked, this, [this](const QModelIndex& idx) {
            if (!idx.isValid()) return;
            QString p = model_->filePath(idx);
            setPath(p);
            emit pathChanged(p);
            popup_->hide();
        });

        layout->addWidget(tree_);
    }

    // 首次打开时请求根目录（驱动器列表）
    if (model_->canFetchMore(QModelIndex())) {
        model_->fetchMore(QModelIndex());
    }

    // 展开并定位到当前路径
    QString current = path();
    if (!current.isEmpty()) {
        QModelIndex idx = model_->indexForPath(current);
        if (idx.isValid()) {
            tree_->setCurrentIndex(idx);
            tree_->scrollTo(idx, QAbstractItemView::PositionAtCenter);
            // 展开所有祖先
            QModelIndex p = idx.parent();
            while (p.isValid()) {
                tree_->expand(p);
                p = p.parent();
            }
        }
    }

    QRect btnRect = this->rect();
    QPoint bottomLeft = this->mapToGlobal(btnRect.bottomLeft());
    int popupWidth = qMax(this->width(), 420);
    int popupHeight = 320;

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
