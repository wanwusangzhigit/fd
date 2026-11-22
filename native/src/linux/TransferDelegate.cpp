// TransferDelegate.cpp — see TransferDelegate.h.
#include "TransferDelegate.h"

#include <QApplication>
#include <QPainter>
#include <QStyleOptionProgressBar>

namespace wfd {
namespace linuxgui {

TransferDelegate::TransferDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {}

void TransferDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const {
    if (index.column() != kProgressColumn) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    const int pct = index.data(static_cast<int>(RowRole::Pct)).toInt();
    const int tint = index.data(static_cast<int>(RowRole::StateTint)).toInt();

    // Draw the cell text via the base delegate first, keeping selection and
    // state coloring, then overlay the strip in the lower portion.
    QStyleOptionViewItem textOpt(option);
    QStyledItemDelegate::paint(painter, textOpt, index);

    QRect bar = option.rect.adjusted(4, 0, -4, -3);
    const int h = 7;
    bar.setTop(bar.bottom() - h);
    if (bar.width() <= 2 || bar.height() <= 0) return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);

    // Border + empty track.
    painter->setPen(QPen(QColor(150, 150, 150)));
    painter->setBrush(QColor(230, 230, 230));
    painter->drawRect(bar);

    // Fill.
    if (pct > 0) {
        QRect fill = bar.adjusted(1, 1, -1, -1);
        fill.setWidth(fill.width() * qBound(0, pct, 100) / 100);
        QColor fillColor(0, 120, 215);
        if (tint == 1) fillColor = QColor(0, 150, 0);
        else if (tint == 2) fillColor = QColor(190, 0, 0);
        else if (tint == 3) fillColor = QColor(130, 130, 130);
        painter->fillRect(fill, fillColor);
    }
    painter->restore();
}

} // namespace linuxgui
} // namespace wfd
