// TransferDelegate.h — paints the Progress column of the transfers table.
//
// Mirrors the Win32 GUI's NM_CUSTOMDRAW progress strip: a bordered two-tone
// bar plus a state-tinted text color. The percentage text lives in the same
// cell; the delegate draws the bar in the lower part of it.
#pragma once

#include <QStyledItemDelegate>

namespace wfd {
namespace linuxgui {

// Column index painted by this delegate (must match TransferColumn in the
// Win32 GuiApp: COL_PROGRESS == 4).
inline constexpr int kProgressColumn = 4;

// Roles the model stores in each table item (Qt::UserRole + n).
enum class RowRole {
    TaskId = Qt::UserRole + 1,
    Pct,
    IsSend,
    SavedPath,
    StateTint,   // 0=none, 1=green (completed), 2=red (failed), 3=gray (canceled/queued)
};

class TransferDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit TransferDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
};

} // namespace linuxgui
} // namespace wfd
