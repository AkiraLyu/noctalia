// Smart auto-hide pins the bar or dock visible while the output has nothing on screen. An overlay is
// on screen without being a workspace member, so it has to count as content even though the active
// workspace stays empty.

#include "compositors/umbriel/umbriel_runtime.h"
#include "compositors/umbriel/umbriel_workspace_backend.h"
#include "shell/smart_auto_hide.h"
#include "shell/wayland/layer_surface.h"
#include "tests/test_check.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

  using shell::SmartAutoHideQueryState;
  using shell::SmartAutoHideRect;
  using shell::smart_auto_hide::OutputContent;
  using shell::smart_auto_hide::outputHasContent;
  using shell::smart_auto_hide::wantsPinnedVisible;

  constexpr auto kAnyToplevel = [] { return std::optional<bool>{true}; };
  constexpr auto kNoToplevel = [] { return std::optional<bool>{false}; };
  constexpr auto kUnknownToplevel = [] { return std::optional<bool>{}; };

  Workspace workspace(std::string id, std::string name, std::uint32_t index, bool active, bool occupied) {
    return Workspace{
        .id = std::move(id),
        .name = std::move(name),
        .coordinates = {},
        .index = index,
        .active = active,
        .occupied = occupied,
    };
  }

  WorkspaceWindowAssignment assignment(std::string workspaceKey, bool minimized = false) {
    WorkspaceWindowAssignment entry{.windowId = "w1", .workspaceKey = std::move(workspaceKey)};
    if (minimized) {
      entry.minimizeState = WindowMinimizeState{.minimized = true, .minimizable = true};
    }
    return entry;
  }

  OutputContent contentOf(
      std::vector<Workspace> workspaces, std::vector<WorkspaceWindowAssignment> assignments,
      bool overlayPresented = false
  ) {
    return OutputContent{
        .workspaces = std::move(workspaces),
        .assignments = std::move(assignments),
        .overlayPresented = overlayPresented,
    };
  }

  void testEmptyWorkspaceHasNoContent() {
    const auto content = contentOf({workspace("1", "1", 1, true, false)}, {});
    TEST_CHECK(!outputHasContent(content, kAnyToplevel));
    TEST_CHECK(wantsPinnedVisible(content, false, kAnyToplevel));
  }

  void testWindowOnTheActiveWorkspaceIsContent() {
    const auto content = contentOf({workspace("1", "1", 1, true, true)}, {assignment("1")});
    TEST_CHECK(outputHasContent(content, kNoToplevel));
    TEST_CHECK(!wantsPinnedVisible(content, false, kNoToplevel));
  }

  void testWindowOnAnotherWorkspaceLeavesThisOneEmpty() {
    const auto content =
        contentOf({workspace("1", "1", 1, true, false), workspace("2", "2", 2, false, true)}, {assignment("2")});
    TEST_CHECK(!outputHasContent(content, kUnknownToplevel));
    TEST_CHECK(wantsPinnedVisible(content, false, kUnknownToplevel));
  }

  void testMinimizedWindowLeavesTheWorkspaceEmpty() {
    const auto content = contentOf({workspace("1", "1", 1, true, true)}, {assignment("1", true)});
    TEST_CHECK(!outputHasContent(content, kAnyToplevel));
    TEST_CHECK(wantsPinnedVisible(content, false, kAnyToplevel));
  }

  // A scratchpad floats over an otherwise empty workspace. Its members carry no workspace assignment,
  // so the overlay flag is the only evidence that something is on screen.
  void testPresentedOverlayHidesOnAnEmptyWorkspace() {
    const auto content = contentOf({workspace("1", "1", 1, true, false)}, {}, true);
    TEST_CHECK(outputHasContent(content, kAnyToplevel));
    TEST_CHECK(!wantsPinnedVisible(content, false, kAnyToplevel));
  }

  void testOverviewKeepsTheSurfaceVisible() {
    const auto content = contentOf({workspace("1", "1", 1, true, true)}, {assignment("1")}, true);
    TEST_CHECK(wantsPinnedVisible(content, true, kAnyToplevel));
  }

  void testToplevelFallbackWhenTheBackendPublishesNoAssignments() {
    const auto empty = contentOf({workspace("1", "1", 1, true, false)}, {});
    TEST_CHECK(!outputHasContent(empty, kNoToplevel));
    const auto occupied = contentOf({workspace("1", "1", 1, true, true)}, {});
    TEST_CHECK(outputHasContent(occupied, kAnyToplevel));
  }

  void testUnknownToplevelStateFallsBackToOccupancy() {
    TEST_CHECK(!outputHasContent(contentOf({workspace("1", "1", 1, true, false)}, {}), kUnknownToplevel));
    TEST_CHECK(outputHasContent(contentOf({workspace("1", "1", 1, true, true)}, {}), kUnknownToplevel));
  }

  // A presented scratchpad member carries no workspace, so the backend has to read `scratchpad` and
  // `visible` for the overlay to exist at all.
  nlohmann::json
  windowEntry(std::string id, std::string workspace, std::string scratchpad, bool visible, std::string output) {
    return nlohmann::json{
        {"id", std::move(id)},
        {"workspace", std::move(workspace)},
        {"scratchpad", std::move(scratchpad)},
        {"visible", visible},
        {"output", std::move(output)},
        {"app_id", "foot"},
        {"title", "shell"},
        {"active", false},
        {"x", 0},
        {"y", 0},
        {"minimized", false},
        {"minimizable", true},
    };
  }

  void testUmbrielReportsAPresentedScratchpadAsAnOverlay() {
    compositors::umbriel::UmbrielRuntime runtime;
    UmbrielWorkspaceBackend backend(runtime);

    backend.handleEvent("windows", nlohmann::json::array({windowEntry("w1", "", "notes", true, "eDP-1")}));
    TEST_CHECK(backend.hasOnScreenOverlay("eDP-1"));
    TEST_CHECK(!backend.hasOnScreenOverlay("HDMI-A-1"));
    // The taskbar's contract is unchanged: members are not workspace windows.
    TEST_CHECK(backend.workspaceWindows("eDP-1").empty());

    // Hiding the pad flips `visible`, which is the transition Umbriel already reports.
    backend.handleEvent("windows", nlohmann::json::array({windowEntry("w2", "", "notes", false, "eDP-1")}));
    TEST_CHECK(!backend.hasOnScreenOverlay("eDP-1"));

    // A workspace window is not an overlay.
    backend.handleEvent("windows", nlohmann::json::array({windowEntry("w3", "eDP-1:1", "", true, "eDP-1")}));
    TEST_CHECK(!backend.hasOnScreenOverlay("eDP-1"));
    TEST_CHECK(backend.workspaceWindows("eDP-1").size() == 1);
  }

  void testOverlayWithoutAnActiveWorkspaceKeepsTheSurfaceVisible() {
    // Hotplug and transitions report no active workspace; an overlay flag alone must not move the surface.
    TEST_CHECK(!outputHasContent(contentOf({}, {}, true), kAnyToplevel));
    TEST_CHECK(wantsPinnedVisible(contentOf({}, {}, true), false, kAnyToplevel));
  }

  void testNumberedWorkspaceNamesMatchTheirNumber() {
    // Sway publishes "1: web" as both id and name while assignments use "1"; the shared matcher covers it.
    const auto content = contentOf({workspace("1: web", "1: web", 0, true, true)}, {assignment("1")});
    TEST_CHECK(outputHasContent(content, kNoToplevel));
  }

  void testOutputWithoutAnActiveWorkspaceHasNoContent() {
    const auto content = contentOf({workspace("1", "1", 1, false, true)}, {assignment("1")});
    TEST_CHECK(!outputHasContent(content, kAnyToplevel));
    TEST_CHECK(wantsPinnedVisible(content, false, kAnyToplevel));
  }

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

      // Presented overlay hides on an empty workspace when no collision windows are available.
      SmartAutoHideQueryState overlayState;
      overlayState.workspaces = {activeWorkspace("1", false)};
      overlayState.overlayPresented = true;
      TEST_CHECK(!shell::smartAutoHideWantsPinnedVisibleFromState(overlayState, rightBar));

      // Without an active workspace (hotplug/transition), overlay flag alone keeps surface visible.
      overlayState.workspaces.clear();
      TEST_CHECK(shell::smartAutoHideWantsPinnedVisibleFromState(overlayState, rightBar));
    }
  }

} // namespace

int main() {
  testEmptyWorkspaceHasNoContent();
  testWindowOnTheActiveWorkspaceIsContent();
  testWindowOnAnotherWorkspaceLeavesThisOneEmpty();
  testMinimizedWindowLeavesTheWorkspaceEmpty();
  testPresentedOverlayHidesOnAnEmptyWorkspace();
  testOverviewKeepsTheSurfaceVisible();
  testToplevelFallbackWhenTheBackendPublishesNoAssignments();
  testUnknownToplevelStateFallsBackToOccupancy();
  testOutputWithoutAnActiveWorkspaceHasNoContent();
  testOverlayWithoutAnActiveWorkspaceKeepsTheSurfaceVisible();
  testNumberedWorkspaceNamesMatchTheirNumber();
  testUmbrielReportsAPresentedScratchpadAsAnOverlay();
  checkRectIntersection();
  checkOutputLocalVisualRectMapping();
  checkCollisionDecisions();
  return 0;
}
