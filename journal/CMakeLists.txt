# Journal parsing and history. Plain C++: no Qt.
add_library(journal STATIC
        src/feed.cpp
        src/game_event.cpp
        src/history_store.cpp
        src/journal_files.cpp
        src/journal_parser.cpp
        src/journal_service.cpp
        src/sql.cpp
        src/projections/carrier.cpp
        src/projections/ledger.cpp
        src/projections/mission_log.cpp
        src/projections/visited_system.cpp
)
target_include_directories(journal PUBLIC include PRIVATE src)
target_compile_features(journal PUBLIC cxx_std_23)
target_link_libraries(journal
        PUBLIC nlohmann_json::nlohmann_json
        PRIVATE sqlite3 notify spdlog::spdlog
)

if (PLUM_BUILD_TESTS)
    add_executable(journal_tests tests/journal_tests.cpp)
    target_include_directories(journal_tests PRIVATE src)
    target_link_libraries(journal_tests PRIVATE journal sqlite3 spdlog::spdlog doctest::doctest)
    plum_copy_runtime_dlls(journal_tests)
    enable_testing()
    add_test(NAME journal_tests COMMAND journal_tests)
endif ()
