#include "shell/smart_auto_hide.h"

#include "shell/wayland/layer_surface.h"

#include <algorithm>
#include <cmath>

namespace shell {

  bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace) {
    return switcher_membership::workspaceKeyMatchesAssignment(assignmentKey, workspace);
  }

  SmartAutoHideRect computeOutputLocalVisualRect(
      std::uint32_t anchor, std::int32_t marginTop, std::int32_t marginRight, std::int32_t marginBottom,
      std::int32_t marginLeft, std::int32_t surfaceW, std::int32_t surfaceH, std::int32_t outputW, std::int32_t outputH,
      float panelX, float panelY, float panelW, float panelH
  ) noexcept {
    if (surfaceW <= 0 || surfaceH <= 0 || panelW <= 0.0F || panelH <= 0.0F) {
      return {};
    }

    const bool aTop = (anchor & LayerShellAnchor::Top) != 0;
    const bool aBottom = (anchor & LayerShellAnchor::Bottom) != 0;
    const bool aLeft = (anchor & LayerShellAnchor::Left) != 0;
    const bool aRight = (anchor & LayerShellAnchor::Right) != 0;

    std::int32_t surfX = marginLeft;
    if (aLeft && aRight) {
      surfX = marginLeft;
    } else if (aRight) {
      surfX = outputW - marginRight - surfaceW;
    } else if (!aLeft) {
      const std::int32_t availW = std::max<std::int32_t>(0, outputW - marginLeft - marginRight);
      surfX = marginLeft + (availW - surfaceW) / 2;
    }

    std::int32_t surfY = marginTop;
    if (aTop && aBottom) {
      surfY = marginTop;
    } else if (aBottom) {
      surfY = outputH - marginBottom - surfaceH;
    } else if (!aTop) {
      const std::int32_t availH = std::max<std::int32_t>(0, outputH - marginTop - marginBottom);
      surfY = marginTop + (availH - surfaceH) / 2;
    }

    return SmartAutoHideRect{
        .x = surfX + static_cast<std::int32_t>(std::lround(panelX)),
        .y = surfY + static_cast<std::int32_t>(std::lround(panelY)),
        .width = static_cast<std::int32_t>(std::lround(panelW)),
        .height = static_cast<std::int32_t>(std::lround(panelH)),
    };
  }

  bool rectsIntersect(const SmartAutoHideRect& a, const SmartAutoHideRect& b) noexcept {
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0) {
      return false;
    }
    return a.x < b.x + b.width && a.x + a.width > b.x && a.y < b.y + b.height && a.y + a.height > b.y;
  }

  bool
  smartAutoHideWantsPinnedVisibleFromState(const SmartAutoHideQueryState& state, const SmartAutoHideRect& surfaceRect) {
    if (state.overviewOpen) {
      return true;
    }

    const Workspace* active = nullptr;
    for (const auto& workspace : state.workspaces) {
      if (workspace.active) {
        active = &workspace;
        break;
      }
    }

    for (const auto& window : state.windows) {
      if (window.minimizeState.has_value() && window.minimizeState->minimized) {
        continue;
      }

      if (window.visibleOnScreen.has_value()) {
        if (!*window.visibleOnScreen) {
          continue;
        }
      } else {
        const bool onActiveWorkspace = active != nullptr && workspaceKeyMatchesAssignment(window.workspaceKey, *active);
        const bool onOverlayWorkspace = std::ranges::contains(state.overlayWorkspaceKeys, window.workspaceKey);
        if (!window.pinned && !onActiveWorkspace && !onOverlayWorkspace) {
          continue;
        }
      }

      if (!window.geometry.has_value()) {
        return false;
      }

      if (surfaceRect.width <= 0 || surfaceRect.height <= 0) {
        return false;
      }

      const std::int32_t localX =
          window.geometry->globalCoordinates ? (window.geometry->x - state.outputLogicalX) : window.geometry->x;
      const std::int32_t localY =
          window.geometry->globalCoordinates ? (window.geometry->y - state.outputLogicalY) : window.geometry->y;
      const SmartAutoHideRect windowRect{
          .x = localX,
          .y = localY,
          .width = window.geometry->width,
          .height = window.geometry->height,
      };
      if (rectsIntersect(surfaceRect, windowRect)) {
        return false;
      }
    }

    if (state.windows.empty()) {
      if (active != nullptr && state.overlayPresented) {
        return false;
      }
      if (state.hasUnminimizedToplevels == false) {
        return true;
      }
      if (active != nullptr && active->occupied) {
        return false;
      }
    }

    return true;
  }

} // namespace shell
