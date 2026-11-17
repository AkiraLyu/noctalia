#include "compositors/compositor_platform.h"
#include "shell/smart_auto_hide.h"

namespace shell {

  bool smartAutoHideWantsPinnedVisible(
      const CompositorPlatform& platform, wl_output* output, const SmartAutoHideRect& surfaceRect
  ) {
    const WaylandOutput* wlOutput = platform.findOutputByWl(output);
    SmartAutoHideQueryState state{
        .overviewOpen = platform.hasOverviewState() && platform.isOverviewOpen(),
        .outputLogicalX = wlOutput != nullptr ? wlOutput->logicalX : 0,
        .outputLogicalY = wlOutput != nullptr ? wlOutput->logicalY : 0,
        .workspaces = platform.workspaces(output),
        .overlayWorkspaceKeys = platform.openOverlayWorkspaceKeys(output),
        .windows = platform.collisionWindows(output),
        .hasUnminimizedToplevels = platform.hasUnminimizedToplevels(output),
        .overlayPresented = platform.hasOnScreenOverlay(output),
    };
    return smartAutoHideWantsPinnedVisibleFromState(state, surfaceRect);
  }

} // namespace shell
