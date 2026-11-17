#pragma once

#include "compositors/workspace_backend.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class CompositorPlatform;
struct wl_output;

namespace shell {

  struct SmartAutoHideRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    bool operator==(const SmartAutoHideRect&) const = default;
  };

  struct SmartAutoHideQueryState {
    bool overviewOpen = false;
    std::int32_t outputLogicalX = 0;
    std::int32_t outputLogicalY = 0;
    std::vector<Workspace> workspaces;
    std::vector<std::string> overlayWorkspaceKeys;
    std::vector<WorkspaceWindowAssignment> windows;
    std::optional<bool> hasUnminimizedToplevels;
  };

  bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace);

  SmartAutoHideRect computeOutputLocalVisualRect(
      std::uint32_t anchor, std::int32_t marginTop, std::int32_t marginRight, std::int32_t marginBottom,
      std::int32_t marginLeft, std::int32_t surfaceW, std::int32_t surfaceH, std::int32_t outputW, std::int32_t outputH,
      float panelX, float panelY, float panelW, float panelH
  ) noexcept;

  bool rectsIntersect(const SmartAutoHideRect& a, const SmartAutoHideRect& b) noexcept;

  bool
  smartAutoHideWantsPinnedVisibleFromState(const SmartAutoHideQueryState& state, const SmartAutoHideRect& surfaceRect);

  bool smartAutoHideWantsPinnedVisible(
      const CompositorPlatform& platform, wl_output* output, const SmartAutoHideRect& surfaceRect
  );

} // namespace shell
