#include "MemberListDelegate.hpp"

#include <QPainter>
#include <QPainterPath>

#include "MemberListModel.hpp"
#include "Core/MemberListManager.hpp"

constexpr static int GroupHeight = 22;
constexpr static int MemberHeight = 34;
constexpr static int MemberActivityHeight = 48;
constexpr static int AvatarSize = 28;
constexpr static int AvatarRadius = 7;
constexpr static int HorizontalPadding = 8;
constexpr static int AvatarTextSpacing = 8;

namespace Acheron {
namespace UI {

MemberListDelegate::MemberListDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void MemberListDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                               const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    int itemType = index.data(MemberListModel::ItemTypeRole).toInt();

    if (itemType == static_cast<int>(Core::MemberListItem::Type::Group))
        paintGroup(painter, option, index);
    else if (itemType == static_cast<int>(Core::MemberListItem::Type::Member))
        paintMember(painter, option, index);
    else
        paintPlaceholder(painter, option);

    painter->restore();
}

QSize MemberListDelegate::sizeHint(const QStyleOptionViewItem &option,
                                   const QModelIndex &index) const
{
    int itemType = index.data(MemberListModel::ItemTypeRole).toInt();

    if (itemType == static_cast<int>(Core::MemberListItem::Type::Group))
        return QSize(option.rect.width(), GroupHeight);

    if (itemType == static_cast<int>(Core::MemberListItem::Type::Member) &&
        !index.data(MemberListModel::ActivityRole).toString().isEmpty()) {
        return QSize(option.rect.width(), MemberActivityHeight);
    }

    return QSize(option.rect.width(), MemberHeight);
}

void MemberListDelegate::paintGroup(QPainter *painter, const QStyleOptionViewItem &option,
                                    const QModelIndex &index) const
{
    QString groupName = index.data(MemberListModel::GroupNameRole).toString();
    int groupCount = index.data(MemberListModel::GroupCountRole).toInt();

    // separator except for the first
    if (index.row() > 0) {
        QColor sepColor = option.palette.mid().color();
        sepColor.setAlpha(60);
        painter->setPen(QPen(sepColor, 1));
        painter->drawLine(option.rect.left() + HorizontalPadding,
                          option.rect.top(),
                          option.rect.right() - HorizontalPadding,
                          option.rect.top());
    }

    QString text = groupName.toUpper() + QString::fromUtf8(" \u2014 ") + QString::number(groupCount);

    QFont font = option.font;
    font.setPixelSize(10);
    font.setWeight(QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.3);
    painter->setFont(font);

    painter->setPen(option.palette.color(QPalette::Disabled, QPalette::Text));

    QRect textRect = option.rect.adjusted(HorizontalPadding, 0, -HorizontalPadding, 0);
    textRect.setTop(textRect.top() + 6);
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
}

void MemberListDelegate::paintMember(QPainter *painter, const QStyleOptionViewItem &option,
                                     const QModelIndex &index) const
{
    if (option.state & QStyle::State_MouseOver) {
        QColor hoverColor = option.palette.highlight().color();
        hoverColor.setAlpha(30);
        painter->setPen(Qt::NoPen);
        painter->setBrush(hoverColor);
        painter->drawRoundedRect(option.rect.adjusted(HorizontalPadding / 2, 2,
                                                      -HorizontalPadding / 2, -2),
                                 6, 6);
    }

    int x = option.rect.left() + HorizontalPadding;
    int centerY = option.rect.top() + (option.rect.height() - AvatarSize) / 2;

    QPixmap avatar = index.data(MemberListModel::AvatarRole).value<QPixmap>();
    QRect avatarRect(x, centerY, AvatarSize, AvatarSize);

    if (!avatar.isNull()) {
        QPainterPath clipPath;
        clipPath.addRoundedRect(avatarRect, AvatarRadius, AvatarRadius);
        painter->save();
        painter->setClipPath(clipPath);
        painter->drawPixmap(avatarRect, avatar.scaled(AvatarSize, AvatarSize,
                                                      Qt::KeepAspectRatioByExpanding,
                                                      Qt::SmoothTransformation));
        painter->restore();
    } else {
        QColor defaultBg = option.palette.mid().color();
        defaultBg.setAlpha(100);
        painter->setBrush(defaultBg);
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(avatarRect, AvatarRadius, AvatarRadius);
    }

    x += AvatarSize + AvatarTextSpacing;
    int textWidth = option.rect.right() - x - HorizontalPadding;

    QString displayName = index.data(MemberListModel::UsernameRole).toString();
    QString activity = index.data(MemberListModel::ActivityRole).toString();
    QColor roleColor = index.data(MemberListModel::RoleColorRole).value<QColor>();

    QFont font = option.font;
    font.setPixelSize(12);
    font.setWeight(QFont::Medium);
    painter->setFont(font);

    QColor nameColor;
    if (roleColor.isValid())
        nameColor = roleColor;
    else
        nameColor = option.palette.color(QPalette::Text);

    painter->setPen(nameColor);

    QFontMetrics fm(font);
    QString elidedName = fm.elidedText(displayName, Qt::ElideRight, textWidth);
    if (activity.isEmpty()) {
        const QRect nameRect(x, option.rect.top(), textWidth, option.rect.height());
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);
        return;
    }

    const int textTop = option.rect.top() + 7;
    painter->drawText(QRect(x, textTop, textWidth, 17),
                      Qt::AlignLeft | Qt::AlignVCenter, elidedName);

    QFont activityFont = option.font;
    activityFont.setPixelSize(10);
    activityFont.setWeight(QFont::Normal);
    painter->setFont(activityFont);

    QColor activityColor = option.palette.color(QPalette::PlaceholderText);
    if (option.state & QStyle::State_MouseOver)
        activityColor = option.palette.color(QPalette::Text);
    painter->setPen(activityColor);

    QFontMetrics activityMetrics(activityFont);
    const QString elidedActivity = activityMetrics.elidedText(activity, Qt::ElideRight, textWidth);
    painter->drawText(QRect(x, textTop + 17, textWidth, 15),
                      Qt::AlignLeft | Qt::AlignVCenter, elidedActivity);
}

void MemberListDelegate::paintPlaceholder(QPainter *painter,
                                          const QStyleOptionViewItem &option) const
{
    QColor placeholderColor = option.palette.mid().color();
    placeholderColor.setAlpha(40);
    painter->setPen(Qt::NoPen);
    painter->setBrush(placeholderColor);

    int x = option.rect.left() + HorizontalPadding;
    int centerY = option.rect.top() + (option.rect.height() - AvatarSize) / 2;

    painter->drawRoundedRect(QRect(x, centerY, AvatarSize, AvatarSize),
                             AvatarRadius, AvatarRadius);

    x += AvatarSize + AvatarTextSpacing;
    int nameWidth = qMin(80, option.rect.right() - x - HorizontalPadding);
    int nameHeight = 10;
    int nameY = option.rect.top() + (option.rect.height() - nameHeight) / 2;
    painter->drawRoundedRect(QRect(x, nameY, nameWidth, nameHeight), 3, 3);
}

} // namespace UI
} // namespace Acheron
