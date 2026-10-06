#pragma once

#include <QJsonObject>
#include <QString>

namespace Acheron {
namespace Discord {

struct ActivitySummary
{
    QString text;
    QString tooltip;

    [[nodiscard]] bool isEmpty() const { return text.isEmpty(); }
};

ActivitySummary summarizeActivities(const QJsonObject &presence);

} // namespace Discord
} // namespace Acheron
