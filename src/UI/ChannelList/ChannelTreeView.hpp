#pragma once

#include <QTreeView>
#include <QHash>

#include <functional>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace UI {

class ChannelTreeView : public QTreeView
{
    Q_OBJECT
public:
    ChannelTreeView(QWidget *parent = nullptr);

    void setModel(QAbstractItemModel *m) override;
    void performDefaultExpansion();

    void setAccountVoiceChannel(Core::Snowflake accountId, Core::Snowflake channelId);
    [[nodiscard]] bool isAccountInVoice(Core::Snowflake accountId) const;
    void setNotificationSoundMuteResolver(
            std::function<bool(Core::Snowflake, Core::Snowflake)> resolver);

signals:
    void markAsReadRequested(const QModelIndex &proxyIndex);
    void openInNewTabRequested(const QModelIndex &proxyIndex);
    void joinVoiceChannelRequested(const QModelIndex &proxyIndex);
    void disconnectVoiceRequested(const QModelIndex &proxyIndex);
    void joinThreadRequested(const QModelIndex &proxyIndex);
    void leaveThreadRequested(const QModelIndex &proxyIndex);
    void leaveGuildRequested(Core::Snowflake accountId, Core::Snowflake guildId);
    void notificationSoundMuteRequested(Core::Snowflake accountId,
                                        Core::Snowflake channelId, bool muted);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

    bool handleMouseEventForExpansion(QMouseEvent *event);

    void onRowsInserted(const QModelIndex &parent, int first, int last);

    Core::Snowflake findAccountIdForIndex(const QModelIndex &sourceIndex) const;

    QHash<Core::Snowflake, Core::Snowflake> accountVoiceChannels; // accountId -> channelId
    std::function<bool(Core::Snowflake, Core::Snowflake)> notificationSoundMuteResolver;
};

} // namespace UI
} // namespace Acheron
