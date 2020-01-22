add_library(util
    util.cpp
    arcball_camera.cpp
    shader.cpp
    gl_core_4_5.c
    transfer_function_widget.cpp)

set_target_properties(util PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON)

target_link_libraries(util PUBLIC
    ospray::ospray
    ospcommon::ospcommon
    TBB::tbb
    imgui
    glm::glm
    ${SDL2_LIBRARY}
    ${OPENGL_LIBRARY})

target_include_directories(util PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
	$<BUILD_INTERFACE:${SDL2_INCLUDE_DIR}>
	$<BUILD_INTERFACE:${OPENGL_INCLUDE_DIR}>)

target_compile_definitions(util PUBLIC
    -DTFN_WIDGET_NO_STB_IMAGE_IMPL=1
    -DGLM_ENABLE_EXPERIMENTAL=1)

