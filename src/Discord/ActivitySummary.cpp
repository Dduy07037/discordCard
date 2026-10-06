#include "ActivitySummary.hpp"

#include <QJsonArray>
#include <QStringList>

namespace Acheron {
namespace Discord {

namespace {

constexpr int MaxDisplayCharacters = 128;
constexpr int MaxTooltipCharacters = 512;

QString boundedString(QString text, int limit)
{
    text = text.simplified();
    if (text.size() <= limit)
        return text;

    text.truncate(qMax(1, limit - 1));
    text += QChar(0x2026);
    return text;
}

QString boundedText(const QJsonValue &value, int limit = MaxDisplayCharacters)
{
    return boundedString(value.toString(), limit);
}

int activityPriority(int type)
{
    switch (type) {
    case 1: // Streaming
        return 60;
    case 0: // Playing
        return 50;
    case 2: // Listening
        return 40;
    case 5: // Competing
        return 30;
    case 3: // Watching
        return 20;
    case 4: // Custom status
        return 10;
    default:
        return 0;
    }
}

QString preferredActivityField(const QJsonObject &activity)
{
    const QString name = boundedText(activity.value("name"));
    const QString state = boundedText(activity.value("state"));
    const QString details = boundedText(activity.value("details"));

    switch (activity.value("status_display_type").toInt(0)) {
    case 1:
        if (!state.isEmpty())
            return state;
        break;
    case 2:
        if (!details.isEmpty())
            return details;
        break;
    default:
        if (!name.isEmpty())
            return name;
        break;
    }

    if (!name.isEmpty())
        return name;
    if (!details.isEmpty())
        return details;
    return state;
}

QString displayText(const QJsonObject &activity)
{
    const int type = activity.value("type").toInt(-1);
    QString value = preferredActivityField(activity);

    if (type == 4) {
        value = boundedText(activity.value("state"));
        const QString emoji = boundedText(activity.value("emoji").toObject().value("name"), 24);
        if (!emoji.isEmpty())
            value = value.isEmpty() ? emoji : emoji + QLatin1Char(' ') + value;
        return boundedString(value, MaxDisplayCharacters);
    }

    if (value.isEmpty())
        return {};

    switch (type) {
    case 0:
        return boundedString(QStringLiteral("Playing %1").arg(value), MaxDisplayCharacters);
    case 1:
        return boundedString(QStringLiteral("Streaming %1").arg(value), MaxDisplayCharacters);
    case 2:
        return boundedString(QStringLiteral("Listening to %1").arg(value), MaxDisplayCharacters);
    case 3:
        return boundedString(QStringLiteral("Watching %1").arg(value), MaxDisplayCharacters);
    case 5:
        return boundedString(QStringLiteral("Competing in %1").arg(value), MaxDisplayCharacters);
    default:
        return boundedString(value, MaxDisplayCharacters);
    }
}

QString tooltipText(const QJsonObject &activity, const QString &display)
{
    QStringList lines;
    lines.append(display);

    const QString name = boundedText(activity.value("name"));
    const QString details = boundedText(activity.value("details"));
    const QString state = boundedText(activity.value("state"));

    auto appendUnique = [&lines](const QString &label, const QString &value) {
        if (value.isEmpty())
            return;
        for (const QString &line : lines) {
            if (line.contains(value, Qt::CaseInsensitive))
                return;
        }
        lines.append(label + value);
    };

    appendUnique(QStringLiteral("App: "), name);
    appendUnique(QStringLiteral("Details: "), details);
    appendUnique(QStringLiteral("Status: "), state);

    const QJsonArray partySize = activity.value("party").toObject().value("size").toArray();
    if (partySize.size() == 2) {
        lines.append(QStringLiteral("Party: %1/%2")
                             .arg(partySize.at(0).toInt())
                             .arg(partySize.at(1).toInt()));
    }

    QString tooltip = lines.join(QLatin1Char('\n'));
    if (tooltip.size() > MaxTooltipCharacters) {
        tooltip.truncate(MaxTooltipCharacters - 1);
        tooltip += QChar(0x2026);
    }
    return tooltip;
}

} // namespace

ActivitySummary summarizeActivities(const QJsonObject &presence)
{
    const QJsonArray activities = presence.value("activities").toArray();

    QJsonObject selected;
    int selectedPriority = -1;
    for (const QJsonValue &value : activities) {
        if (!value.isObject())
            continue;

        const QJsonObject activity = value.toObject();
        const int priority = activityPriority(activity.value("type").toInt(-1));
        const QString display = displayText(activity);
        if (display.isEmpty() || priority <= selectedPriority)
            continue;

        selected = activity;
        selectedPriority = priority;
    }

    ActivitySummary summary;
    if (selected.isEmpty())
        return summary;

    summary.text = displayText(selected);
    summary.tooltip = tooltipText(selected, summary.text);
    if (summary.tooltip == summary.text)
        summary.tooltip.clear();
    return summary;
}

} // namespace Discord
} // namespace Acheron
