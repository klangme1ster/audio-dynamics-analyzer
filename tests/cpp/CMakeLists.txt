# Header-only / single-file dependencies are fetched as sources only (SOURCE_SUBDIR points at a
# directory without a CMakeLists.txt), so their own build scripts never run.
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR sources-only)
FetchContent_Declare(libebur128
    GIT_REPOSITORY https://github.com/jiixyj/libebur128.git
    GIT_TAG v1.2.6
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR sources-only)
FetchContent_MakeAvailable(doctest libebur128)

# libebur128: independent test oracle only (spec ADA-ORACLE), never linked into the product
add_library(ebur128_oracle STATIC ${libebur128_SOURCE_DIR}/ebur128/ebur128.c)
target_include_directories(ebur128_oracle PUBLIC ${libebur128_SOURCE_DIR}/ebur128)
if(MSVC)
    # MSVC has no <sys/queue.h>; libebur128 ships its own copy
    target_include_directories(ebur128_oracle PRIVATE ${libebur128_SOURCE_DIR}/ebur128/queue)
    target_compile_definitions(ebur128_oracle PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(ada_engine_tests
    test_main.cpp
    test_engine.cpp
    test_history.cpp
    test_transport.cpp
    test_realtime.cpp)
target_include_directories(ada_engine_tests SYSTEM PRIVATE ${doctest_SOURCE_DIR}) # third-party: no warnings
target_link_libraries(ada_engine_tests PRIVATE ada_engine ebur128_oracle)
ada_set_warnings(ada_engine_tests)
add_test(NAME ada_engine_tests COMMAND ada_engine_tests)

# Command-line runner used by the Python parity test (python/tests/test_parity.py)
add_executable(ada_cli ada_cli.cpp)
target_link_libraries(ada_cli PRIVATE ada_engine)
ada_set_warnings(ada_cli)
