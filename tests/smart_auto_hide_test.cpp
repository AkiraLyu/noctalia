#include "shell/smart_auto_hide.h"
#include "shell/wayland/layer_surface.h"
#include "tests/test_check.h"

namespace {

  using shell::SmartAutoHideQueryState;
  using shell::SmartAutoHideRect;

  void checkRectIntersection() {
    const SmartAutoHideRect bar{0, 0, 1920, 40};
    // Exact boundary contact does not overlap.
    TEST_CHECK(!shell::rectsIntersect(bar, SmartAutoHideRect{0, 40, 1920, 1040}));
    TEST_CHECK(!shell::rectsIntersect(bar, SmartAutoHideRect{1920, 0, 400, 1080}));
    // Any positive-area overlap triggers collision.
    TEST_CHECK(shell::rectsIntersect(bar, SmartAutoHideRect{100, 39, 500, 500}));
    TEST_CHECK(shell::rectsIntersect(bar, SmartAutoHideRect{0, 0, 1920, 1080}));
    // Empty rects never intersect.
    TEST_CHECK(!shell::rectsIntersect(bar, SmartAutoHideRect{10, 10, 0, 100}));
    TEST_CHECK(!shell::rectsIntersect(SmartAutoHideRect{}, SmartAutoHideRect{0, 0, 100, 100}));
  }

  void checkOutputLocalVisualRectMapping() {
    // Top bar with margin_edge folded into gutter (top surface margin 0, bar visual y=8).
    const auto topBar = shell::computeOutputLocalVisualRect(
        LayerShellAnchor::Top | LayerShellAnchor::Left | LayerShellAnchor::Right, 0, 0, 0, 0, 1920, 48, 1920, 1080,
        12.0F, 8.0F, 1896.0F, 32.0F
    );
    TEST_CHECK((topBar == SmartAutoHideRect{12, 8, 1896, 32}));

    // Right vertical bar.
    const auto rightBar = shell::computeOutputLocalVisualRect(
        LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Right, 0, 0, 0, 0, 56, 1080, 1920, 1080,
        16.0F, 0.0F, 40.0F, 1080.0F
    );
    TEST_CHECK((rightBar == SmartAutoHideRect{1880, 0, 40, 1080}));

    // Centered bottom dock pill (only Bottom anchor, pill centered inside 1920-wide surface).
    const auto bottomDock = shell::computeOutputLocalVisualRect(
        LayerShellAnchor::Bottom, 0, 0, 0, 0, 1920, 80, 1920, 1080, 800.0F, 12.0F, 320.0F, 60.0F
    );
    TEST_CHECK((bottomDock == SmartAutoHideRect{800, 1012, 320, 60}));
  }

  Workspace activeWorkspace(std::string id = "1", bool occupied = true) {
    Workspace ws;
    ws.id = std::move(id);
    ws.active = true;
    ws.occupied = occupied;
    return ws;
  }

  WorkspaceWindowAssignment makeWindow(
      std::string id, std::string workspaceKey, std::optional<WindowGeometry> geometry = std::nullopt,
      std::optional<bool> visibleOnScreen = std::nullopt,
      std::optional<WindowMinimizeState> minimizeState = std::nullopt
  ) {
    WorkspaceWindowAssignment win;
    win.windowId = std::move(id);
    win.workspaceKey = std::move(workspaceKey);
    win.geometry = geometry;
    win.visibleOnScreen = visibleOnScreen;
    win.minimizeState = minimizeState;
    return win;
  }

  void checkCollisionDecisions() {
    const SmartAutoHideRect rightBar{1880, 0, 40, 1080};
    const SmartAutoHideRect bottomDock{800, 1010, 320, 62};

    // Overview open always keeps pinned visible.
    {
      SmartAutoHideQueryState state;
      state.overviewOpen = true;
      state.windows.push_back(makeWindow("1", "1", WindowGeometry{0, 0, 1920, 1080, true}, true));
      TEST_CHECK(shell::smartAutoHideWantsPinnedVisibleFromState(state, rightBar));
    }

    // Window on left half does not collide with right bar or centered bottom dock if above dock.
    {
      SmartAutoHideQueryState state;
      state.workspaces = {activeWorkspace()};
      state.windows.push_back(makeWindow("1", "1", WindowGeometry{0, 0, 900, 1000, true}, true));
      TEST_CHECK(shell::smartAutoHideWantsPinnedVisibleFromState(state, rightBar));
      TEST_CHECK(shell::smartAutoHideWantsPinnedVisibleFromState(state, bottomDock));
    }

    // Window expanded to cover bottom dock hides dock, still misses right bar.
    {
      SmartAutoHideQueryState state;
      state.workspaces = {activeWorkspace()};
      state.windows.push_back(makeWindow("1", "1", WindowGeometry{0, 0, 900, 1050, true}, true));
      TEST_CHECK(shell::smartAutoHideWantsPinnedVisibleFromState(state, rightBar));
      TEST_CHECK(!shell::smartAutoHideWantsPinnedVisibleFromState(state, bottomDock));
    }

    // Multi-monitor global coordinates subtract outputLogicalX / Y.
    {
      SmartAutoHideQueryState state;
      state.outputLogicalX = 1920;
      state.outputLogicalY = 0;
      state.workspaces = {activeWorkspace("DP-2:1")};
      // Window covers left half of DP-2: global [1920, 2820), local [0, 900).
      state.windows.push_back(makeWindow("2", "DP-2:1", WindowGeometry{1920, 0, 900, 1000, true}, true));
      TEST_CHECK(shell::smartAutoHideWantsPinnedVisibleFromState(state, rightBar));
      // Window covers right edge of DP-2: global [2920, 3840), local [1000, 1920).
      state.windows[0].geometry = WindowGeometry{2920, 0, 920, 1080, true};
      TEST_CHECK(!shell::smartAutoHideWantsPinnedVisibleFromState(state, rightBar));
    }

    // Output-local coordinates (e.g. Niri tile_pos_in_workspace_view) ignore outputLogicalX/Y.
    {
      SmartAutoHideQueryState state;
      state.outputLogicalX = 1920;
      state.outputLogicalY = 0;
      state.workspaces = {activeWorkspace()};
      // Scrolled off-screen to the left: [-800, -100).
      state.windows.push_back(makeWindow("niri-1", "1", WindowGeometry{-800, 0, 700, 1080, false}));
      TEST_CHECK(shell::smartAutoHideWantsPinnedVisibleFromState(state, rightBar));
      // Scrolled onto right bar: [1200, 1920).
      state.windows[0].geometry = WindowGeometry{1200, 0, 720, 1080, false};
      TEST_CHECK(!shell::smartAutoHideWantsPinnedVisibleFromState(state, rightBar));
    }

    // Minimized or non-visible windows are ignored even if their stored rect overlaps.
    {
      SmartAutoHideQueryState state;
      state.workspaces = {activeWorkspace()};
      state.windows.push_back(makeWindow(
          "min", "1", WindowGeometry{0, 0, 1920, 1080, true}, true,
          WindowMinimizeState{.minimized = true, .minimizable = true}
      ));
      state.windows.push_back(makeWindow("hidden-tab", "1", WindowGeometry{0, 0, 1920, 1080, true}, false));
      TEST_CHECK(shell::smartAutoHideWantsPinnedVisibleFromState(state, rightBar));
      TEST_CHECK(shell::smartAutoHideWantsPinnedVisibleFromState(state, bottomDock));
    }

    // Legacy fallback when compositor provides no window geometry.
    {
      SmartAutoHideQueryState state;
      state.workspaces = {activeWorkspace()};
      TEST_CHECK(!shell::smartAutoHideWantsPinnedVisibleFromState(state, rightBar));

      state.hasUnminimizedToplevels = false;
      TEST_CHECK(shell::smartAutoHideWantsPinnedVisibleFromState(state, rightBar));
    }
  }

} // namespace

int main() {
  checkRectIntersection();
  checkOutputLocalVisualRectMapping();
  checkCollisionDecisions();
  return 0;
}
