// Aseprite
// Copyright (C) 2019-2025  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include <stdio.h>

#include "app/app.h"
#include "app/color.h"
#include "app/color_utils.h"
#include "app/cmd/set_user_data.h"
#include "app/commands/command.h"
#include "app/commands/commands.h"
#include "app/commands/filters/filter_manager_impl.h"
#include "app/commands/filters/filter_window.h"
#include "app/commands/new_params.h"
#include "app/context.h"
#include "app/find_widget.h"
#include "app/i18n/strings.h"
#include "app/ini_file.h"
#include "app/load_widget.h"
#include "app/pref/preferences.h"
#include "app/site.h"
#include "app/ui/color_bar.h"
#include "app/ui/color_button.h"
#include "app/ui/color_tint_shade_tone.h"
#include "app/ui/keyboard_shortcuts.h"
#include "app/ui/timeline/timeline.h"
#include "app/ui_context.h"
#include "doc/image.h"
#include "doc/layer.h"
#include "doc/palette.h"
#include "doc/user_data.h"
#include "doc/mask.h"
#include "doc/sprite.h"
#include "filters/replace_color_filter.h"
#include "filters/filter_manager.h"
#include "filters/filter.h"
#include "ui/label.h"
#include "ui/box.h"
#include "ui/ui.h"

namespace app {

struct ReplaceColorParams : public NewParams {
  Param<bool> ui{ this, true, "ui" };
  Param<filters::Target> channels{ this, 0, "channels" };
  Param<app::Color> from{ this, app::Color(), "from" };
  Param<app::Color> to{ this, app::Color(), "to" };
  Param<int> tolerance{ this, 0, "tolerance" };
  Param<bool> layerRecolor{ this, false, "layerRecolor" };
};

// Wrapper for ReplaceColorFilter to handle colors in an easy way
class ReplaceColorFilterWrapper : public ReplaceColorFilter {
public:
  ReplaceColorFilterWrapper(Layer* layer) : m_layer(layer) {}

  void setFrom(const app::Color& from)
  {
    m_from = from;
    if (m_layer)
      ReplaceColorFilter::setFrom(color_utils::color_for_layer(from, m_layer));
  }
  void setTo(const app::Color& to)
  {
    m_to = to;
    if (m_layer)
      ReplaceColorFilter::setTo(color_utils::color_for_layer(to, m_layer));
  }

  app::Color getFrom() const { return m_from; }
  app::Color getTo() const { return m_to; }

private:
  Layer* m_layer;
  app::Color m_from;
  app::Color m_to;
};

static const char* ConfigSection = "ReplaceColor";

class ReplaceColorWindow : public FilterWindow {
public:
  ReplaceColorWindow(ReplaceColorFilterWrapper& filter, FilterManagerImpl& filterMgr)
    : FilterWindow(Strings::replace_color_title().c_str(),
                   ConfigSection,
                   &filterMgr,
                   WithChannelsSelector,
                   WithoutTiledCheckBox)
    , m_filter(filter)
    , m_controlsWidget(app::load_widget<Widget>("replace_color.xml", "controls"))
    , m_fromButton(app::find_widget<ColorButton>(m_controlsWidget.get(), "from"))
    , m_toButton(app::find_widget<ColorButton>(m_controlsWidget.get(), "to"))
    , m_toleranceSlider(app::find_widget<ui::Slider>(m_controlsWidget.get(), "tolerance"))
  {
    getContainer()->addChild(m_controlsWidget.get());

    m_fromButton->setColor(m_filter.getFrom());
    m_toButton->setColor(m_filter.getTo());
    m_toleranceSlider->setValue(m_filter.getTolerance());

    m_fromButton->Change.connect(&ReplaceColorWindow::onFromChange, this);
    m_toButton->Change.connect(&ReplaceColorWindow::onToChange, this);
    m_toleranceSlider->Change.connect(&ReplaceColorWindow::onToleranceChange, this);
  }

private:
  void onFromChange(const app::Color& color)
  {
    stopPreview();
    m_filter.setFrom(color);
    restartPreview();
  }

  void onToChange(const app::Color& color)
  {
    stopPreview();
    m_filter.setTo(color);
    restartPreview();
  }

  void onToleranceChange()
  {
    stopPreview();
    m_filter.setTolerance(m_toleranceSlider->getValue());
    restartPreview();
  }

  bool onProcessMessage(ui::Message* msg) override
  {
    switch (msg->type()) {
      case ui::kKeyDownMessage: {
        KeyboardShortcuts* keys = KeyboardShortcuts::instance();
        const KeyPtr key = keys->command(CommandId::SwitchColors());
        if (key && key->isPressed(msg)) {
          // Switch colors
          app::Color from = m_fromButton->getColor();
          app::Color to = m_toButton->getColor();
          m_fromButton->setColor(to);
          m_toButton->setColor(from);
        }
        break;
      }
    }
    return FilterWindow::onProcessMessage(msg);
  }

  ReplaceColorFilterWrapper& m_filter;
  std::unique_ptr<ui::Widget> m_controlsWidget;
  ColorButton* m_fromButton;
  ColorButton* m_toButton;
  ui::Slider* m_toleranceSlider;
};


//////////////////////////////////////////////////////////////////////
// Quick layer recoloring via the timeline color-lock indicator.
//
// Unlike Replace Color (which changes a matching source color), this
// changes the RGB/gray/index of every nontransparent pixel in the
// active raster layer, preserving each pixel's existing alpha.
class LayerRecolorFilter : public filters::Filter {
public:
  LayerRecolorFilter(Layer* layer, const app::Color& color)
    : m_layer(layer)
  {
    setColor(color);
  }

  void setColor(const app::Color& color)
  {
    if (color.getType() == app::Color::MaskType ||
        (color.getType() != app::Color::IndexType && color.getAlpha() == 0))
      return;

    const doc::color_t pixel = color_utils::color_for_layer(color, m_layer);
    if (m_layer->sprite()->pixelFormat() == IMAGE_INDEXED &&
        pixel == m_layer->sprite()->transparentColor())
      return;

    m_pixel = pixel;
    m_color = color;
  }

  app::Color color() const { return m_color; }

  app::Color lockColor() const
  {
    switch (m_layer->sprite()->pixelFormat()) {
      case IMAGE_RGB:
        return app::Color::fromRgb(rgba_getr(m_pixel), rgba_getg(m_pixel), rgba_getb(m_pixel));
      case IMAGE_GRAYSCALE:
        return app::Color::fromGray(graya_getv(m_pixel));
      case IMAGE_INDEXED:
        return app::Color::fromIndex(int(m_pixel));
      default:
        return m_color;
    }
  }

  const char* getName() override { return "Recolor Layer"; }

  void applyToRgba(filters::FilterManager* filterMgr) override
  {
    FILTER_LOOP_THROUGH_ROW_BEGIN(uint32_t)
      if (rgba_geta(*src_address) != 0) {
        *dst_address = rgba(rgba_getr(m_pixel),
                            rgba_getg(m_pixel),
                            rgba_getb(m_pixel),
                            rgba_geta(*src_address));
      }
    FILTER_LOOP_THROUGH_ROW_END()
  }

  void applyToGrayscale(filters::FilterManager* filterMgr) override
  {
    FILTER_LOOP_THROUGH_ROW_BEGIN(uint16_t)
      if (graya_geta(*src_address) != 0)
        *dst_address = graya(graya_getv(m_pixel), graya_geta(*src_address));
    FILTER_LOOP_THROUGH_ROW_END()
  }

  void applyToIndexed(filters::FilterManager* filterMgr) override
  {
    const auto transparent = m_layer->sprite()->transparentColor();
    const bool background = m_layer->isBackground();
    FILTER_LOOP_THROUGH_ROW_BEGIN(uint8_t)
      if (background || *src_address != transparent)
        *dst_address = uint8_t(m_pixel);
    FILTER_LOOP_THROUGH_ROW_END()
  }

private:
  Layer* m_layer;
  app::Color m_color = app::Color::fromRgb(255, 255, 255);
  doc::color_t m_pixel = 0;
};

class LayerRecolorWindow : public FilterWindow {
public:
  LayerRecolorWindow(LayerRecolorFilter& filter, FilterManagerImpl& mgr)
    : FilterWindow(Strings::replace_color_layer_title().c_str(),
                   "RecolorLayer",
                   &mgr,
                   WithoutChannelsSelector,
                   WithoutTiledCheckBox)
    , m_filter(filter)
    , m_label(Strings::replace_color_layer_new_color())
    , m_colorButton(filter.color(), mgr.pixelFormat(), ColorButtonOptions())
  {
    // The layer recolor window is modal. Embed the same HSV
    // saturation/value square + hue strip used in the sidebar, so
    // color picking does not need to escape the modal dialog.
    mgr.setCelsTarget(CelsTarget::ActiveLayer);
    hideTargetButtons();
    getContainer()->addChild(&m_colorRow);
    m_colorRow.addChild(&m_label);
    m_colorRow.addChild(&m_colorButton);
    getContainer()->addChild(&m_colorSelector);
    m_colorSelector.setSizeHint(196 * ui::guiscale(), 156 * ui::guiscale());
    m_colorSelector.setExpansive(true);
    m_colorSelector.selectColor(m_filter.lockColor());

    m_colorSelector.ColorChange.connect(
      [this](const app::Color& newColor, ui::MouseButton button) {
        if (button == ui::kButtonRight)
          return; // A secondary pick must not accidentally change the target.
        changePreviewColor(newColor, true);
      });

    m_colorButton.Change.connect([this](const app::Color& newColor) {
      if (!m_syncingColor)
        changePreviewColor(newColor, false);
    });
  }

private:
  void changePreviewColor(const app::Color& newColor, bool fromSelector)
  {
    if (newColor.getType() == app::Color::MaskType)
      return;

    stopPreview();
    m_filter.setColor(newColor);
    const app::Color chosen = m_filter.lockColor();

    // The embedded HSV picker and the numeric/color swatch control
    // always show the same target. Only one preview restart per edit.
    if (fromSelector) {
      m_syncingColor = true;
      m_colorButton.setColor(chosen);
      m_syncingColor = false;
    }
    else {
      m_colorSelector.selectColor(chosen);
    }
    restartPreview();
  }

  LayerRecolorFilter& m_filter;
  ui::HBox m_colorRow;
  ui::Label m_label;
  ColorButton m_colorButton;
  ColorTintShadeTone m_colorSelector;
  bool m_syncingColor = false;
};

class ReplaceColorCommand : public CommandWithNewParams<ReplaceColorParams> {
public:
  ReplaceColorCommand();

protected:
  bool onEnabled(Context* context) override;
  void onExecute(Context* context) override;
};

ReplaceColorCommand::ReplaceColorCommand()
  : CommandWithNewParams<ReplaceColorParams>(CommandId::ReplaceColor())
{
}

bool ReplaceColorCommand::onEnabled(Context* context)
{
  return context->checkFlags(ContextFlags::ActiveDocumentIsWritable |
                             ContextFlags::HasActiveSprite);
}

void ReplaceColorCommand::onExecute(Context* context)
{
  const bool ui = (params().ui() && context->isUIAvailable());
  Site site = context->activeSite();

  if (params().layerRecolor()) {
    Layer* layer = site.layer();
    if (!ui || !layer || !layer->isImage() || layer->isTilemap() ||
        layer->isReference() || !layer->canEditPixels())
      return;

    ColorBar* colorBar = ColorBar::instance();
    app::Color initialColor = app::Color::fromRgb(255, 255, 255);
    if (colorBar) {
      initialColor = (colorBar->isLayerColorLocked(site.document(), layer) ?
                      colorBar->layerColorLock(site.document(), layer) :
                      colorBar->getFgColor());
    }
    if (initialColor.getType() == app::Color::MaskType ||
        (initialColor.getType() != app::Color::IndexType &&
         initialColor.getAlpha() == 0))
      initialColor = app::Color::fromRgb(255, 255, 255);

    // Source pixels are only changed on OK/Apply. The existing filter
    // preview renderer draws changes to the active frame without
    // modifying the document, and Cancel discards uncommitted edits.
    LayerRecolorFilter layerFilter(layer, initialColor);
    FilterManagerImpl manager(context, &layerFilter);

    manager.setAfterApplyHook([&layerFilter, layer, doc = manager.document()](Tx& tx) {
      doc::UserData newData = layer->userData();
      newData.properties("mukuhata.layer_color_lock")["color"] =
        layerFilter.lockColor().toString();
      tx(new cmd::SetUserData(layer, newData, doc));
    });
    manager.setAfterCommitHook([] {
      if (ColorBar* bar = ColorBar::instance())
        bar->onActiveSiteChange(UIContext::instance()->activeSite());
      if (App::instance()->timeline())
        App::instance()->timeline()->invalidate();
    });

    LayerRecolorWindow window(layerFilter, manager);
    window.doModal();
    return;
  }

  ReplaceColorFilterWrapper filter(site.layer());
  FilterManagerImpl filterMgr(context, &filter);
  filterMgr.setTarget(site.sprite()->pixelFormat() == IMAGE_INDEXED ?
                        TARGET_INDEX_CHANNEL :
                        TARGET_RED_CHANNEL | TARGET_GREEN_CHANNEL | TARGET_BLUE_CHANNEL |
                          TARGET_GRAY_CHANNEL | TARGET_ALPHA_CHANNEL);

  filter.setFrom(Preferences::instance().colorBar.fgColor());
  filter.setTo(Preferences::instance().colorBar.bgColor());
  if (ui)
    filter.setTolerance(get_config_int(ConfigSection, "Tolerance", 0));

  if (params().from.isSet())
    filter.setFrom(params().from());
  if (params().to.isSet())
    filter.setTo(params().to());
  if (params().tolerance.isSet())
    filter.setTolerance(params().tolerance());
  if (params().channels.isSet())
    filterMgr.setTarget(params().channels());

  if (ui) {
    ReplaceColorWindow window(filter, filterMgr);
    if (window.doModal())
      set_config_int(ConfigSection, "Tolerance", filter.getTolerance());
  }
  else {
    filterMgr.startWorker(false);
  }
}

Command* CommandFactory::createReplaceColorCommand()
{
  return new ReplaceColorCommand;
}

} // namespace app
