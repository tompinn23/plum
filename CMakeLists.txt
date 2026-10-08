cmake_minimum_required(VERSION 4.3)
project(plum LANGUAGES C CXX)

if (MSVC)
    # CMake has no C++26 flag for MSVC yet; C++23 there means /std:c++latest.
    set(CMAKE_CXX_STANDARD 23)
else ()
    set(CMAKE_CXX_STANDARD 26)
endif ()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The version is git's description of the checkout: the nearest tag, or the commit, plus -dirty for
# uncommitted changes. It is taken at configure time; a commit or checkout reconfigures.
find_package(Git QUIET)
set(PLUM_VERSION "unknown")
if (GIT_FOUND AND EXISTS ${CMAKE_SOURCE_DIR}/.git)
    execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE PLUM_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            ${CMAKE_SOURCE_DIR}/.git/HEAD ${CMAKE_SOURCE_DIR}/.git/logs/HEAD)
endif ()
message(STATUS "plum version ${PLUM_VERSION}")

option(PLUM_BUILD_TESTS "Build the journal library tests" ON)

# Point CMake at the local Qt install unless the user already supplied one.
if (WIN32 AND NOT CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "C:/Qt/6.10.3/msvc2022_64")
endif ()

# ── Dependencies ────────────────────────────────────────────────────────────

include(cmake/CPM.cmake)

CPMAddPackage("gh:nlohmann/json@3.12.0")
CPMAddPackage("gh:marzer/tomlplusplus@3.4.0")
CPMAddPackage(
        NAME spdlog
        GITHUB_REPOSITORY gabime/spdlog
        VERSION 1.15.3
        OPTIONS "SPDLOG_USE_STD_FORMAT ON"
)

CPMAddPackage(
        NAME sqlite3
        VERSION 3.50.4
        URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
        URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
        DOWNLOAD_ONLY YES
)
add_library(sqlite3 SHARED ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_OMIT_DEPRECATED)
if (WIN32)
    # Every public function is declared SQLITE_API, so this exports exactly the C API.
    target_compile_definitions(sqlite3
            PRIVATE "SQLITE_API=__declspec(dllexport)"
            INTERFACE "SQLITE_API=__declspec(dllimport)")
endif ()

# Copies the DLLs a target links against (sqlite3.dll) next to its executable so it runs in place.
function(plum_copy_runtime_dlls target)
    if (WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
                COMMAND_EXPAND_LISTS)
    endif ()
endfunction()
if (MSVC)
    target_compile_options(sqlite3 PRIVATE /w)
else ()
    target_compile_options(sqlite3 PRIVATE -w)
endif ()

if (PLUM_BUILD_TESTS)
    CPMAddPackage("gh:doctest/doctest@2.4.12")
    enable_testing()
endif ()

set(NOTIFY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NOTIFY_BUILD_TESTS ${PLUM_BUILD_TESTS} CACHE BOOL "" FORCE)


add_subdirectory(notify)
add_subdirectory(journal)

# ── Application ─────────────────────────────────────────────────────────────

find_package(Qt6 6.10 REQUIRED COMPONENTS Widgets NetworkAuth)
qt_standard_project_setup()

qt_add_executable(plum
        main.cpp
        src/main_window.cpp include/main_window.hpp
        src/sidebar.cpp include/sidebar.hpp
        src/feed_subscriber.cpp include/feed_subscriber.hpp
        src/dashboard.cpp include/dashboard.hpp
        src/dashboards/commander_dashboard.cpp src/dashboards/commander_dashboard.hpp
        src/dashboards/carrier_dashboard.cpp src/dashboards/carrier_dashboard.hpp
        src/dashboards/massacre_dashboard.cpp src/dashboards/massacre_dashboard.hpp
        src/dashboards/event_log_dashboard.cpp src/dashboards/event_log_dashboard.hpp
        src/journal_dialog.cpp include/journal_dialog.hpp
        src/journal_sources.cpp include/journal_sources.hpp
        src/overlays.cpp include/overlays.hpp
        src/game_windows.cpp include/game_windows.hpp
        src/client.cpp include/client.hpp
        src/config.cpp include/config.hpp
        src/commodities.cpp include/commodities.hpp
        src/overlays/station_info.cpp include/overlays/station_info.hpp
        src/registry.cpp include/registry.hpp
)

qt_add_resources(plum "resources"
        PREFIX "/"
        FILES
        resources/theme.qss
        resources/commodity.csv
)

target_include_directories(plum PRIVATE include)
target_link_libraries(plum PRIVATE Qt6::Widgets Qt6::NetworkAuth journal spdlog::spdlog tomlplusplus::tomlplusplus)
set_target_properties(plum PROPERTIES WIN32_EXECUTABLE ON)
if (WIN32)
    target_link_libraries(plum PRIVATE dwmapi)
endif ()

# Issued by Frontier for the Companion API. Empty: the Link buttons explain there is none.
set(PLUM_CAPI_CLIENT_ID "" CACHE STRING "Frontier Companion API client id")
target_compile_definitions(plum PRIVATE PLUM_CAPI_CLIENT_ID="${PLUM_CAPI_CLIENT_ID}")
plum_copy_runtime_dlls(plum)

# Copy the Qt runtime next to the executable so it runs straight from the IDE.
if (WIN32 AND TARGET Qt6::windeployqt)
    add_custom_command(TARGET plum POST_BUILD
            COMMAND Qt6::windeployqt --no-translations --no-compiler-runtime
            --no-system-d3d-compiler --no-opengl-sw "$<TARGET_FILE:plum>"
            COMMENT "Deploying Qt runtime")
endif ()

# ── Install / packaging ─────────────────────────────────────────────────────
# `cmake --install <build> --prefix <dir>` produces a self-contained app: the
# executable, sqlite3 and the Qt libraries/plugins it needs (windeployqt on Windows).

install(TARGETS plum sqlite3
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

qt_generate_deploy_app_script(
        TARGET plum
        OUTPUT_SCRIPT plum_deploy_script
        NO_TRANSLATIONS
)
install(SCRIPT ${plum_deploy_script})
