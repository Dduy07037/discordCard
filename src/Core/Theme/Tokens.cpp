#include "Tokens.hpp"

namespace Acheron {
namespace Core {
namespace Theme {

const std::vector<TokenDescriptor> &registry()
{
    static const std::vector<TokenDescriptor> reg = {
        // backgrounds
        { Token::WindowBg, "window.bg", "Window background", "Backgrounds", QColor(17, 19, 24),
          QPalette::Window, false },
        { Token::BaseBg, "base.bg", "Base", "Backgrounds", QColor(22, 25, 32),
          QPalette::Base, false },
        { Token::AlternateBaseBg, "base.alt", "Alternate base", "Backgrounds", QColor(28, 32, 40),
          QPalette::AlternateBase, false },
        { Token::ButtonBg, "button.bg", "Button", "Backgrounds", QColor(35, 40, 50),
          QPalette::Button, false },
        { Token::TooltipBg, "tooltip.bg", "Tooltip background", "Backgrounds", QColor(35, 40, 50),
          QPalette::ToolTipBase, false },
        { Token::SurfaceHover, "surface.hover", "Hover surface", "Backgrounds", QColor(42, 48, 59),
          std::nullopt, false },
        { Token::SurfacePressed, "surface.pressed", "Pressed surface", "Backgrounds", QColor(48, 55, 68),
          std::nullopt, false },

        // text
        { Token::WindowText, "text.window", "Window text", "Text", QColor(240, 242, 246),
          QPalette::WindowText, false },
        { Token::PrimaryText, "text.primary", "Primary text", "Text", QColor(216, 220, 227),
          QPalette::Text, false },
        { Token::ButtonText, "text.button", "Button text", "Text", QColor(240, 242, 246),
          QPalette::ButtonText, false },
        { Token::TooltipText, "text.tooltip", "Tooltip text", "Text", QColor(240, 242, 246),
          QPalette::ToolTipText, false },
        { Token::PlaceholderText, "text.placeholder", "Placeholder text", "Text",
          QColor(145, 153, 166), QPalette::PlaceholderText, false },
        { Token::DisabledText, "text.disabled", "Disabled text", "Text", QColor(103, 112, 126),
          std::nullopt, false },
        { Token::BrightText, "text.bright", "Bright text", "Text", QColor(139, 176, 255),
          QPalette::BrightText, false },

        // accents
        { Token::Highlight, "accent.highlight", "Highlight", "Accents",
          QColor(91, 140, 255), QPalette::Highlight, false },
        { Token::HighlightedText, "accent.highlightText", "Highlighted text", "Accents",
          QColor(255, 255, 255), QPalette::HighlightedText, false },
        { Token::LinkText, "accent.link", "Links", "Accents", QColor(131, 168, 255), QPalette::Link,
          false },
        { Token::Divider, "accent.divider", "Dividers", "Accents", QColor(47, 53, 65),
          QPalette::Mid, false },
        { Token::StrongDivider, "accent.strongDivider", "Strong dividers", "Accents", QColor(61, 69, 83),
          std::nullopt, false },
        { Token::Success, "status.success", "Success", "Status", QColor(64, 201, 128),
          std::nullopt, false },
        { Token::Warning, "status.warning", "Warning", "Status", QColor(240, 178, 74),
          std::nullopt, false },
        { Token::Danger, "status.danger", "Danger", "Status", QColor(242, 102, 109),
          std::nullopt, false },

        // extended chat elements
        { Token::ChatError, "chat.error", "Failed message", "Chat", QColor(242, 102, 109),
          std::nullopt, false },
        { Token::EmbedDefault, "chat.embedDefault", "Default embed color", "Chat",
          QColor(91, 140, 255), std::nullopt, true },
        { Token::MentionText, "chat.mentionText", "Mention text", "Chat", QColor(205, 219, 255),
          std::nullopt, false },
        { Token::MentionBg, "chat.mentionBg", "Mention background", "Chat",
          QColor(91, 140, 255, 64), std::nullopt, true },
    };
    return reg;
}

const TokenDescriptor *findById(const QString &id)
{
    for (const TokenDescriptor &d : registry()) {
        if (id == QLatin1String(d.id))
            return &d;
    }
    return nullptr;
}

const TokenDescriptor &descriptor(Token token)
{
    for (const TokenDescriptor &d : registry()) {
        if (d.token == token)
            return d;
    }
    return registry().front(); // just in case
}

} // namespace Theme
} // namespace Core
} // namespace Acheron
