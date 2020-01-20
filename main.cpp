#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <limits>
#include <thread>
#include <SDL.h>
#include <ospray/ospray.h>
#include <ospray/ospray_cpp.h>
#include "arcball_camera.h"
#include "gl_core_4_5.h"
#include "imgui/imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl.h"
#include "loader.h"
#include "stb_image_write.h"
#include "util/arcball_camera.h"
#include "util/json.hpp"
#include "util/shader.h"
#include "util/transfer_function_widget.h"
#include "util/util.h"

using namespace ospray;
using namespace ospcommon;
using json = nlohmann::json;

const std::string fullscreen_quad_vs = R"(
#version 330 core

const vec4 pos[4] = vec4[4](
	vec4(-1, 1, 0.5, 1),
	vec4(-1, -1, 0.5, 1),
	vec4(1, 1, 0.5, 1),
	vec4(1, -1, 0.5, 1)
);

void main(void){
	gl_Position = pos[gl_VertexID];
}
)";

const std::string display_texture_fs = R"(
#version 330 core

uniform sampler2D img;

out vec4 color;

void main(void){ 
	ivec2 uv = ivec2(gl_FragCoord.xy);
	color = texelFetch(img, uv, 0);
})";

const std::string USAGE =
#ifdef OPENVISUS_FOUND
    "./mini_scivis <volume.json/idx> [options]\n"
#else
    "./mini_scivis <volume.json> [options]\n"
#endif
    "Options:\n"
    "  -iso <val>               Render an isosurface at the specified value\n"
    "  -vr <lo> <hi>            Provide the value range for the volume to skip computing it\n"
    "  -r (scivis|pathtracer)   Select the OSPRay renderer to use\n"
    "  -h                       Print this help.";

int win_width = 1280;
int win_height = 720;

glm::vec2 transform_mouse(glm::vec2 in)
{
    return glm::vec2(in.x * 2.f / win_width - 1.f, 1.f - 2.f * in.y / win_height);
}

void run_app(const std::vector<std::string> &args, SDL_Window *window);

int main(int argc, const char **argv)
{
    if (argc < 2) {
        std::cout << "[error]: A volume config JSON file is required. Fetch one from "
                     "OpenScivisDatasets using the provided script\n";
        std::cout << USAGE << "\n";
        return 1;
    }

    if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
        std::cerr << "Failed to init SDL: " << SDL_GetError() << "\n";
        return -1;
    }

    const char *glsl_version = "#version 330 core";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window *window = SDL_CreateWindow("OSPRay Mini-Scivis",
                                          SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED,
                                          win_width,
                                          win_height,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_SetSwapInterval(1);
    SDL_GL_MakeCurrent(window, gl_context);

    if (ogl_LoadFunctions() == ogl_LOAD_FAILED) {
        std::cerr << "Failed to initialize OpenGL\n";
        return 1;
    }

    ospInit(&argc, argv);
    // set an error callback to catch any OSPRay errors and exit the application
    ospDeviceSetErrorFunc(ospGetCurrentDevice(), [](OSPError error, const char *msg) {
        std::cerr << "[OSPRay error]: " << msg << std::endl << std::flush;
        throw std::runtime_error(msg);
    });

    // Setup Dear ImGui context
    ImGui::CreateContext();

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer bindings
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    run_app(std::vector<std::string>(argv, argv + argc), window);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    ospShutdown();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

void run_app(const std::vector<std::string> &args, SDL_Window *window)
{
    json config;
    std::string idx_file;
    math::vec2f value_range(std::numeric_limits<float>::infinity());
    float isovalue = std::numeric_limits<float>::infinity();
    std::string renderer_type = "scivis";
    std::vector<VolumeBrick> bricks;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-vr") {
            value_range.x = std::stof(args[++i]);
            value_range.y = std::stof(args[++i]);
        } else if (args[i] == "-iso") {
            isovalue = std::stof(args[++i]);
        } else if (args[i] == "-r") {
            renderer_type = args[++i];
        } else if (args[i] == "-h") {
            std::cout << USAGE << "\n";
            return;
        } else {
            if (get_file_extension(args[i]) == "json") {
                std::ifstream cfg_file(args[i].c_str());
                cfg_file >> config;

                std::string base_path = get_file_basepath(args[i]);
                if (base_path == args[i]) {
                    base_path = ".";
                }
                const std::string base_name = get_file_basename(config["url"]);
                config["volume"] = base_path + "/" + base_name;
                bricks.push_back(load_raw_volume(config));
            } else if (get_file_extension(args[i]) == "size") {
                config["type"] = "float64";
                bricks.push_back(load_dns_brick(args[i]));
            } else {
#ifdef OPENVISUS_FOUND
                config = json();
                bricks.push_back(load_idx_volume(args[i], config));
#else
                std::cerr << "[error]: Requested to load non-JSON file data " << args[i]
                          << ", but OpenVisus was not found\n";
                std::exit(1);
#endif
            }
        }
    }
    std::cout << config.dump(4) << "\n";

    if (!std::isfinite(value_range.x) || !std::isfinite(value_range.y)) {
        std::cout << "Computing value range\n";
        const std::string voxel_type = config["type"].get<std::string>();
        value_range.x = std::numeric_limits<float>::infinity();
        value_range.y = -std::numeric_limits<float>::infinity();
        for (const auto &b : bricks) {
            math::vec2f brick_vr;
            if (voxel_type == "uint8") {
                brick_vr = compute_value_range(b.voxel_data->data(), b.voxel_data->size());
            } else if (voxel_type == "uint16") {
                brick_vr =
                    compute_value_range(reinterpret_cast<uint16_t *>(b.voxel_data->data()),
                                        b.voxel_data->size() / sizeof(uint16_t));
            } else if (voxel_type == "float32") {
                brick_vr = compute_value_range(reinterpret_cast<float *>(b.voxel_data->data()),
                                               b.voxel_data->size() / sizeof(float));
            } else if (voxel_type == "float64") {
                brick_vr =
                    compute_value_range(reinterpret_cast<double *>(b.voxel_data->data()),
                                        b.voxel_data->size() / sizeof(double));
            }
            value_range.x = std::min(value_range.x, brick_vr.x);
            value_range.y = std::max(value_range.y, brick_vr.y);
        }
        std::cout << "Computed value range: " << value_range << "\n";
    }

    math::box3f world_bounds = bricks[0].bounds;
    for (const auto &b : bricks) {
        world_bounds.extend(b.bounds);
    }

    const float world_diagonal = math::length(world_bounds.size());
    const math::vec3f world_center = world_bounds.center();
    ArcballCamera arcball(
        glm::vec3(world_center.x, world_center.y, world_center.z - world_diagonal * 1.5),
        glm::vec3(world_center.x, world_center.y, world_center.z),
        glm::vec3(0, 1, 0));

    TransferFunctionWidget tfn_widget;
    std::vector<float> tfn_colors;
    std::vector<float> tfn_opacities;
    tfn_widget.get_colormapf(tfn_colors, tfn_opacities);

    cpp::TransferFunction tfn("piecewiseLinear");
    tfn.setParam(
        "color",
        cpp::Data(
            tfn_colors.size() / 3, reinterpret_cast<math::vec3f *>(tfn_colors.data()), true));
    tfn.setParam("opacity", cpp::Data(tfn_opacities.size(), tfn_opacities.data(), true));
    tfn.setParam("valueRange", value_range);
    tfn.commit();

    cpp::Renderer renderer(renderer_type);
    float sampling_rate = 1.f;
    renderer.setParam("volumeSamplingRate", sampling_rate);
    renderer.commit();

    for (auto &b : bricks) {
        b.model.setParam("transferFunction", tfn);
        b.model.commit();
    }

    std::vector<cpp::Group> groups;
    for (auto &b : bricks) {
        cpp::Group g;
        g.setParam("volume", cpp::Data(b.model));
        groups.push_back(g);
    }

    cpp::Material iso_material(renderer_type, "obj");
    iso_material.setParam("Kd", math::vec3f(0.9f, 0.9f, 0.9f));
    iso_material.commit();

    if (std::isfinite(isovalue)) {
        for (size_t i = 0; i < bricks.size(); ++i) {
            auto geom = extract_isosurfaces(config, bricks[i], isovalue);
            if (geom) {
                cpp::GeometricModel geom_model;
                geom_model = cpp::GeometricModel(geom);
                geom_model.setParam("material", iso_material);
                geom_model.commit();
                groups[i].setParam("geometry", cpp::Data(geom_model));
            }
        }
    }
    for (auto &g : groups) {
        g.commit();
    }

    std::vector<cpp::Instance> instances;
    for (size_t i = 0; i < groups.size(); ++i) {
        cpp::Instance instance(groups[i]);
        auto transform = math::affine3f::translate(bricks[i].bounds.lower);
        instance.setParam("xfm", transform);
        instance.commit();
        instances.push_back(instance);
    }

    // create and setup an ambient light
    cpp::Light ambient_light("ambient");
    ambient_light.commit();

    cpp::Light directional_light("distant");
    directional_light.setParam("direction", math::vec3f(0.5f, -1.f, 0.25f));
    directional_light.commit();
    std::vector<cpp::Light> lights = {ambient_light, directional_light};

    cpp::World world;
    world.setParam("instance", cpp::Data(instances));
    world.setParam("light", cpp::Data(lights));
    world.commit();

    glm::vec3 cam_eye = arcball.eye();
    glm::vec3 cam_dir = arcball.dir();
    glm::vec3 cam_up = arcball.up();

    cpp::Camera camera("perspective");
    camera.setParam("aspect", static_cast<float>(win_width) / win_height);
    camera.setParam("position", math::vec3f(cam_eye.x, cam_eye.y, cam_eye.z));
    camera.setParam("direction", math::vec3f(cam_dir.x, cam_dir.y, cam_dir.z));
    camera.setParam("up", math::vec3f(cam_up.x, cam_up.y, cam_up.z));
    camera.setParam("fovy", 40.f);
    camera.commit();

    cpp::FrameBuffer fb(
        math::vec2i(win_width, win_height), OSP_FB_SRGBA, OSP_FB_COLOR | OSP_FB_ACCUM);
    fb.clear();

    Shader display_render(fullscreen_quad_vs, display_texture_fs);
    display_render.uniform("img", 0);

    GLuint render_texture;
    glGenTextures(1, &render_texture);
    glBindTexture(GL_TEXTURE_2D, render_texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, win_width, win_height);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLuint vao;
    glCreateVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glClearColor(0.0, 0.0, 0.0, 0.0);
    glDisable(GL_DEPTH_TEST);

    // Start rendering asynchronously
    cpp::Future future = fb.renderFrame(renderer, camera, world);
    std::vector<OSPObject> pending_commits;

    ImGuiIO &io = ImGui::GetIO();
    glm::vec2 prev_mouse(-2.f);
    bool done = false;
    bool camera_changed = true;
    bool window_changed = false;
    bool take_screenshot = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                done = true;
            }
            if (!io.WantCaptureKeyboard && event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    done = true;
                } else if (event.key.keysym.sym == SDLK_p) {
                    auto eye = arcball.eye();
                    auto dir = arcball.dir();
                    auto up = arcball.up();
                    std::cout << "-eye " << eye.x << " " << eye.y << " " << eye.z << " -dir "
                              << dir.x << " " << dir.y << " " << dir.z << " -up " << up.x
                              << " " << up.y << " " << up.z << "\n";
                } else if (event.key.keysym.sym == SDLK_c) {
                    take_screenshot = true;
                }
            }
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                done = true;
            }
            if (!io.WantCaptureMouse) {
                if (event.type == SDL_MOUSEMOTION) {
                    const glm::vec2 cur_mouse =
                        transform_mouse(glm::vec2(event.motion.x, event.motion.y));
                    if (prev_mouse != glm::vec2(-2.f)) {
                        if (event.motion.state & SDL_BUTTON_LMASK) {
                            arcball.rotate(prev_mouse, cur_mouse);
                            camera_changed = true;
                        } else if (event.motion.state & SDL_BUTTON_RMASK) {
                            arcball.pan(cur_mouse - prev_mouse);
                            camera_changed = true;
                        }
                    }
                    prev_mouse = cur_mouse;
                } else if (event.type == SDL_MOUSEWHEEL) {
                    arcball.zoom(event.wheel.y * world_diagonal / 100.f);
                    camera_changed = true;
                }
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_RESIZED) {
                window_changed = true;
                win_width = event.window.data1;
                win_height = event.window.data2;
                io.DisplaySize.x = win_width;
                io.DisplaySize.y = win_height;

                camera.setParam("aspect", static_cast<float>(win_width) / win_height);
                pending_commits.push_back(camera.handle());

                // make new framebuffer
                fb = cpp::FrameBuffer(math::vec2i(win_width, win_height),
                                      OSP_FB_SRGBA,
                                      OSP_FB_COLOR | OSP_FB_ACCUM);
                fb.clear();

                glDeleteTextures(1, &render_texture);
                glGenTextures(1, &render_texture);
                // Setup the render textures for color and normals
                glBindTexture(GL_TEXTURE_2D, render_texture);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, win_width, win_height);

                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
        }

        if (camera_changed) {
            cam_eye = arcball.eye();
            cam_dir = arcball.dir();
            cam_up = arcball.up();

            camera.setParam("position", math::vec3f(cam_eye.x, cam_eye.y, cam_eye.z));
            camera.setParam("direction", math::vec3f(cam_dir.x, cam_dir.y, cam_dir.z));
            camera.setParam("up", math::vec3f(cam_up.x, cam_up.y, cam_up.z));
            pending_commits.push_back(camera.handle());
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame(window);
        ImGui::NewFrame();

        if (ImGui::Begin("Params")) {
            static float density_scale = 1.f;
            if (ImGui::SliderFloat("Density Scale", &density_scale, 0.5f, 10.f)) {
                for (auto &b : bricks) {
                    b.model.setParam("densityScale", density_scale);
                    pending_commits.push_back(b.model.handle());
                }
            }
            if (ImGui::SliderFloat("Sampling Rate", &sampling_rate, 0.1f, 5.f)) {
                renderer.setParam("volumeSamplingRate", sampling_rate);
                pending_commits.push_back(renderer.handle());
            }
            ImGui::End();
        }

        if (ImGui::Begin("Transfer Function")) {
            if (ImGui::Button("Save Transfer Function")) {
                auto tfn_img = tfn_widget.get_colormap();
                stbi_write_png("transfer_function.png",
                               tfn_img.size() / 4,
                               1,
                               4,
                               tfn_img.data(),
                               tfn_img.size());
                std::cout << "Transfer function saved to 'transfer_function.png'\n";
            }
            tfn_widget.draw_ui();
            ImGui::End();
        }

        // Rendering
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);

        if (future.isReady()) {
            if (!window_changed) {
                uint32_t *img = (uint32_t *)fb.map(OSP_FB_COLOR);
                glTexSubImage2D(GL_TEXTURE_2D,
                                0,
                                0,
                                0,
                                win_width,
                                win_height,
                                GL_RGBA,
                                GL_UNSIGNED_BYTE,
                                img);
                if (take_screenshot) {
                    take_screenshot = false;
                    stbi_flip_vertically_on_write(1);
                    stbi_write_jpg("mini_scivis.jpg", win_width, win_height, 4, img, 90);
                    std::cout << "Screenshot saved to 'mini_scivis.jpg'\n";
                    stbi_flip_vertically_on_write(0);
                }
                fb.unmap(img);
            }
            window_changed = false;

            if (tfn_widget.changed()) {
                tfn_widget.get_colormapf(tfn_colors, tfn_opacities);
                tfn.setParam("color",
                             cpp::Data(tfn_colors.size() / 3,
                                       reinterpret_cast<math::vec3f *>(tfn_colors.data()),
                                       true));
                tfn.setParam("opacity",
                             cpp::Data(tfn_opacities.size(), tfn_opacities.data(), true));
                tfn.setParam("valueRange", value_range);
                pending_commits.push_back(tfn.handle());
                for (auto &b : bricks) {
                    pending_commits.push_back(b.model.handle());
                }
            }

            if (!pending_commits.empty()) {
                fb.clear();
            }
            for (auto &c : pending_commits) {
                ospCommit(c);
            }
            pending_commits.clear();

            future = fb.renderFrame(renderer, camera, world);
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(display_render.program);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);

        camera_changed = false;
    }
}

