// Aseprite
// Copyright (C) 2026-present  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#include "app/ui/toolset_tree.h"

#include "app/i18n/strings.h"
#include "skin/skin_theme.h"
#include "text/font_metrics.h"
#include "ui/message.h"
#include "ui/paint_event.h"
#include "ui/scale.h"
#include "ui/size_hint_event.h"
#include "ui/utf8_range_builder.h"
#include "ui/view.h"

using namespace text;

namespace app {

using namespace ui;
using namespace app::skin;

void ToolsetTreeNode::removeChild(TreeNode* child)
{
  if (!child || child->parent() != this)
    return;

  auto* prev = child->prev();
  auto* next = child->next();

  if (prev)
    prev->setNext(next);
  else
    m_firstChild = next;

  if (next)
    next->setPrev(prev);
  else
    m_lastChild = prev;

  child->attachTo(nullptr, nullptr, nullptr);
}

void ToolsetTreeNode::insertChildBefore(TreeNode* child, TreeNode* before)
{
  if (!child || !before || before->parent() != this)
    return;

  auto* prev = before->prev();
  child->attachTo(this, prev, before);
  before->setPrev(child);

  if (prev)
    prev->setNext(child);
  else
    m_firstChild = child;
}

ToolsetTree::ToolsetTree() : m_dragTimer(300, this)
{
  initTheme();
}

int ToolsetTree::nodeHeight(const TreeNode* node) const
{
  return node->textBlob() ? node->textBlob()->bounds().h + (m_themeCache.itemSpacing * 2) :
                            m_themeCache.rowHeight;
}

ToolsetTreeNode* ToolsetTree::nodeAtY(int y, int* outNodeY) const
{
  int nodeY = border().top();
  for (auto* node = root(); node; node = node->nextInTree()) {
    int nextNodeY = nodeY + nodeHeight(node);
    if (y >= nodeY && y < nextNodeY) {
      if (outNodeY)
        *outNodeY = nodeY;
      return static_cast<ToolsetTreeNode*>(node);
    }
    nodeY = nextNodeY;
  }
  return nullptr;
}

void ToolsetTree::toggleCollapse(TreeNode* node, bool recursive)
{
  if (node == root())
    return;
  Tree::toggleCollapse(node, recursive);
}

bool ToolsetTree::onProcessMessage(Message* msg)
{
  switch (msg->type()) {
    case kMouseDownMessage: {
      bool handled = Tree::onProcessMessage(msg);
      if (handled && !m_dragNode && selected()) {
        m_dragCandidate = static_cast<ToolsetTreeNode*>(selected());
        m_dragMousePos = static_cast<MouseMessage*>(msg)->position();
        m_dragTimer.start();
      }
      return handled;
    }
    case kMouseMoveMessage: {
      if (m_dragNode) {
        auto* mouseMsg = static_cast<MouseMessage*>(msg);
        m_dragMousePos = mouseMsg->position();

        const gfx::PointF offsetPos(mouseMsg->position() - childrenBounds().origin() -
                                    gfx::Point(border().left(), 0));
        int targetNodeY = 0;
        auto* target = nodeAtY(offsetPos.y, &targetNodeY);
        bool dragIsGroup = (m_dragNode->parent() == root());
        bool dragIsTool = !dragIsGroup;
        bool targetIsGroup = target && (target->parent() == root());

        if (target && target != m_dragNode && target != root() && (dragIsTool || targetIsGroup)) {
          m_dropTarget = target;
          int h = nodeHeight(target);
          int relY = offsetPos.y - targetNodeY;
          if (targetIsGroup && dragIsTool) {
            if (relY < h / 4)
              m_dropPos = DropPos::Before;
            else if (relY > h * 3 / 4)
              m_dropPos = DropPos::After;
            else
              m_dropPos = DropPos::Inside;
          }
          else {
            m_dropPos = (relY < h / 2) ? DropPos::Before : DropPos::After;
          }

          bool samePosition = (m_dropPos == DropPos::After && m_dropTarget->next() == m_dragNode) ||
                              (m_dropPos == DropPos::Before && m_dropTarget->prev() == m_dragNode);

          if (samePosition || (dragIsGroup && m_dropTarget && m_dropPos == DropPos::After &&
                               m_dropTarget->hasChildren() && !m_dropTarget->isCollapsed()))
            m_dropTarget = nullptr;
        }
        else {
          m_dropTarget = nullptr;
        }

        invalidate();
        return true;
      }

      if (m_dragTimer.isRunning())
        return true;

      return Tree::onProcessMessage(msg);
    }
    case kMouseUpMessage: {
      m_dragTimer.stop();

      if (m_dragNode && m_dropTarget && m_dropTarget != m_dragNode) {
        auto* oldParent = static_cast<ToolsetTreeNode*>(m_dragNode->parent());
        bool dragIsGroup = (oldParent == root());
        if (oldParent)
          oldParent->removeChild(m_dragNode);

        if (m_dropPos == DropPos::Inside) {
          if (m_dropTarget->firstChild())
            m_dropTarget->insertChildBefore(m_dragNode, m_dropTarget->firstChild());
          else
            m_dropTarget->addChild(m_dragNode);
          m_dropTarget->setIcon(m_dropTarget->firstChild()->icon());
        }
        else {
          auto* targetParent = static_cast<ToolsetTreeNode*>(m_dropTarget->parent());
          if (targetParent) {
            if (!dragIsGroup && targetParent == root()) {
              const std::string& baseName = Strings::options_toolset_new_group();
              int n = 1;
              for (bool found = true; found; ++n) {
                found = false;
                std::string candidate = baseName + " " + std::to_string(n);
                for (auto* g = root()->firstChild(); g; g = g->next()) {
                  if (g->text() == candidate) {
                    found = true;
                    break;
                  }
                }
              }
              std::string groupName = baseName + " " + std::to_string(n - 1);
              std::string groupId = "custom_group_" + std::to_string(n - 1);
              auto* newGroup = new ToolsetTreeNode(groupName);
              newGroup->setRemovable(true);
              newGroup->setId(groupId);
              newGroup->addChild(m_dragNode);
              newGroup->setIcon(m_dragNode->icon());
              if (m_dropPos == DropPos::Before)
                targetParent->insertChildBefore(newGroup, m_dropTarget);
              else {
                auto* afterNext = m_dropTarget->next();
                if (afterNext)
                  targetParent->insertChildBefore(newGroup, afterNext);
                else
                  targetParent->addChild(newGroup);
              }
            }
            else {
              if (m_dropPos == DropPos::Before)
                targetParent->insertChildBefore(m_dragNode, m_dropTarget);
              else {
                auto* afterNext = m_dropTarget->next();
                if (afterNext)
                  targetParent->insertChildBefore(m_dragNode, afterNext);
                else
                  targetParent->addChild(m_dragNode);
              }
              if (targetParent != root())
                targetParent->setIcon(targetParent->firstChild()->icon());
            }
          }
        }

        if (oldParent && oldParent != root() && !oldParent->firstChild() &&
            oldParent->isRemovable()) {
          static_cast<ToolsetTreeNode*>(oldParent->parent())->removeChild(oldParent);
          delete oldParent;
        }
        else if (oldParent && oldParent != root() && oldParent->firstChild()) {
          oldParent->setIcon(oldParent->firstChild()->icon());
        }

        setSelected(m_dragNode, true);
        invalidate();
      }

      m_dragCandidate = nullptr;
      m_dragNode = nullptr;
      m_dropTarget = nullptr;

      return Tree::onProcessMessage(msg);
    }
    case kTimerMessage: {
      if (static_cast<TimerMessage*>(msg)->timer() == &m_dragTimer) {
        m_dragTimer.stop();
        if (m_dragCandidate && hasCapture()) {
          m_dragNode = m_dragCandidate;

          gfx::PointF pt(clientChildrenBounds().origin() + gfx::PointF(border().size()));
          for (auto* node = root(); node; node = node->nextInTree()) {
            if (node == m_dragCandidate)
              break;
            pt.y += nodeHeight(node);
          }
          int nodeX = m_dragCandidate->depth() * m_themeCache.depthSpacing;
          m_dragOffset = m_dragMousePos - bounds().origin() - gfx::Point(pt.x + nodeX, pt.y);

          invalidate();
        }
        return true;
      }
      return Tree::onProcessMessage(msg);
    }
  }
  return Tree::onProcessMessage(msg);
}

void ToolsetTree::onPaint(PaintEvent& ev)
{
  Graphics* g = ev.graphics();

  const auto& textColors = m_themeCache.textColors;
  const Paint& textPaint = isEnabled() ? textColors.text : textColors.disabledText;
  const Paint& backgroundPaint = isEnabled() ? textColors.background :
                                               textColors.disabledBackground;
  const Paint& selectedPaint = textColors.selectedText;
  const Paint& selectedBackgroundPaint = textColors.selectedBackground;

  const int itemSpacing = m_themeCache.itemSpacing;
  const int depthSpacing = m_themeCache.depthSpacing;

  g->drawRect(g->getClipBounds(), backgroundPaint);

  gfx::PointF point(clientChildrenBounds().origin() + gfx::PointF(border().size()));
  const gfx::Rect clipBounds = g->getClipBounds();
  const auto fontHeight = font()->metrics(nullptr);
  const auto fullWidth = size().w;

  for (auto* node = root(); node; node = node->nextInTree()) {
    const bool draw = clipBounds.intersects(
      gfx::Rect(point.x, point.y, fullWidth, nodeHeight(node)));

    if (draw && !node->textBlob()) {
      Utf8RangeBuilder rangeBuilder(node->text().size());
      node->setTextBlob(
        TextBlob::MakeWithShaper(theme()->fontMgr(), font(), node->text(), &rangeBuilder));
    }

    const auto height = node->textBlob() ? node->textBlob()->bounds().h : fontHeight;

    if (draw) {
      const auto depth = node->depth();
      const int currentDepthSpacing = depth * depthSpacing;

      auto textPoint = point + gfx::Point(currentDepthSpacing, 0);

      // Icon
      if (node->icon()) {
        const gfx::Point iconPoint(textPoint.x - node->icon()->size().w - itemSpacing,
                                   guiscaled_center(textPoint.y, height, node->icon()->size().w));
        if (coloredIcons())
          g->drawRgbaSurface(node->icon()->bitmap(0), iconPoint.x, iconPoint.y);
        else
          g->drawColoredRgbaSurface(node->icon()->bitmap(0),
                                    textColors.text.color(),
                                    iconPoint.x,
                                    iconPoint.y);
      }
      // Fallback for tool groups without icon associated
      else if (node->hasChildren() && node != root()) {
        const auto* skinTheme = SkinTheme::get(this);
        const auto part = node->isCollapsed() ? skinTheme->parts.iconTreeExpand() :
                                                skinTheme->parts.iconTreeCollapse();
        const auto buttonPoint = gfx::Point(
          point.x + (currentDepthSpacing - part->size().w - itemSpacing),
          guiscaled_center(point.y, height, part->size().h));
        if (coloredIcons())
          g->drawRgbaSurface(part->bitmap(0), buttonPoint.x, buttonPoint.y);
        else
          g->drawColoredRgbaSurface(part->bitmap(0),
                                    textColors.text.color(),
                                    buttonPoint.x,
                                    buttonPoint.y);
      }

      // Text background when selected
      if (node == selected() && isEnabled()) {
        g->drawRect(
          gfx::RectF(textPoint - gfx::Point(itemSpacing / 2, itemSpacing / 2),
                     node->textBlob()->bounds().size() + gfx::SizeF(itemSpacing, itemSpacing)),
          selectedBackgroundPaint);
      }

      // Text
      g->drawTextBlob(node->textBlob(),
                      textPoint,
                      node == selected() && isEnabled() ? selectedPaint : textPaint);
    }

    point.y += height + (itemSpacing * 2);
  }

  // Drop preview
  if (m_dragNode && m_dropTarget) {
    int targetNodeY = 0;
    for (auto* node = root(); node; node = node->nextInTree()) {
      if (node == m_dropTarget)
        break;
      targetNodeY += nodeHeight(node);
    }

    if (m_dropPos == DropPos::Inside) {
      auto basePoint = clientChildrenBounds().origin() + gfx::PointF(border().size());
      int lineX = m_dropTarget->depth() * depthSpacing;
      gfx::PointF tp(basePoint.x + lineX, basePoint.y + targetNodeY);
      if (m_dropTarget->textBlob()) {
        Paint fillPaint;
        fillPaint.color(selectedBackgroundPaint.color());
        fillPaint.style(Paint::Fill);
        g->drawRect(gfx::RectF(tp - gfx::PointF(itemSpacing / 2, itemSpacing / 2),
                               m_dropTarget->textBlob()->bounds().size() +
                                 gfx::SizeF(itemSpacing, itemSpacing)),
                    fillPaint);
        g->drawTextBlob(m_dropTarget->textBlob(), tp, selectedPaint);
      }
    }
    else {
      Paint linePaint;
      linePaint.color(selectedBackgroundPaint.color());
      linePaint.style(Paint::Stroke);
      int lineY = targetNodeY;
      if (m_dropPos == DropPos::After)
        lineY += nodeHeight(m_dropTarget);
      int lineX = m_dropTarget->depth() * depthSpacing;
      g->drawRect(gfx::Rect(lineX, lineY, fullWidth - lineX, 0), linePaint);
      g->drawRect(gfx::Rect(lineX, lineY - 1, fullWidth - lineX, 2), linePaint);
    }
  }

  // Drag overlay
  if (m_dragNode) {
    gfx::Point pos = m_dragMousePos - bounds().origin() - m_dragOffset;
    const auto dragHeight = m_dragNode->textBlob() ? m_dragNode->textBlob()->bounds().h :
                                                     fontHeight;
    if (m_dragNode->icon()) {
      int iconX = pos.x - m_dragNode->icon()->size().w - itemSpacing;
      int iconY = guiscaled_center(pos.y, dragHeight, m_dragNode->icon()->size().h);
      if (coloredIcons())
        g->drawRgbaSurface(m_dragNode->icon()->bitmap(0), iconX, iconY);
      else
        g->drawColoredRgbaSurface(m_dragNode->icon()->bitmap(0),
                                  textColors.text.color(),
                                  iconX,
                                  iconY);
    }
    if (m_dragNode->textBlob())
      g->drawTextBlob(m_dragNode->textBlob(), gfx::PointF(pos), textPaint);
  }
}

void ToolsetTree::onInitTheme(InitThemeEvent& ev)
{
  Tree::onInitTheme(ev);

  auto* theme = SkinTheme::get(this);
  const auto iconPart = theme->parts.iconTreeCollapse();

  m_themeCache.itemSpacing = theme->dimensions.treeItemSpacing();
  m_themeCache.rowHeight = font()->metrics(nullptr) + (m_themeCache.itemSpacing * 2);
  m_themeCache.textColors = theme->getTextColors(this);
  m_themeCache.depthSpacing = iconPart ? iconPart->size().w + m_themeCache.itemSpacing * 6 :
                                         m_themeCache.itemSpacing * 6;
}

void ToolsetTree::onSizeHint(SizeHintEvent& ev)
{
  gfx::Size hint = border().size() + m_themeCache.itemSpacing;
  for (const auto* node = root(); node; node = node->nextInTree()) {
    int width = 0;
    if (node->textBlob()) {
      width = std::ceil(node->textBlob()->bounds().w);
    }
    else {
      width = std::ceil(font()->textLength(node->text()));
    }

    width += (node->depth() * m_themeCache.depthSpacing) + (border().size().w * 2);

    hint.w = std::max<int>(hint.w, width);
    hint.h += nodeHeight(node);
  }

  ev.setSizeHint(hint);
}

} // namespace app
