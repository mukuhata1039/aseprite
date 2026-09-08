// Aseprite
// Copyright (C) 2020-2023  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/ui/editor/play_state.h"

#include "app/commands/command.h"
#include "app/commands/commands.h"
#include "app/loop_tag.h"
#include "app/pref/preferences.h"
#include "app/tools/ink.h"
#include "app/ui/editor/editor.h"
#include "app/ui/editor/editor_customization_delegate.h"
#include "app/ui/editor/scrolling_state.h"
#include "app/ui/skin/skin_theme.h"
#include "app/ui_context.h"
#include "doc/tag.h"
#include "ui/manager.h"
#include "ui/message.h"
#include "ui/system.h"

namespace app {

using namespace ui;

PlayState::PlayState(const bool playOnce, const bool playAll, const bool playSubtags)
  : m_editor(nullptr)
  , m_playOnce(playOnce)
  , m_playAll(playAll)
  , m_playSubtags(playSubtags)
  , m_toScroll(false)
  , m_playTimer(10)
  , m_nextFrameTime(-1)
  , m_refFrame(0)
  , m_tag(nullptr)
  , m_customPlaybackTag(0, 0)
{
  m_playTimer.Tick.connect(&PlayState::onPlaybackTick, this);

  // Hook BeforeCommandExecution signal so we know if the user wants
  // to execute other command, so we can stop the animation.
  m_ctxConn = UIContext::instance()->BeforeCommandExecution.connect(
    &PlayState::onBeforeCommandExecution,
    this);
}

Tag* PlayState::playingTag() const
{
  return m_tag;
}

void PlayState::onEnterState(Editor* editor)
{
  StateWithWheelBehavior::onEnterState(editor);

  if (!m_editor) {
    m_editor = editor;
    m_refFrame = editor->frame();
  }

  // Get the normal Aseprite tag first.
  if (!m_playAll) {
    m_tag = m_editor->getCustomizationDelegate()
              ->getTagProvider()
              ->getTagByFrame(m_refFrame, true);

    // Don't repeat the tag infinitely if the tag repeat field doesn't
    // say so.
    if (m_playSubtags && m_tag && m_tag->repeat() != 0) {
      m_tag = nullptr;
    }
  }
  else {
    m_tag = nullptr;
  }

  // ------------------------------------------------------------
  // CUSTOM PLAYBACK
  //
  // If this Editor has a remembered playback range, use that range.
  // Otherwise, use the whole sprite range so LOOP / PING-PONG also
  // works when the user has never selected a custom frame range.
  //
  // The temporary tag is not added to the Sprite/document.
  // ------------------------------------------------------------
  if (m_editor->hasCustomPlaybackRange()) {
    m_customPlaybackTag.setFrameRange(
      m_editor->customPlaybackFrom(),
      m_editor->customPlaybackTo());
  }
  else {
    m_customPlaybackTag.setFrameRange(
      frame_t(0),
      m_editor->sprite()->lastFrame());
  }

  if (m_editor->customPlaybackMode() == Editor::CustomPlaybackMode::PingPong) {
    m_customPlaybackTag.setAniDir(AniDir::PING_PONG);
  }
  else {
    m_customPlaybackTag.setAniDir(AniDir::FORWARD);
  }

  // Use our temporary tag instead of a real document tag.
  m_tag = &m_customPlaybackTag;

  // Force Playback::PlayInLoop so our temporary tag is used
  // continuously.
  m_playOnce = false;
  m_playAll = false;
  m_playSubtags = false;

  // Go to the first frame of the animation or active frame tag.
  if (m_playOnce) {
    frame_t frame = 0;

    if (m_tag) {
      frame =
        (m_tag->aniDir() == AniDir::REVERSE ?
           m_tag->toFrame() :
           m_tag->fromFrame());
    }

    m_editor->setFrame(frame);
  }

  m_toScroll = false;

  // Maybe we came from ScrollingState and the timer is already
  // running. Which also means there was a Playback in course, so
  // don't create a new one (this fixes an issue when the editor
  // came back from the ScrollingState while playing a tag
  // with ping-pong direction: the direction was reset every time
  // the user released the mouse button after scrolling the editor).
  if (!m_playTimer.isRunning()) {
    // For a remembered custom range, start playback from the
    // beginning of that range. For whole-sprite playback, keep the
    // current frame as the starting point.
    if (m_editor->hasCustomPlaybackRange())
      m_editor->setFrame(m_customPlaybackTag.fromFrame());

    m_playback = doc::Playback(
      m_editor->sprite(),
      m_playSubtags ?
        m_editor->sprite()->tags().getInternalList() :
        TagsList(),
      m_editor->frame(),
      m_playOnce ?
        doc::Playback::PlayOnce :
      m_playAll ?
        doc::Playback::PlayWithoutTagsInLoop :
        doc::Playback::PlayInLoop,
      m_tag);

    m_nextFrameTime = getNextFrameTime();
    m_curFrameTick = base::current_tick();
    m_playTimer.start();
  }
}

EditorState::LeaveAction PlayState::onLeaveState(Editor* editor, EditorState* newState)
{
  // We don't stop the timer if we are going to the ScrollingState
  // (we keep playing the animation).
  if (!m_toScroll) {
    m_playTimer.stop();

    if (m_playOnce || Preferences::instance().general.rewindOnStop())
      m_editor->setFrame(m_refFrame);
  }

  return KeepState;
}

void PlayState::onBeforePopState(Editor* editor)
{
  m_ctxConn.disconnect();
  StateWithWheelBehavior::onBeforePopState(editor);
}

bool PlayState::onMouseDown(Editor* editor, MouseMessage* msg)
{
  if (editor->hasCapture())
    return true;

  // When an editor is clicked the current view is changed.
  UIContext* context = UIContext::instance();
  context->setActiveView(editor->getDocView());

  // A click with right-button stops the animation.
  if (msg->button() == kButtonRight) {
    editor->stop();
    return true;
  }

  // Set this flag to indicate that we are going to ScrollingState for
  // some time, so we don't change the current frame.
  m_toScroll = true;

  // If the active tool is the Zoom tool, we start zooming.
  if (editor->checkForZoom(msg))
    return true;

  // Start scroll loop.
  editor->startScrollingState(msg);
  return true;
}

bool PlayState::onMouseUp(Editor* editor, MouseMessage* msg)
{
  editor->releaseMouse();
  return true;
}

bool PlayState::onMouseMove(Editor* editor, MouseMessage* msg)
{
  editor->updateStatusBar();
  return true;
}

bool PlayState::onKeyDown(Editor* editor, KeyMessage* msg)
{
  return false;
}

bool PlayState::onKeyUp(Editor* editor, KeyMessage* msg)
{
  return false;
}

bool PlayState::onSetCursor(Editor* editor, const gfx::Point& mouseScreenPos)
{
  tools::Ink* ink = editor->getCurrentEditorInk();

  if (ink) {
    if (ink->isZoom()) {
      auto theme = skin::SkinTheme::get(editor);
      editor->showMouseCursor(
        kCustomCursor,
        theme->cursors.magnifier());

      return true;
    }
  }

  editor->showMouseCursor(kScrollCursor);
  return true;
}

void PlayState::onRemoveTag(Editor* editor, doc::Tag* tag)
{
  if (m_tag == tag)
    m_tag = nullptr;

  m_playback.removeReferencesToTag(tag);
}

void PlayState::onPlaybackTick()
{
  ASSERT(m_playTimer.isRunning());

  if (m_nextFrameTime < 0)
    return;

  m_nextFrameTime -=
    (base::current_tick() - m_curFrameTick);

  while (m_nextFrameTime <= 0) {
    doc::frame_t frame = m_playback.nextFrame();

    if (m_playback.isStopped() ||
        // TODO invalid frame from Playback::nextFrame(), in this way
        //      we avoid any kind of crash or assert fail
        frame < 0 ||
        frame > m_editor->sprite()->lastFrame()) {

      TRACEARGS(
        "!!! PlayState: invalid frame from Playback::nextFrame() frame=",
        frame);

      m_editor->stop();
      break;
    }

    m_editor->setFrame(frame);
    m_nextFrameTime += getNextFrameTime();
  }

  m_curFrameTick = base::current_tick();
}

// Before executing any command, we stop the animation.
void PlayState::onBeforeCommandExecution(CommandExecutionEvent& ev)
{
  // This check just in case we stay connected to context signals when
  // the editor is already deleted.
  ASSERT(m_editor);
  ASSERT(m_editor->manager() == ui::Manager::getDefault());

  // If the command is for other editor, we don't stop the animation.
  // This keeps playback in other Editors independent.
  if (!m_editor->isActive())
    return;

  // Playback control commands must not be preemptively stopped here.
  // Their own command handlers decide which editor(s) should stop/play.
  //
  // There are other commands that just don't stop the animation
  // (zoom, scroll, etc.).
  if (ev.command()->id() == CommandId::PlayAnimation() ||
      ev.command()->id() == CommandId::Zoom() ||
      ev.command()->id() == CommandId::Scroll() ||
      ev.command()->id() == CommandId::Timeline()) {
    return;
  }

  m_editor->stop();
}

double PlayState::getNextFrameTime()
{
  return
    m_editor->sprite()->frameDuration(m_editor->frame()) /
    m_editor->getAnimationSpeedMultiplier();
}

} // namespace app
