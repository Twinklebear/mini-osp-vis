add_library(util
    util.cpp
    arcball_camera.cpp)

set_target_properties(util PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON)

target_link_libraries(util PUBLIC
    ospray::ospray
    rkcommon::rkcommon
    TBB::tbb
    glm::glm)

target_include_directories(util PUBLIC
    ${CMAKE_CURRENT_LIST_DIR})

target_compile_definitions(util PUBLIC
    -DTFN_WIDGET_NO_STB_IMAGE_IMPL=1
    -DGLM_ENABLE_EXPERIMENTAL=1)

