cmake_minimum_required(VERSION 3.20)
project(boolean_search VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(SEARCH_SANITIZERS "AddressSanitizer и UndefinedBehaviorSanitizer" OFF)

find_package(ZLIB REQUIRED)
find_package(ICU REQUIRED COMPONENTS uc)
find_package(Threads REQUIRED)

add_library(search_core
    src/storage.cpp
    src/analyzer.cpp
    src/corpus.cpp
    src/index.cpp
    src/query.cpp
    src/benchmark.cpp
    src/web.cpp
)
target_include_directories(search_core PUBLIC include)
target_include_directories(search_core SYSTEM PRIVATE third_party)
target_link_libraries(search_core PUBLIC ZLIB::ZLIB ICU::uc Threads::Threads)
target_compile_options(search_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(SEARCH_SANITIZERS)
    target_compile_options(search_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(search_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(search_engine src/main.cpp)
target_include_directories(search_engine SYSTEM PRIVATE third_party)
target_compile_definitions(search_engine PRIVATE SEARCH_WEB_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/web")
target_link_libraries(search_engine PRIVATE search_core)

enable_testing()
add_executable(search_tests tests/tests.cpp)
target_include_directories(search_tests SYSTEM PRIVATE third_party)
target_link_libraries(search_tests PRIVATE search_core)
add_test(NAME search_tests COMMAND search_tests)
add_test(NAME cli_restart
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_restart.sh $<TARGET_FILE:search_engine>
)

find_program(SEARCH_CURL curl)
find_program(SEARCH_JQ jq)
if(SEARCH_CURL AND SEARCH_JQ)
    add_test(NAME web_api
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/web.sh
            $<TARGET_FILE:search_engine> ${CMAKE_CURRENT_SOURCE_DIR}/web
    )
    set_tests_properties(web_api PROPERTIES TIMEOUT 45)

    find_program(SEARCH_FLOCK flock)
    find_program(SEARCH_SS ss)
    if(SEARCH_FLOCK AND SEARCH_SS)
        add_test(NAME web_lifecycle
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/web_lifecycle.sh
                $<TARGET_FILE:search_engine> ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(web_lifecycle PROPERTIES TIMEOUT 45)
    endif()
endif()
