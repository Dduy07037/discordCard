#include "Fonts.hpp"

#include <QApplication>
#include <QFontDatabase>

namespace Acheron {
namespace Core {
namespace Theme {

namespace {

QFont interfaceFont()
{
    const QStringList families = QFontDatabase::families();
    QFont font = QApplication::font();
    if (families.contains(QStringLiteral("Segoe UI Variable Text")))
        font.setFamily(QStringLiteral("Segoe UI Variable Text"));
    else if (families.contains(QStringLiteral("Segoe UI")))
        font.setFamily(QStringLiteral("Segoe UI"));
    font.setPointSizeF(9.5);
    font.setHintingPreference(QFont::PreferFullHinting);
    return font;
}

} // namespace

const std::vector<FontDescriptor> &fontRegistry()
{
    static const QFont uiFont = interfaceFont();
    static const std::vector<FontDescriptor> reg = {
        { FontRole::Ui, "font.ui", "Interface", uiFont },
        { FontRole::Message, "font.message", "Messages", uiFont },
        { FontRole::Code, "font.code", "Code",
          QFontDatabase::systemFont(QFontDatabase::FixedFont) },
    };
    return reg;
}

const FontDescriptor *findFontById(const QString &id)
{
    for (const FontDescriptor &d : fontRegistry()) {
        if (id == QLatin1String(d.id))
            return &d;
    }
    return nullptr;
}

const FontDescriptor &fontDescriptor(FontRole role)
{
    for (const FontDescriptor &d : fontRegistry()) {
        if (d.role == role)
            return d;
    }
    return fontRegistry().front(); // just in case
}

} // namespace Theme
} // namespace Core
} // namespace Acheron
