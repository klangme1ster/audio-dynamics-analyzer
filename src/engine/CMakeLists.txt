add_library(ada_engine STATIC
    DynamicsEngine.cpp
    HistoryStore.cpp
    KWeighting.cpp
    LoudnessHistogram.cpp
    Summary.cpp)

target_include_directories(ada_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ada_engine PUBLIC cxx_std_20)
set_target_properties(ada_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
ada_set_warnings(ada_engine)
