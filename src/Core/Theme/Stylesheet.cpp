#include "Core/Theme/Stylesheet.hpp"

#include "Core/Theme/Fonts.hpp"
#include "Core/Theme/Manager.hpp"
#include "Core/Theme/Tokens.hpp"

#include <QColor>

namespace Acheron {
namespace Core {
namespace Theme {

namespace {
QString hex(const QColor &c)
{
    return c.name(QColor::HexRgb);
}
} // namespace

QString buildStyleSheet()
{
    const Manager &m = Manager::instance();

    const QColor windowBg = m.color(Token::WindowBg);
    const QColor baseBg = m.color(Token::BaseBg);
    const QColor alternateBg = m.color(Token::AlternateBaseBg);
    const QColor buttonBg = m.color(Token::ButtonBg);
    const QColor tooltipBg = m.color(Token::TooltipBg);
    const QColor tooltipText = m.color(Token::TooltipText);
    const QColor windowText = m.color(Token::WindowText);
    const QColor primaryText = m.color(Token::PrimaryText);
    const QColor buttonText = m.color(Token::ButtonText);
    const QColor mutedText = m.color(Token::PlaceholderText);
    const QColor disabledText = m.color(Token::DisabledText);
    const QColor divider = m.color(Token::Divider);
    const QColor strongDivider = m.color(Token::StrongDivider);
    const QColor highlight = m.color(Token::Highlight);
    const QColor highlightedText = m.color(Token::HighlightedText);
    const QColor hover = m.color(Token::SurfaceHover);
    const QColor pressed = m.color(Token::SurfacePressed);

    constexpr qreal tooltipFontScale = 0.9;
    QString tooltipFontSize;
    const qreal uiPointSize = m.font(FontRole::Ui).pointSizeF();
    if (uiPointSize > 0)
        tooltipFontSize = QStringLiteral("  font-size: %1pt;").arg(uiPointSize * tooltipFontScale);

    QString qss = QStringLiteral(R"QSS(
QWidget {
  color: %primaryText%;
  selection-background-color: %highlight%;
  selection-color: %highlightedText%;
}
QMainWindow, QDialog { background: %windowBg%; }
QFrame[frameShape="4"], QFrame[frameShape="5"] { color: %divider%; }
QToolTip {
  background-color: %tooltipBg%;
  color: %tooltipText%;
  border: 1px solid %strongDivider%;
  border-radius: 5px;
  padding: 5px 8px;
%tooltipFontSize%
}
QPushButton {
  min-height: 30px;
  background: %buttonBg%;
  color: %buttonText%;
  border: 1px solid %divider%;
  border-radius: 6px;
  padding: 0 12px;
}
QPushButton:hover { background: %hover%; border-color: %strongDivider%; }
QPushButton:pressed, QPushButton:checked { background: %pressed%; border-color: %highlight%; }
QPushButton:focus { border: 1px solid %highlight%; }
QPushButton:disabled { color: %disabledText%; background: %baseBg%; border-color: %divider%; }
QToolButton {
  min-width: 28px;
  min-height: 28px;
  border: 1px solid transparent;
  border-radius: 6px;
  padding: 2px 6px;
}
QToolButton:hover { background: %hover%; border-color: %divider%; }
QToolButton:pressed, QToolButton:checked { background: %pressed%; }
QToolButton:focus { border-color: %highlight%; }
QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox,
QDateEdit, QTimeEdit, QDateTimeEdit, QComboBox {
  min-height: 30px;
  background: %baseBg%;
  color: %windowText%;
  border: 1px solid %divider%;
  border-radius: 6px;
  padding: 0 9px;
}
QTextEdit, QPlainTextEdit { padding: 7px 9px; }
QLineEdit:hover, QTextEdit:hover, QPlainTextEdit:hover, QSpinBox:hover,
QDoubleSpinBox:hover, QDateEdit:hover, QTimeEdit:hover, QDateTimeEdit:hover,
QComboBox:hover { border-color: %strongDivider%; }
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QSpinBox:focus,
QDoubleSpinBox:focus, QDateEdit:focus, QTimeEdit:focus, QDateTimeEdit:focus,
QComboBox:focus { border-color: %highlight%; }
QLineEdit:disabled, QTextEdit:disabled, QPlainTextEdit:disabled, QComboBox:disabled {
  color: %disabledText%; background: %windowBg%;
}
QComboBox::drop-down { width: 24px; border: none; }
QComboBox QAbstractItemView {
  background: %alternateBg%;
  border: 1px solid %strongDivider%;
  outline: 0;
  padding: 4px;
}
QAbstractItemView {
  background: %baseBg%;
  alternate-background-color: %alternateBg%;
  border: 1px solid %divider%;
  border-radius: 6px;
  outline: 0;
}
QAbstractItemView::item { min-height: 28px; }
QAbstractItemView::item:hover { background: %hover%; }
QAbstractItemView::item:selected { background: %pressed%; color: %windowText%; }
QMenu {
  background: %alternateBg%;
  border: 1px solid %strongDivider%;
  border-radius: 7px;
  padding: 5px;
}
QMenu::item { min-height: 28px; border-radius: 4px; padding: 2px 28px 2px 10px; }
QMenu::item:selected { background: %hover%; }
QMenu::separator { height: 1px; background: %divider%; margin: 5px 7px; }
QScrollBar:vertical { width: 10px; margin: 2px; background: transparent; }
QScrollBar:horizontal { height: 10px; margin: 2px; background: transparent; }
QScrollBar::handle { min-width: 32px; min-height: 32px; background: %strongDivider%; border-radius: 4px; }
QScrollBar::handle:hover { background: %mutedText%; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: transparent; border: none; }
QSlider::groove:horizontal { height: 4px; background: %divider%; border-radius: 2px; }
QSlider::sub-page:horizontal { background: %highlight%; border-radius: 2px; }
QSlider::handle:horizontal { width: 14px; margin: -5px 0; background: %windowText%; border: 2px solid %highlight%; border-radius: 7px; }
QSlider::handle:horizontal:hover { background: %highlightedText%; }
QSlider::handle:horizontal:focus { border-color: %highlightedText%; }
QTabWidget::pane { border: 1px solid %divider%; border-radius: 7px; }
QTabBar::tab { min-height: 30px; padding: 0 12px; color: %mutedText%; border-bottom: 2px solid transparent; }
QTabBar::tab:hover { color: %windowText%; background: %hover%; }
QTabBar::tab:selected { color: %windowText%; border-bottom-color: %highlight%; }
QGroupBox { margin-top: 14px; padding-top: 10px; border: 1px solid %divider%; border-radius: 7px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; color: %mutedText%; }
QSplitter::handle { background: %divider%; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
#ContentFrame { background: %alternateBg%; border: 1px solid %strongDivider%; border-radius: 10px; }
#channelTree, #serverRail, #chatView, #memberList { border: none; border-radius: 0; }
#channelTree, #serverRail, #memberList { background: %baseBg%; }
#chatView { background: %windowBg%; }
#memberList QScrollBar::handle:vertical { min-height: 40px; }
#MessageInput { background: %baseBg%; border: 1px solid %divider%; border-radius: 8px; padding: 8px 10px; }
#MessageInput:hover { border-color: %strongDivider%; }
#MessageInput:focus { border-color: %highlight%; }
#EmojiAutocompletePopup QListView { background: transparent; border: none; }
)QSS");

    qss.replace("%windowBg%", hex(windowBg));
    qss.replace("%baseBg%", hex(baseBg));
    qss.replace("%alternateBg%", hex(alternateBg));
    qss.replace("%buttonBg%", hex(buttonBg));
    qss.replace("%tooltipBg%", hex(tooltipBg));
    qss.replace("%tooltipText%", hex(tooltipText));
    qss.replace("%windowText%", hex(windowText));
    qss.replace("%primaryText%", hex(primaryText));
    qss.replace("%buttonText%", hex(buttonText));
    qss.replace("%mutedText%", hex(mutedText));
    qss.replace("%disabledText%", hex(disabledText));
    qss.replace("%divider%", hex(divider));
    qss.replace("%strongDivider%", hex(strongDivider));
    qss.replace("%highlight%", hex(highlight));
    qss.replace("%highlightedText%", hex(highlightedText));
    qss.replace("%hover%", hex(hover));
    qss.replace("%pressed%", hex(pressed));
    qss.replace("%tooltipFontSize%", tooltipFontSize);

    return qss;
}

} // namespace Theme
} // namespace Core
} // namespace Acheron
