// Aseprite
// Copyright (C) 2026  Contributors
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/commands/command.h"
#include "app/context.h"
#include "app/ui/editor/editor.h"

namespace app {

class ToggleMirrorViewCommand : public Command {
public:
  ToggleMirrorViewCommand() : Command(CommandId::ToggleMirrorView()) {}

protected:
  bool onEnabled(Context* context) override
  {
    auto* editor = Editor::activeEditor();
    return context->isUIAvailable() && editor && editor->sprite();
  }

  bool onChecked(Context* context) override
  {
    auto* editor = Editor::activeEditor();
    return editor && editor->viewMirrored();
  }

  void onExecute(Context* context) override
  {
    auto* editor = Editor::activeEditor();
    if (editor && editor->sprite())
      editor->setViewMirrored(!editor->viewMirrored());
  }
};

Command* CommandFactory::createToggleMirrorViewCommand()
{
  return new ToggleMirrorViewCommand;
}

} // namespace app
