// Aseprite
// Copyright (C) 2001-2017  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/app.h"
#include "app/cmd/set_palette.h"
#include "app/cmd_sequence.h"
#include "app/color.h"
#include "app/color_utils.h"
#include "app/commands/command.h"
#include "app/commands/params.h"
#include "app/console.h"
#include "app/context_access.h"
#include "app/document_undo.h"
#include "app/file_selector.h"
#include "app/ini_file.h"
#include "app/modules/editors.h"
#include "app/modules/gui.h"
#include "app/modules/palettes.h"
#include "app/pref/preferences.h"
#include "app/transaction.h"
#include "app/ui/color_bar.h"
#include "app/ui/color_sliders.h"
#include "app/ui/editor/editor.h"
#include "app/ui/hex_color_entry.h"
#include "app/ui/palette_view.h"
#include "app/ui/skin/skin_slider_property.h"
#include "app/ui/status_bar.h"
#include "app/ui/toolbar.h"
#include "app/ui_context.h"
#include "base/bind.h"
#include "base/fs.h"
#include "doc/image.h"
#include "doc/palette.h"
#include "doc/sprite.h"
#include "gfx/hsl.h"
#include "gfx/hsv.h"
#include "gfx/rgb.h"
#include "gfx/size.h"
#include "ui/graphics.h"
#include "ui/ui.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace app {

using namespace gfx;
using namespace ui;

enum { RGB_MODE, HSV_MODE, HSL_MODE };
enum { ABS_MODE, REL_MODE };

class PaletteEntryEditor : public Window {
public:
  PaletteEntryEditor();

  void setColor(const app::Color& color);

protected:
  bool onProcessMessage(Message* msg) override;

  void onExit();
  void onCloseWindow();
  void onFgBgColorChange(const app::Color& _color);
  void onColorSlidersChange(ColorSlidersChangeEvent& ev);
  void onColorHexEntryChange(const app::Color& color);
  void onColorTypeClick();
  void onChangeModeClick();

private:
  void selectColorType(app::Color::Type type);
  void setPaletteEntry(const app::Color& color);
  void setAbsolutePaletteEntryChannel(ColorSliders::Channel channel, const app::Color& color);
  void setRelativePaletteEntryChannel(ColorSliders::Channel channel, int delta);
  void setNewPalette(Palette* palette, const char* operationName);
  void updateCurrentSpritePalette(const char* operationName);
  void updateColorBar();
  void updateWidgetsFromSelectedEntries();
  void onPalChange();
  void resetRelativeInfo();
  void getPicks(PalettePicks& picks);

  app::Color::Type m_type;
  Box m_vbox;
  Box m_topBox;
  Box m_bottomBox;
  ButtonSet m_colorType;
  ButtonSet m_changeMode;
  HexColorEntry m_hexColorEntry;
  Label m_entryLabel;
  ColorSliders m_sliders;

  // This variable is used to avoid updating the m_hexColorEntry text
  // when the color change is generated from a
  // HexColorEntry::ColorChange signal. In this way we don't override
  // what the user is writting in the text field.
  bool m_disableHexUpdate;

  ui::Timer m_redrawTimer;
  bool m_redrawAll;

  // True if the palette change must be implant in the UndoHistory
  // (e.g. when two or more changes in the palette are made in short
  // time).
  bool m_implantChange;

  // True if the PaletteChange signal is generated by the same
  // PaletteEntryEditor instance.
  bool m_selfPalChange;

  obs::scoped_connection m_palChangeConn;

  // Palette used for relative changes.
  Palette m_fromPalette;
  std::map<ColorSliders::Channel, int> m_relDeltas;
};

static PaletteEntryEditor* g_window = NULL;

class PaletteEditorCommand : public Command {
public:
  PaletteEditorCommand();
  Command* clone() const override { return new PaletteEditorCommand(*this); }

protected:
  void onLoadParams(const Params& params) override;
  void onExecute(Context* context) override;
  bool onChecked(Context* context) override;

private:
  bool m_open;
  bool m_close;
  bool m_switch;
  bool m_background;
};

PaletteEditorCommand::PaletteEditorCommand()
  : Command("PaletteEditor",
            "Palette Editor",
            CmdRecordableFlag)
{
  m_open = true;
  m_close = false;
  m_switch = false;
  m_background = false;
}

void PaletteEditorCommand::onLoadParams(const Params& params)
{
  std::string target = params.get("target");
  if (target == "foreground") m_background = false;
  else if (target == "background") m_background = true;

  std::string open_str = params.get("open");
  if (open_str == "true") m_open = true;
  else m_open = false;

  std::string close_str = params.get("close");
  if (close_str == "true") m_close = true;
  else m_close = false;

  std::string switch_str = params.get("switch");
  if (switch_str == "true") m_switch = true;
  else m_switch = false;
}

void PaletteEditorCommand::onExecute(Context* context)
{
  // If this is the first time the command is execute...
  if (!g_window) {
    // If the command says "Close the palette editor" and it is not
    // created yet, we just do nothing.
    if (m_close)
      return;

    // If this is "open" or "switch", we have to create the frame.
    g_window = new PaletteEntryEditor();
  }
  // If the frame is already created and it's visible, close it (only in "switch" or "close" modes)
  else if (g_window->isVisible() && (m_switch || m_close)) {
    // Hide the frame
    g_window->closeWindow(NULL);
    return;
  }

  if (m_switch || m_open) {
    if (!g_window->isVisible()) {
      // Default bounds
      g_window->remapWindow();

      int width = MAX(g_window->bounds().w, ui::display_w()/2);
      g_window->setBounds(Rect(
          ui::display_w() - width - ToolBar::instance()->bounds().w,
          ui::display_h() - g_window->bounds().h - StatusBar::instance()->bounds().h,
          width, g_window->bounds().h));

      // Load window configuration
      load_window_pos(g_window, "PaletteEditor");
    }

    // Run the frame in background.
    g_window->openWindow();
    ColorBar::instance()->setPaletteEditorButtonState(true);
  }

  // Show the specified target color
  {
    app::Color color =
      (m_background ? Preferences::instance().colorBar.bgColor():
                      Preferences::instance().colorBar.fgColor());

    g_window->setColor(color);
  }
}

bool PaletteEditorCommand::onChecked(Context* context)
{
  if(!g_window)
  {
    return false;
  }
  return g_window->isVisible();
}

//////////////////////////////////////////////////////////////////////
// PaletteEntryEditor implementation
//
// Based on ColorPopup class.

PaletteEntryEditor::PaletteEntryEditor()
  : Window(WithTitleBar, "Palette Editor (F4)")
  , m_type(app::Color::MaskType)
  , m_vbox(VERTICAL)
  , m_topBox(HORIZONTAL)
  , m_bottomBox(HORIZONTAL)
  , m_colorType(3)
  , m_changeMode(2)
  , m_entryLabel("")
  , m_disableHexUpdate(false)
  , m_redrawTimer(250, this)
  , m_redrawAll(false)
  , m_implantChange(false)
  , m_selfPalChange(false)
  , m_fromPalette(0, 0)
{
  m_colorType.addItem("RGB")->setFocusStop(false);
  m_colorType.addItem("HSV")->setFocusStop(false);
  m_colorType.addItem("HSL")->setFocusStop(false);
  m_changeMode.addItem("Abs")->setFocusStop(false);
  m_changeMode.addItem("Rel")->setFocusStop(false);

  m_topBox.setBorder(gfx::Border(0));
  m_topBox.setChildSpacing(0);
  m_bottomBox.setBorder(gfx::Border(0));

  // Top box
  m_topBox.addChild(&m_colorType);
  m_topBox.addChild(new Separator("", VERTICAL));
  m_topBox.addChild(&m_changeMode);
  m_topBox.addChild(new Separator("", VERTICAL));
  m_topBox.addChild(&m_hexColorEntry);
  m_topBox.addChild(&m_entryLabel);
  m_topBox.addChild(new BoxFiller);

  // Main vertical box
  m_vbox.addChild(&m_topBox);
  m_vbox.addChild(&m_sliders);
  m_vbox.addChild(&m_bottomBox);
  addChild(&m_vbox);

  m_colorType.ItemChange.connect(base::Bind<void>(&PaletteEntryEditor::onColorTypeClick, this));
  m_changeMode.ItemChange.connect(base::Bind<void>(&PaletteEntryEditor::onChangeModeClick, this));

  m_sliders.ColorChange.connect(&PaletteEntryEditor::onColorSlidersChange, this);
  m_hexColorEntry.ColorChange.connect(&PaletteEntryEditor::onColorHexEntryChange, this);

  m_changeMode.setSelectedItem(ABS_MODE);
  selectColorType(app::Color::RgbType);

  // We hook fg/bg color changes (by eyedropper mainly) to update the selected entry color
  Preferences::instance().colorBar.fgColor.AfterChange.connect(
    &PaletteEntryEditor::onFgBgColorChange, this);
  Preferences::instance().colorBar.bgColor.AfterChange.connect(
    &PaletteEntryEditor::onFgBgColorChange, this);

  // We hook the Window::Close event to save the frame position before closing it.
  this->Close.connect(base::Bind<void>(&PaletteEntryEditor::onCloseWindow, this));

  // We hook App::Exit signal to destroy the g_window singleton at exit.
  App::instance()->Exit.connect(&PaletteEntryEditor::onExit, this);

  // Hook for palette change to redraw the palette editor frame
  m_palChangeConn =
    App::instance()->PaletteChange.connect(&PaletteEntryEditor::onPalChange, this);

  initTheme();
}

void PaletteEntryEditor::setColor(const app::Color& color)
{
  m_sliders.setColor(color);
  if (!m_disableHexUpdate)
    m_hexColorEntry.setColor(color);

  PalettePicks entries;
  getPicks(entries);
  int i, j, i2;

  // Find the first selected entry
  for (i=0; i<(int)entries.size(); ++i)
    if (entries[i])
      break;

  // Find the first unselected entry after i
  for (i2=i+1; i2<(int)entries.size(); ++i2)
    if (!entries[i2])
      break;

  // Find the last selected entry
  for (j=entries.size()-1; j>=0; --j)
    if (entries[j])
      break;

  if (i == j) {
    m_entryLabel.setTextf(" Entry: %d", i);
  }
  else if (j-i+1 == i2-i) {
    m_entryLabel.setTextf(" Range: %d-%d", i, j);
  }
  else if (i == int(entries.size())) {
    m_entryLabel.setText(" No Entry");
  }
  else {
    m_entryLabel.setText(" Multiple Entries");
  }

  m_topBox.layout();
}

bool PaletteEntryEditor::onProcessMessage(Message* msg)
{
  if (msg->type() == kTimerMessage &&
      static_cast<TimerMessage*>(msg)->timer() == &m_redrawTimer) {
    // Redraw all editors
    if (m_redrawAll) {
      m_redrawAll = false;
      m_implantChange = false;
      m_redrawTimer.stop();

      // Call all observers of PaletteChange event.
      m_selfPalChange = true;
      App::instance()->PaletteChange();
      m_selfPalChange = false;

      // Redraw all editors
      try {
        ContextWriter writer(UIContext::instance());
        Document* document(writer.document());
        if (document != NULL)
          document->notifyGeneralUpdate();
      }
      catch (...) {
        // Do nothing
      }
    }
    // Redraw just the current editor
    else {
      m_redrawAll = true;
      if (current_editor != NULL)
        current_editor->updateEditor();
    }
  }
  return Window::onProcessMessage(msg);
}

void PaletteEntryEditor::onExit()
{
  delete this;
}

void PaletteEntryEditor::onCloseWindow()
{
  // Save window configuration
  save_window_pos(this, "PaletteEditor");

  // Uncheck the "Edit Palette" button.
  ColorBar::instance()->setPaletteEditorButtonState(false);
}

void PaletteEntryEditor::onFgBgColorChange(const app::Color& _color)
{
  app::Color color = _color;

  if (!color.isValid())
    return;

  if (color.getType() != app::Color::IndexType) {
    PaletteView* paletteView = ColorBar::instance()->getPaletteView();
    int index = paletteView->getSelectedEntry();
    if (index < 0)
      return;

    color = app::Color::fromIndex(index);
  }

  if (color.getType() == app::Color::IndexType) {
    setColor(color);
    resetRelativeInfo();
  }
}

void PaletteEntryEditor::onColorSlidersChange(ColorSlidersChangeEvent& ev)
{
  setColor(ev.color());

  if (ev.mode() == ColorSliders::Mode::Absolute)
    setAbsolutePaletteEntryChannel(ev.channel(), ev.color());
  else
    setRelativePaletteEntryChannel(ev.channel(), ev.delta());

  updateCurrentSpritePalette("Color Change");
  updateColorBar();
}

void PaletteEntryEditor::onColorHexEntryChange(const app::Color& color)
{
  // Disable updating the hex entry so we don't override what the user
  // is writting in the text field.
  m_disableHexUpdate = true;

  setColor(color);
  setPaletteEntry(color);
  updateCurrentSpritePalette("Color Change");
  updateColorBar();

  m_disableHexUpdate = false;
}

void PaletteEntryEditor::onColorTypeClick()
{
  switch (m_colorType.selectedItem()) {
    case RGB_MODE:
      selectColorType(app::Color::RgbType);
      break;
    case HSV_MODE:
      selectColorType(app::Color::HsvType);
      break;
    case HSL_MODE:
      selectColorType(app::Color::HslType);
      break;
  }
}

void PaletteEntryEditor::onChangeModeClick()
{
  switch (m_changeMode.selectedItem()) {
    case ABS_MODE:
      m_sliders.setMode(ColorSliders::Mode::Absolute);
      break;
    case REL_MODE:
      m_sliders.setMode(ColorSliders::Mode::Relative);
      break;
  }

  // Update sliders, entries, etc.
  updateWidgetsFromSelectedEntries();
}

void PaletteEntryEditor::setPaletteEntry(const app::Color& color)
{
  PalettePicks entries;
  getPicks(entries);

  color_t new_pal_color = doc::rgba(color.getRed(),
                                    color.getGreen(),
                                    color.getBlue(), 255);

  Palette* palette = get_current_palette();
  for (int c=0; c<palette->size(); c++) {
    if (entries[c])
      palette->setEntry(c, new_pal_color);
  }
}

void PaletteEntryEditor::setAbsolutePaletteEntryChannel(ColorSliders::Channel channel, const app::Color& color)
{
  PalettePicks entries;
  getPicks(entries);
  int picksCount = entries.picks();

  uint32_t src_color;
  int r, g, b, a;

  Palette* palette = get_current_palette();
  for (int c=0; c<palette->size(); c++) {
    if (!entries[c])
      continue;

    // Get the current RGB values of the palette entry
    src_color = palette->getEntry(c);
    r = rgba_getr(src_color);
    g = rgba_getg(src_color);
    b = rgba_getb(src_color);
    a = rgba_geta(src_color);

    switch (m_type) {

      case app::Color::RgbType:
        // Modify one entry
        if (picksCount == 1) {
          r = color.getRed();
          g = color.getGreen();
          b = color.getBlue();
          a = color.getAlpha();
        }
        // Modify one channel a set of entries
        else {
          // Setup the new RGB values depending of the modified channel.
          switch (channel) {
            case ColorSliders::Channel::Red:
              r = color.getRed();
            case ColorSliders::Channel::Green:
              g = color.getGreen();
              break;
            case ColorSliders::Channel::Blue:
              b = color.getBlue();
              break;
            case ColorSliders::Channel::Alpha:
              a = color.getAlpha();
              break;
          }
        }
        break;

      case app::Color::HsvType: {
        Hsv hsv;

        // Modify one entry
        if (picksCount == 1) {
          hsv.hue(color.getHsvHue());
          hsv.saturation(color.getHsvSaturation());
          hsv.value(color.getHsvValue());
          a = color.getAlpha();
        }
        // Modify one channel a set of entries
        else {
          // Convert RGB to HSV
          hsv = Hsv(Rgb(r, g, b));

          // Only modify the desired HSV channel
          switch (channel) {
            case ColorSliders::Channel::HsvHue:
              hsv.hue(color.getHsvHue());
              break;
            case ColorSliders::Channel::HsvSaturation:
              hsv.saturation(color.getHsvSaturation());
              break;
            case ColorSliders::Channel::HsvValue:
              hsv.value(color.getHsvValue());
              break;
            case ColorSliders::Channel::Alpha:
              a = color.getAlpha();
              break;
          }
        }

        // Convert HSV back to RGB
        Rgb rgb(hsv);
        r = rgb.red();
        g = rgb.green();
        b = rgb.blue();
        break;
      }

      case app::Color::HslType: {
        Hsl hsl;

        // Modify one entry
        if (picksCount == 1) {
          hsl.hue(color.getHslHue());
          hsl.saturation(color.getHslSaturation());
          hsl.lightness(color.getHslLightness());
          a = color.getAlpha();
        }
        // Modify one channel a set of entries
        else {
          // Convert RGB to HSL
          hsl = Hsl(Rgb(r, g, b));

          // Only modify the desired HSL channel
          switch (channel) {
            case ColorSliders::Channel::HslHue:
              hsl.hue(color.getHslHue());
              break;
            case ColorSliders::Channel::HslSaturation:
              hsl.saturation(color.getHslSaturation());
              break;
            case ColorSliders::Channel::HslLightness:
              hsl.lightness(color.getHslLightness());
              break;
            case ColorSliders::Channel::Alpha:
              a = color.getAlpha();
              break;
          }
        }

        // Convert HSL back to RGB
        Rgb rgb(hsl);
        r = rgb.red();
        g = rgb.green();
        b = rgb.blue();
        break;
      }

    }

    palette->setEntry(c, doc::rgba(r, g, b, a));
  }
}

void PaletteEntryEditor::setRelativePaletteEntryChannel(ColorSliders::Channel channel, int delta)
{
  PalettePicks entries;
  getPicks(entries);

  // Update modified delta
  m_relDeltas[channel] = delta;

  uint32_t src_color;
  int r, g, b, a;

  Palette* palette = get_current_palette();
  for (int c=0; c<palette->size(); c++) {
    if (!entries[c])
      continue;

    // Get the current RGB values of the palette entry
    src_color = m_fromPalette.getEntry(c);
    r = rgba_getr(src_color);
    g = rgba_getg(src_color);
    b = rgba_getb(src_color);
    a = rgba_geta(src_color);

    switch (m_type) {

      case app::Color::RgbType:
        r = MID(0, r+m_relDeltas[ColorSliders::Channel::Red],   255);
        g = MID(0, g+m_relDeltas[ColorSliders::Channel::Green], 255);
        b = MID(0, b+m_relDeltas[ColorSliders::Channel::Blue],  255);
        a = MID(0, a+m_relDeltas[ColorSliders::Channel::Alpha], 255);
        break;

      case app::Color::HsvType: {
        // Convert RGB to HSV
        Hsv hsv(Rgb(r, g, b));

        double h = hsv.hue()       +m_relDeltas[ColorSliders::Channel::HsvHue];
        double s = hsv.saturation()+m_relDeltas[ColorSliders::Channel::HsvSaturation]/100.0;
        double v = hsv.value()     +m_relDeltas[ColorSliders::Channel::HsvValue]     /100.0;

        if (h < 0.0) h += 360.0;
        else if (h > 360.0) h -= 360.0;

        hsv.hue       (MID(0.0, h, 360.0));
        hsv.saturation(MID(0.0, s, 1.0));
        hsv.value     (MID(0.0, v, 1.0));

        // Convert HSV back to RGB
        Rgb rgb(hsv);
        r = rgb.red();
        g = rgb.green();
        b = rgb.blue();
        a = MID(0, a+m_relDeltas[ColorSliders::Channel::Alpha], 255);
        break;
      }

      case app::Color::HslType: {
        // Convert RGB to HSL
        Hsl hsl(Rgb(r, g, b));

        double h = hsl.hue()       +m_relDeltas[ColorSliders::Channel::HslHue];
        double s = hsl.saturation()+m_relDeltas[ColorSliders::Channel::HslSaturation]/100.0;
        double l = hsl.lightness() +m_relDeltas[ColorSliders::Channel::HslLightness] /100.0;

        if (h < 0.0) h += 360.0;
        else if (h > 360.0) h -= 360.0;

        hsl.hue       (h);
        hsl.saturation(MID(0.0, s, 1.0));
        hsl.lightness (MID(0.0, l, 1.0));

        // Convert HSL back to RGB
        Rgb rgb(hsl);
        r = rgb.red();
        g = rgb.green();
        b = rgb.blue();
        a = MID(0, a+m_relDeltas[ColorSliders::Alpha], 255);
        break;
      }

    }

    palette->setEntry(c, doc::rgba(r, g, b, a));
  }
}

void PaletteEntryEditor::selectColorType(app::Color::Type type)
{
  m_type = type;
  m_sliders.setColorType(type);

  resetRelativeInfo();

  switch (type) {
    case app::Color::RgbType: m_colorType.setSelectedItem(RGB_MODE); break;
    case app::Color::HsvType: m_colorType.setSelectedItem(HSV_MODE); break;
    case app::Color::HslType: m_colorType.setSelectedItem(HSL_MODE); break;
  }

  m_vbox.layout();
  m_vbox.invalidate();
}

void PaletteEntryEditor::updateCurrentSpritePalette(const char* operationName)
{
  if (UIContext::instance()->activeDocument() &&
      UIContext::instance()->activeDocument()->sprite()) {
    try {
      ContextWriter writer(UIContext::instance());
      Document* document(writer.document());
      Sprite* sprite(writer.sprite());
      Palette* newPalette = get_current_palette(); // System current pal
      frame_t frame = writer.frame();
      Palette* currentSpritePalette = sprite->palette(frame); // Sprite current pal
      int from, to;

      // Check differences between current sprite palette and current system palette
      from = to = -1;
      currentSpritePalette->countDiff(newPalette, &from, &to);

      if (from >= 0 && to >= from) {
        DocumentUndo* undo = document->undoHistory();
        Cmd* cmd = new cmd::SetPalette(sprite, frame, newPalette);

        // Add undo information to save the range of pal entries that will be modified.
        if (m_implantChange &&
            undo->lastExecutedCmd() &&
            undo->lastExecutedCmd()->label() == operationName) {
          // Implant the cmd in the last CmdSequence if it's
          // related about color palette modifications
          ASSERT(dynamic_cast<CmdSequence*>(undo->lastExecutedCmd()));
          static_cast<CmdSequence*>(undo->lastExecutedCmd())->add(cmd);
          cmd->execute(UIContext::instance());
        }
        else {
          Transaction transaction(writer.context(), operationName, ModifyDocument);
          transaction.execute(cmd);
          transaction.commit();
        }
      }
    }
    catch (base::Exception& e) {
      Console::showException(e);
    }
  }

  PaletteView* palette_editor = ColorBar::instance()->getPaletteView();
  palette_editor->invalidate();

  if (!m_redrawTimer.isRunning())
    m_redrawTimer.start();

  m_redrawAll = false;
  m_implantChange = true;
}

void PaletteEntryEditor::updateColorBar()
{
  ColorBar::instance()->invalidate();
}

void PaletteEntryEditor::updateWidgetsFromSelectedEntries()
{
  PaletteView* palette_editor = ColorBar::instance()->getPaletteView();
  int index = palette_editor->getSelectedEntry();
  if (index >= 0)
    setColor(app::Color::fromIndex(index));

  resetRelativeInfo();

  // Redraw the window
  invalidate();
}

void PaletteEntryEditor::onPalChange()
{
  if (!m_selfPalChange)
    updateWidgetsFromSelectedEntries();
}

void PaletteEntryEditor::resetRelativeInfo()
{
  m_sliders.resetRelativeSliders();
  get_current_palette()->copyColorsTo(&m_fromPalette);
  m_relDeltas.clear();
}

void PaletteEntryEditor::getPicks(PalettePicks& picks)
{
  PaletteView* palView = ColorBar::instance()->getPaletteView();
  palView->getSelectedEntries(picks);
  if (picks.picks() == 0) {
    int i = palView->getSelectedEntry();
    if (i >= 0 && i < picks.size())
      picks[i] = true;
  }
}

Command* CommandFactory::createPaletteEditorCommand()
{
  return new PaletteEditorCommand;
}

} // namespace app
