cmake_minimum_required(VERSION 3.19)
project(gloview VERSION "0.3.0" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(WLR_USE_UNSTABLE)
# -fno-gnu-unique: C++ static-init guard vars (_ZGV...) otherwise emit as STB_GNU_UNIQUE,
# which makes glibc mark the .so NODELETE so dlclose never unmaps it — a same-path
# `hyprctl plugin load` after unload then returns the CACHED old image (same dlopen handle)
# and the hot-reload silently runs stale code. Plain symbols let dlclose truly unmap.
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fPIC -fno-gnu-unique)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPR_DEPS REQUIRED hyprland)
# Raw GL (glCreateShader/glDrawArrays…) for the plugin's own blur shader.
pkg_check_modules(GLES REQUIRED glesv2)
# Lua's pkg-config module name varies by distro: `lua` (Arch), `lua5.4`/`lua-5.4`
# (Debian/Nix), or `luajit` (what Hyprland itself links). Take the first that exists.
pkg_search_module(LUA REQUIRED lua lua5.4 lua-5.4 lua54 luajit)

add_library(gloview SHARED
  src/main.cpp
  src/session/session.cpp
  src/build/build.cpp
  src/input/mouse.cpp
  src/input/keys.cpp
  src/actions/actions.cpp
  src/layout.cpp
  src/model/recency.cpp
  src/cursor.cpp
  src/blur.cpp
  src/pixel/cache.cpp
  src/clock/clock.cpp
  src/clock/clocks.cpp
  src/clock/curves.cpp
  src/config/config.cpp
  src/anim.cpp
  src/anim_resolve.cpp
  src/debug/log.cpp
  src/render/painter.cpp
  src/render/backdrop.cpp
  src/render/window_content.cpp
  src/render/tile_view.cpp
  src/render/carousel_view.cpp
  src/render/strip_view.cpp
  src/render/fx.cpp
)
set_target_properties(gloview PROPERTIES PREFIX "")  # produce gloview.so, not libgloview.so

target_include_directories(gloview PRIVATE ${HYPR_DEPS_INCLUDE_DIRS} ${LUA_INCLUDE_DIRS} ${GLES_INCLUDE_DIRS})
foreach(inc ${HYPR_DEPS_INCLUDE_DIRS})
  if(EXISTS "${inc}/hyprland/src")
    target_include_directories(gloview PRIVATE "${inc}/hyprland/src")
  endif()
endforeach()

target_link_directories(gloview PRIVATE ${HYPR_DEPS_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS} ${GLES_LIBRARY_DIRS})
target_link_libraries(gloview PRIVATE ${HYPR_DEPS_LIBRARIES} ${LUA_LIBRARIES} ${GLES_LIBRARIES})

install(TARGETS gloview LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

# Dev hot-reload (official same-path unload;load):  cmake --build build --target reload
# This is the upstream-documented dev reload (Hyprland wiki, Plugins/Development):
#   hyprctl plugin unload /abs/plugin.so ; hyprctl plugin load /abs/plugin.so
# It ONLY picks up new code because of -fno-gnu-unique above: C++ static-init guard vars
# (_ZGV...) otherwise emit as STB_GNU_UNIQUE, which makes glibc mark the .so NODELETE so
# dlclose never unmaps it and the same-path load returns the CACHED old image (same dlopen
# handle) -> a silent stale reload. With the flag, dlclose truly unmaps and the same path
# re-mmaps the fresh build. (If you ever load a pre-flag binary, that image sticks NODELETE
# for the life of the Hyprland process and poisons its path until you restart Hyprland once.)
# `hyprctl gloviewunload` (Overview::hardClose) first does a synchronous, animation-free
# teardown + cancels the recapture timer so the unload can't free plugin-owned render/timer
# state while Hyprland still references it (-> SEGV / 150% IPC-dead spin); the 0.15s settle
# lets one plugin-free frame flush the render pass. A plugin crash kills the whole session —
# prefer a nested Hyprland for risky changes. (For installed plugins the user path is
# `hyprpm enable/disable <name>` + `hyprpm reload`.)
# Layout and exit self-checks. Both are plain asserts-over-arithmetic, no
# compositor, so they run on every build: the carousel exit was shipped broken
# three times (its progress read with the wrong sign) and a screenshot does not
# tell a zero-size card from a small one.
enable_testing()
add_test(NAME recency
         COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "${CMAKE_CXX_COMPILER} -std=c++23 -Wall -Wextra ${CMAKE_SOURCE_DIR}/tests/recency_check.cpp ${CMAKE_SOURCE_DIR}/src/model/recency.cpp -o ${CMAKE_CURRENT_BINARY_DIR}/recency_check && ${CMAKE_CURRENT_BINARY_DIR}/recency_check")
add_test(NAME carousel_layout
         COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "${CMAKE_CXX_COMPILER} -std=c++23 -Wall -Wextra -I ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/layout_carousel_check.cpp ${CMAKE_SOURCE_DIR}/src/layout.cpp -o ${CMAKE_CURRENT_BINARY_DIR}/layout_check && ${CMAKE_CURRENT_BINARY_DIR}/layout_check")
add_test(NAME carousel_exit
         COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "${CMAKE_CXX_COMPILER} -std=c++23 -Wall -Wextra ${CMAKE_SOURCE_DIR}/tests/carousel_exit_check.cpp -o ${CMAKE_CURRENT_BINARY_DIR}/exit_check && ${CMAKE_CURRENT_BINARY_DIR}/exit_check")

add_test(NAME config_keys
         COMMAND bash -c "${CMAKE_CXX_COMPILER} -std=c++23 -Wall -Wextra ${CMAKE_SOURCE_DIR}/tests/config_keys_check.cpp -o ${CMAKE_CURRENT_BINARY_DIR}/cfg_keys_check && ${CMAKE_CURRENT_BINARY_DIR}/cfg_keys_check ${CMAKE_SOURCE_DIR}")

# The animation precedence chain, headless. Reachable only because resolve() was split
# out of anim.cpp: Config::Values lives in the Hyprland binary, so anim.cpp cannot be
# linked into a test at all.
add_test(NAME anim_resolve
         COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "${CMAKE_CXX_COMPILER} -std=c++23 -Wall -Wextra -I ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/anim_resolve_check.cpp ${CMAKE_SOURCE_DIR}/src/anim_resolve.cpp -o ${CMAKE_CURRENT_BINARY_DIR}/anim_resolve_check && ${CMAKE_CURRENT_BINARY_DIR}/anim_resolve_check")

# The shape-parameter table against the enum it is indexed by. A Param added without a
# key row used to read the next slot's value at runtime: no error, one wrong number.
add_test(NAME anim_params
         COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "${CMAKE_CXX_COMPILER} -std=c++23 -Wall -Wextra -I ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/anim_params_check.cpp ${CMAKE_SOURCE_DIR}/src/anim_resolve.cpp -o ${CMAKE_CURRENT_BINARY_DIR}/anim_params_check && ${CMAKE_CURRENT_BINARY_DIR}/anim_params_check")

# The style sets, and a scan of the motion files for comparisons against names that are
# not in them. Five string comparisons used to be the only record of what a style may be.
add_test(NAME anim_styles
         COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "${CMAKE_CXX_COMPILER} -std=c++23 -Wall -Wextra -I ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/anim_styles_check.cpp ${CMAKE_SOURCE_DIR}/src/anim_resolve.cpp -o ${CMAKE_CURRENT_BINARY_DIR}/anim_styles_check && ${CMAKE_CURRENT_BINARY_DIR}/anim_styles_check ${CMAKE_SOURCE_DIR}")

# The entry ladder, as arithmetic. staggered() reads the spread from anim::param() and
# cannot be linked, so the part under test takes the spread handed in.
add_test(NAME anim_stagger
         COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "${CMAKE_CXX_COMPILER} -std=c++23 -Wall -Wextra -I ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/anim_stagger_check.cpp ${CMAKE_SOURCE_DIR}/src/anim_resolve.cpp -o ${CMAKE_CURRENT_BINARY_DIR}/anim_stagger_check && ${CMAKE_CURRENT_BINARY_DIR}/anim_stagger_check")

# Which leaf drives the master clock, and the backdrop's exit rule. These were three
# separate expressions of two questions, and nobody checked that the three agreed.
add_test(NAME anim_master
         COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "${CMAKE_CXX_COMPILER} -std=c++23 -Wall -Wextra -I ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/anim_master_check.cpp ${CMAKE_SOURCE_DIR}/src/anim_resolve.cpp -o ${CMAKE_CURRENT_BINARY_DIR}/anim_master_check && ${CMAKE_CURRENT_BINARY_DIR}/anim_master_check")

add_test(NAME curves
         COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "${CMAKE_CXX_COMPILER} -std=c++23 -Wall -Wextra -I ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/curves_check.cpp ${CMAKE_SOURCE_DIR}/tests/hypr_stub.cpp ${CMAKE_SOURCE_DIR}/src/clock/curves.cpp -o ${CMAKE_CURRENT_BINARY_DIR}/curves_check -llua -lhyprutils && ${CMAKE_CURRENT_BINARY_DIR}/curves_check")

# The load seam. Needs the real .so, so it only means anything once it has been
# built: `ctest -R load_seam` afterwards, or CI, where the .so always exists.
# SKIP_RETURN_CODE so a machine without Hyprland reports skip, not a fake failure.
find_program(HYPRLAND_BIN Hyprland)
add_test(NAME load_seam
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/load_seam_check.sh
                 $<TARGET_FILE:gloview> ${HYPRLAND_BIN}
         SKIP_RETURN_CODE 77)

# Hook signatures against the running compositor's exports. A mismatch makes
# findFunctionsByName return nothing and initialize() fail, and hyprpm's plugin.load
# swallows that, so the symptom reads as "my config did not take".
add_test(NAME hook_signatures
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/hook_signature_check.sh
                 $<TARGET_FILE:gloview> ${HYPRLAND_BIN}
                 ${CMAKE_SOURCE_DIR}/src/session/session.cpp
         SKIP_RETURN_CODE 77)

add_custom_target(reload
  COMMAND bash -c "hyprctl gloviewunload || true"
  COMMAND ${CMAKE_COMMAND} -E sleep 0.15
  COMMAND bash -c "hyprctl plugin unload $<TARGET_FILE:gloview> || true"
  # Also drop a hyprpm-installed copy: it autoloads at session start and already holds the
  # shouldRenderWindow hooks, so loading the build path next to it fails to hook (two
  # instances can't trampoline the same prologue). No-op when not installed/loaded.
  # ($ENV{USER} resolves at configure time — a literal $USER gets mangled by the Makefile.)
  COMMAND bash -c "hyprctl plugin unload /var/cache/hyprpm/$ENV{USER}/gloview/gloview.so || true"
  COMMAND hyprctl plugin load $<TARGET_FILE:gloview>
  DEPENDS gloview
  USES_TERMINAL
  COMMENT "Hot-reloading gloview into the running Hyprland"
)
