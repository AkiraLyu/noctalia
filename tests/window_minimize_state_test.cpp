#include "compositors/umbriel/umbriel_runtime.h"
#include "compositors/umbriel/umbriel_workspace_backend.h"
#include "tests/test_check.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

  struct Fixture {
    int workspaceChanges = 0;
    int windowChanges = 0;
    compositors::umbriel::UmbrielRuntime runtime;
    UmbrielWorkspaceBackend backend{runtime};

    Fixture() {
      backend.setChangeCallback([this] { ++workspaceChanges; });
      backend.setWindowChangeCallback([this] { ++windowChanges; });
    }
  };

  nlohmann::json windowSnapshot(std::string_view id, bool minimized, bool minimizable) {
    return {
        {"id", id},
        {"workspace", "DP-1:1"},
        {"app_id", "sample.app"},
        {"title", "Shared title"},
        {"active", false},
        {"x", 10},
        {"y", 20},
        {"minimized", minimized},
        {"minimizable", minimizable},
    };
  }

  const WorkspaceWindow& windowForId(const std::vector<WorkspaceWindow>& windows, std::string_view id) {
    const auto it = std::ranges::find(windows, id, &WorkspaceWindow::windowId);
    TEST_CHECK(it != windows.end());
    return *it;
  }

  void checkState(const UmbrielWorkspaceBackend& backend, std::string_view id, bool minimized, bool minimizable) {
    const auto windows = backend.workspaceWindows();
    const auto& window = windowForId(windows, id);
    TEST_CHECK(window.minimizeState.has_value());
    TEST_CHECK(window.minimizeState->minimized == minimized);
    TEST_CHECK(window.minimizeState->minimizable == minimizable);
  }

  void checkUnknownState(const UmbrielWorkspaceBackend& backend, std::string_view id) {
    const auto windows = backend.workspaceWindows();
    TEST_CHECK(!windowForId(windows, id).minimizeState.has_value());
  }

  void checkSnapshotChanges() {
    Fixture fixture;
    auto snapshot = nlohmann::json::array({windowSnapshot("first", false, true), windowSnapshot("second", true, true)});
    snapshot[0]["active"] = true;
    fixture.backend.handleEvent("windows", snapshot);

    TEST_CHECK(fixture.windowChanges == 1);
    TEST_CHECK(fixture.backend.focusedWindowId() == "first");
    const auto windows = fixture.backend.workspaceWindows("DP-1");
    TEST_CHECK(windows.size() == 2);
    TEST_CHECK(windowForId(windows, "first").title == windowForId(windows, "second").title);
    checkState(fixture.backend, "first", false, true);
    checkState(fixture.backend, "second", true, true);

    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.windowChanges == 1);

    // Eligibility and minimise state can change without any presentation metadata changing.
    fixture.backend.setChangeCallback({});
    snapshot[0]["minimizable"] = false;
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.windowChanges == 2);
    checkState(fixture.backend, "first", false, false);
    checkState(fixture.backend, "second", true, true);

    snapshot[1]["minimized"] = false;
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.windowChanges == 3);
    checkState(fixture.backend, "first", false, false);
    checkState(fixture.backend, "second", false, true);

    fixture.backend.setChangeCallback([&fixture] { ++fixture.workspaceChanges; });
    const int workspaceChangesBeforeReset = fixture.workspaceChanges;
    fixture.backend.handleStreamReset();
    TEST_CHECK(fixture.backend.workspaceWindows().empty());
    TEST_CHECK(!fixture.backend.focusedWindowId().has_value());
    TEST_CHECK(fixture.windowChanges == 4);
    TEST_CHECK(fixture.workspaceChanges == workspaceChangesBeforeReset + 1);

    snapshot[0]["minimized"] = true;
    snapshot[0]["minimizable"] = true;
    snapshot[0]["active"] = false;
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.windowChanges == 5);
    checkState(fixture.backend, "first", true, true);

    fixture.backend.handleEvent("windows", nlohmann::json::array());
    TEST_CHECK(fixture.backend.workspaceWindows().empty());
    TEST_CHECK(fixture.windowChanges == 6);
    fixture.backend.handleEvent("windows", nlohmann::json::array());
    TEST_CHECK(fixture.windowChanges == 6);
  }

  void checkMalformedStates() {
    Fixture fixture;
    const auto valid = nlohmann::json::array({windowSnapshot("first", true, true)});
    const std::array<nlohmann::json, 5> invalidValues = {
        nullptr, "true", 1, nlohmann::json::object(), nlohmann::json::array()
    };
    for (const auto* key : {"minimized", "minimizable"}) {
      fixture.backend.handleEvent("windows", valid);
      auto snapshot = valid;
      snapshot[0].erase(key);
      fixture.backend.handleEvent("windows", snapshot);
      checkUnknownState(fixture.backend, "first");

      for (const auto& invalid : invalidValues) {
        fixture.backend.handleEvent("windows", valid);
        checkState(fixture.backend, "first", true, true);
        const int changesBeforeInvalid = fixture.windowChanges;
        snapshot = valid;
        snapshot[0][key] = invalid;
        fixture.backend.handleEvent("windows", snapshot);
        checkUnknownState(fixture.backend, "first");
        TEST_CHECK(fixture.windowChanges == changesBeforeInvalid + 1);
      }
    }

    // A different spelling cannot supply a missing canonical field.
    auto snapshot = valid;
    snapshot[0].erase("minimized");
    snapshot[0]["minimised"] = true;
    fixture.backend.handleEvent("windows", snapshot);
    checkUnknownState(fixture.backend, "first");

    fixture.backend.handleEvent("windows", valid);
    const int changesBeforeInvalidSnapshot = fixture.windowChanges;
    fixture.backend.handleEvent("windows", nlohmann::json::object());
    checkUnknownState(fixture.backend, "first");
    TEST_CHECK(fixture.windowChanges == changesBeforeInvalidSnapshot + 1);
  }

  void checkWorkspaceSnapshots() {
    Fixture fixture;
    std::vector<Workspace> firstOutput{{.id = "DP-1:1", .active = true}, {.id = "DP-1:2"}};
    std::vector<Workspace> secondOutput{{.id = "DP-2:1"}};
    auto snapshot = nlohmann::json::array({
        {{"id", "DP-1:1"}, {"occupied", false}},
        {{"id", "DP-1:2"}, {"occupied", true}},
        {{"id", "DP-2:1"}, {"occupied", true}},
    });

    fixture.backend.handleEvent("workspaces", snapshot);
    TEST_CHECK(fixture.workspaceChanges == 1);
    TEST_CHECK(fixture.windowChanges == 0);
    fixture.backend.apply(firstOutput, "DP-1");
    fixture.backend.apply(secondOutput, "DP-2");
    TEST_CHECK(!firstOutput[0].occupied);
    TEST_CHECK(firstOutput[1].occupied);
    TEST_CHECK(secondOutput[0].occupied);

    // A pinned window has a workspace ID without making that workspace occupied.
    fixture.backend.handleEvent("windows", nlohmann::json::array({windowSnapshot("pinned", false, false)}));
    fixture.backend.apply(firstOutput, "DP-1");
    TEST_CHECK(!firstOutput[0].occupied);
    TEST_CHECK(firstOutput[1].occupied);
    const int changesBeforeRepeat = fixture.workspaceChanges;
    fixture.backend.handleEvent("workspaces", snapshot);
    TEST_CHECK(fixture.workspaceChanges == changesBeforeRepeat);

    const std::array<nlohmann::json, 4> invalidSnapshots = {
        nlohmann::json::object(),
        nlohmann::json::array({nullptr}),
        nlohmann::json::array({{{"id", "DP-1:2"}}}),
        nlohmann::json::array({{{"id", "DP-1:2"}, {"occupied", "true"}}}),
    };
    for (const auto& invalid : invalidSnapshots) {
      fixture.backend.handleEvent("workspaces", invalid);
      fixture.backend.apply(firstOutput, "DP-1");
      TEST_CHECK(firstOutput[1].occupied);
      TEST_CHECK(fixture.workspaceChanges == changesBeforeRepeat);
    }

    snapshot[1]["occupied"] = false;
    fixture.backend.handleEvent("workspaces", snapshot);
    fixture.backend.apply(firstOutput, "DP-1");
    fixture.backend.apply(secondOutput, "DP-2");
    TEST_CHECK(!firstOutput[1].occupied);
    TEST_CHECK(secondOutput[0].occupied);
    TEST_CHECK(fixture.workspaceChanges == changesBeforeRepeat + 1);

    snapshot.erase(2);
    fixture.backend.handleEvent("workspaces", snapshot);
    fixture.backend.apply(secondOutput, "DP-2");
    TEST_CHECK(!secondOutput[0].occupied);

    snapshot[0]["occupied"] = true;
    fixture.backend.handleEvent("workspaces", snapshot);
    fixture.backend.handleStreamReset();
    fixture.backend.apply(firstOutput, "DP-1");
    TEST_CHECK(!firstOutput[0].occupied);
    TEST_CHECK(!firstOutput[1].occupied);
  }

  void checkWorkspaceNotificationRouting() {
    Fixture fixture;
    auto snapshot = nlohmann::json::array({windowSnapshot("first", false, true)});
    snapshot[0]["active"] = true;
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.workspaceChanges == 1);

    snapshot[0]["x"] = 50;
    snapshot[0]["y"] = 60;
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.workspaceChanges == 1);
    TEST_CHECK(fixture.windowChanges == 2);
    const auto windows = fixture.backend.workspaceWindows();
    TEST_CHECK(windowForId(windows, "first").x == 50);
    TEST_CHECK(windowForId(windows, "first").y == 60);

    snapshot[0]["minimizable"] = false;
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.workspaceChanges == 1);
    TEST_CHECK(fixture.windowChanges == 3);
    checkState(fixture.backend, "first", false, false);

    snapshot[0]["active"] = false;
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.workspaceChanges == 2);
    TEST_CHECK(!fixture.backend.focusedWindowId().has_value());

    snapshot[0]["workspace"] = "DP-2:1";
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.workspaceChanges == 3);

    snapshot[0]["title"] = "Updated title";
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.workspaceChanges == 4);

    fixture.backend.handleEvent("windows", nlohmann::json::array());
    TEST_CHECK(fixture.workspaceChanges == 5);
    TEST_CHECK(fixture.windowChanges == 7);
  }

  void checkGeometryAndCollisionWindows() {
    Fixture fixture;
    int geometryChanges = 0;
    fixture.backend.setWindowGeometryChangeCallback([&] { ++geometryChanges; });

    auto snapshot = nlohmann::json::array({
        {
            {"id", "w1"},
            {"workspace", "DP-1:1"},
            {"output", "DP-1"},
            {"app_id", "foot"},
            {"title", "Foot"},
            {"active", true},
            {"visible", true},
            {"pinned", false},
            {"x", 10},
            {"y", 20},
            {"w", 800},
            {"h", 600},
            {"minimized", false},
            {"minimizable", true},
        },
        {
            {"id", "scratch"},
            {"workspace", ""},
            {"output", "DP-1"},
            {"app_id", "scratch.app"},
            {"title", "Scratch"},
            {"active", false},
            {"visible", true},
            {"pinned", false},
            {"x", 100},
            {"y", 100},
            {"w", 400},
            {"h", 300},
            {"minimized", false},
            {"minimizable", true},
        },
    });
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.windowChanges == 1);
    TEST_CHECK(geometryChanges == 0);
    // workspaceWindows() still omits scratchpad windows without a workspace, while
    // collisionWindows() includes visible scratchpad/pinned windows.
    TEST_CHECK(fixture.backend.workspaceWindows("DP-1").size() == 1);
    const auto collision = fixture.backend.collisionWindows("DP-1");
    TEST_CHECK(collision.size() == 2);
    const auto& w1 = windowForId(collision, "w1");
    TEST_CHECK(w1.geometry.has_value());
    TEST_CHECK(w1.geometry->x == 10 && w1.geometry->y == 20 && w1.geometry->width == 800 && w1.geometry->height == 600);
    TEST_CHECK(w1.visibleOnScreen == std::optional<bool>(true));

    // Geometry-only change routes to windowGeometryChangeCallback without triggering full windowChanges.
    snapshot[0]["x"] = 250;
    snapshot[0]["w"] = 900;
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.windowChanges == 1);
    TEST_CHECK(geometryChanges == 1);
    const auto movedCollision = fixture.backend.collisionWindows("DP-1");
    const auto& w1Moved = windowForId(movedCollision, "w1");
    TEST_CHECK(w1Moved.geometry.has_value() && w1Moved.geometry->x == 250 && w1Moved.geometry->width == 900);
  }

  class ScopedEnvVar {
  public:
    ScopedEnvVar(const char* name, const char* value) : m_name(name) {
      if (const char* prev = std::getenv(name); prev != nullptr) {
        m_hadPrev = true;
        m_prevValue = prev;
      }
      if (value != nullptr) {
        ::setenv(name, value, 1);
      } else {
        ::unsetenv(name);
      }
    }

    ~ScopedEnvVar() {
      if (m_hadPrev) {
        ::setenv(m_name.c_str(), m_prevValue.c_str(), 1);
      } else {
        ::unsetenv(m_name.c_str());
      }
    }

  private:
    std::string m_name;
    bool m_hadPrev = false;
    std::string m_prevValue;
  };

  void checkTitleBarCollisionGeometry() {
    {
      ScopedEnvVar envGuard("UMBRIEL_SOCKET", "/nonexistent/noctalia_umbriel_test.sock");
      Fixture fixture;
      int geometryChanges = 0;
      fixture.backend.setWindowGeometryChangeCallback([&] { ++geometryChanges; });

      auto snapshot = nlohmann::json::array({
          {
              {"id", "ssd"},
              {"workspace", "DP-1:1"},
              {"output", "DP-1"},
              {"app_id", "foot"},
              {"title", "SSD window"},
              {"active", true},
              {"visible", true},
              {"pinned", false},
              {"title_bar", true},
              {"fullscreen", false},
              {"x", 0},
              {"y", 28},
              {"w", 1200},
              {"h", 700},
              {"minimized", false},
              {"minimizable", true},
          },
          {
              {"id", "csd"},
              {"workspace", "DP-1:1"},
              {"output", "DP-1"},
              {"app_id", "org.gnome.App"},
              {"title", "CSD window"},
              {"active", false},
              {"visible", true},
              {"pinned", false},
              {"title_bar", false},
              {"fullscreen", false},
              {"x", 0},
              {"y", 28},
              {"w", 800},
              {"h", 600},
              {"minimized", false},
              {"minimizable", true},
          },
          {
              {"id", "fs"},
              {"workspace", "DP-1:1"},
              {"output", "DP-1"},
              {"app_id", "mpv"},
              {"title", "Fullscreen"},
              {"active", false},
              {"visible", true},
              {"pinned", false},
              {"title_bar", true},
              {"fullscreen", true},
              {"x", 0},
              {"y", 0},
              {"w", 1920},
              {"h", 1080},
              {"minimized", false},
              {"minimizable", true},
          },
          {
              {"id", "explicit"},
              {"workspace", "DP-1:1"},
              {"output", "DP-1"},
              {"app_id", "alacritty"},
              {"title", "Explicit height"},
              {"active", false},
              {"visible", true},
              {"pinned", false},
              {"title_bar", true},
              {"title_bar_height", 36},
              {"fullscreen", false},
              {"x", 10},
              {"y", 40},
              {"w", 500},
              {"h", 400},
              {"minimized", false},
              {"minimizable", true},
          },
      });
      fixture.backend.handleEvent("windows", snapshot);
      const auto windows = fixture.backend.collisionWindows("DP-1");

      const auto& ssd = windowForId(windows, "ssd");
      TEST_CHECK(ssd.y == 28);
      TEST_CHECK(ssd.geometry.has_value());
      TEST_CHECK(
          ssd.geometry->x == 0 && ssd.geometry->y == 0 && ssd.geometry->width == 1200 && ssd.geometry->height == 728
      );

      const auto& csd = windowForId(windows, "csd");
      TEST_CHECK(csd.geometry.has_value());
      TEST_CHECK(
          csd.geometry->x == 0 && csd.geometry->y == 28 && csd.geometry->width == 800 && csd.geometry->height == 600
      );

      const auto& fs = windowForId(windows, "fs");
      TEST_CHECK(fs.geometry.has_value());
      TEST_CHECK(
          fs.geometry->x == 0 && fs.geometry->y == 0 && fs.geometry->width == 1920 && fs.geometry->height == 1080
      );

      const auto& explicitWin = windowForId(windows, "explicit");
      TEST_CHECK(explicitWin.geometry.has_value());
      TEST_CHECK(
          explicitWin.geometry->x == 10
          && explicitWin.geometry->y == 4
          && explicitWin.geometry->width == 500
          && explicitWin.geometry->height == 436
      );

      // Toggling title_bar routes through lightweight geometry re-evaluation.
      snapshot[1]["title_bar"] = true;
      fixture.backend.handleEvent("windows", snapshot);
      TEST_CHECK(fixture.windowChanges == 1);
      TEST_CHECK(geometryChanges == 1);
      const auto updated = fixture.backend.collisionWindows("DP-1");
      const auto& updatedCsd = windowForId(updated, "csd");
      TEST_CHECK(updatedCsd.geometry.has_value() && updatedCsd.geometry->y == 0 && updatedCsd.geometry->height == 628);
    }

    // Verify querying Umbriel's configured appearance.title_bar.height via IPC and refreshing on theme events.
    const std::string socketPath = "/tmp/noctalia_umbriel_titlebar_test_" + std::to_string(::getpid()) + ".sock";
    ::unlink(socketPath.c_str());
    const int serverFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    TEST_CHECK(serverFd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    TEST_CHECK(socketPath.size() < sizeof(addr.sun_path));
    std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);
    TEST_CHECK(::bind(serverFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    TEST_CHECK(::listen(serverFd, 4) == 0);

    std::atomic<int> configuredHeight{34};
    std::atomic<bool> stopServer{false};
    std::thread serverThread([&] {
      while (!stopServer.load()) {
        pollfd pfd{.fd = serverFd, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, 50) <= 0) {
          continue;
        }
        const int clientFd = ::accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) {
          break;
        }
        std::array<char, 512> buf{};
        const ssize_t n = ::read(clientFd, buf.data(), buf.size());
        if (n > 0) {
          const std::string_view req(buf.data(), static_cast<std::size_t>(n));
          if (req.contains("config-get")) {
            const std::string reply =
                "{\"ok\":{\"override\":false,\"value\":" + std::to_string(configuredHeight.load()) + "}}\n";
            [[maybe_unused]] const ssize_t written = ::write(clientFd, reply.data(), reply.size());
          }
        }
        ::close(clientFd);
      }
    });

    {
      ScopedEnvVar envGuard("UMBRIEL_SOCKET", socketPath.c_str());
      Fixture fixture;
      int geometryChanges = 0;
      fixture.backend.setWindowGeometryChangeCallback([&] { ++geometryChanges; });

      fixture.backend.handleEvent(
          "windows",
          nlohmann::json::array({{
              {"id", "tiled"},
              {"workspace", "DP-1:1"},
              {"output", "DP-1"},
              {"app_id", "foot"},
              {"title", "Tiled"},
              {"active", true},
              {"visible", true},
              {"pinned", false},
              {"title_bar", true},
              {"fullscreen", false},
              {"x", 4},
              {"y", 38},
              {"w", 1000},
              {"h", 700},
              {"minimized", false},
              {"minimizable", true},
          }})
      );
      const auto initial = fixture.backend.collisionWindows("DP-1");
      const auto& tiled = windowForId(initial, "tiled");
      TEST_CHECK(tiled.geometry.has_value());
      TEST_CHECK(tiled.geometry->y == 4 && tiled.geometry->height == 734);

      configuredHeight.store(42);
      fixture.backend.handleEvent("theme", nlohmann::json::object());
      TEST_CHECK(geometryChanges == 1);
      const auto reloaded = fixture.backend.collisionWindows("DP-1");
      const auto& tiledReloaded = windowForId(reloaded, "tiled");
      TEST_CHECK(tiledReloaded.geometry.has_value());
      TEST_CHECK(tiledReloaded.geometry->y == -4 && tiledReloaded.geometry->height == 742);
    }

    stopServer.store(true);
    ::shutdown(serverFd, SHUT_RDWR);
    serverThread.join();
    ::close(serverFd);
    ::unlink(socketPath.c_str());
  }

} // namespace

int main() {
  checkSnapshotChanges();
  checkMalformedStates();
  checkWorkspaceSnapshots();
  checkWorkspaceNotificationRouting();
  checkGeometryAndCollisionWindows();
  checkTitleBarCollisionGeometry();
  return 0;
}
