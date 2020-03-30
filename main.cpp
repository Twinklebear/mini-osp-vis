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
#include "stb_image.h"
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
    "\n"
    "  -vr <lo> <hi>            Provide the value range for the volume to skip computing it\n"
    "\n"
    "  -r (scivis|pathtracer)   Select the OSPRay renderer to use\n"
    "\n"
    "  -camera <eye_x> <eye_y> <eye_z> <at_x> <at_y> <at_z> <up_x> <up_y> <up_z>\n"
    "                           Specify the camera position, orbit center and up vector\n"
    "\n"
    "  -tfn <tfcn.png/jpg>      Load the saved RGBA transfer function from the provided image "
    "file\n"
    "\n"
    "  -bg <r> <g> <b>          Set the desired background color (default white)\n"
    "\n"
    "  -iso-color <r> <g> <b>   Set the desired isosurface color (default light gray)\n"
    "\n"
    "  -ambient <intensity>     Set the ambient light intensity\n"
    "\n"
    "  -dir1 <intensity> <x> <y> <z>\n"
    "                           Set the first directional light intensity and direction\n"
    "\n"
    "  -dir2 <intensity> <x> <y> <z>\n"
    "                           Set the second directional light intensity and direction\n"
    "\n"
    "  -density-scale <x>       Set the volume density scaling\n"
    "\n"
    "  -nf <n>                  Set the number of frames to render before saving the image "
    "and exiting\n"
    "\n"
    "  -o <name.jpg>            Set the output image filename\n"
    "\n"
    "  -h                       Print this help.";

int win_width = 1280;
int win_height = 720;

struct LightParams {
    float intensity = 0.5f;
    math::vec3f direction = math::vec3f(0.f);

    LightParams() = default;
    LightParams(float intensity) : intensity(intensity) {}
    LightParams(float intensity, const math::vec3f &dir) : intensity(intensity), direction(dir)
    {
    }
};

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
    VolumeBrick brick;
    bool cmdline_camera = false;
    glm::vec3 cam_eye;
    glm::vec3 cam_at;
    glm::vec3 cam_up;
    math::vec3f background_color(1.f);
    math::vec3f isosurface_color(0.9f);
    std::vector<Colormap> cmdline_colormaps;
    std::array<LightParams, 3> light_params = {
        LightParams(0.3f),
        LightParams(1.f, math::vec3f(0.5f, -1.f, 0.25f)),
        LightParams(1.f, math::vec3f(-0.5f, -0.5f, 0.5f))};

    int render_frame_count = -1;
    std::string output_image_file = "mini_scivis.jpg";
    float density_scale = 1.f;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-vr") {
            value_range.x = std::stof(args[++i]);
            value_range.y = std::stof(args[++i]);
        } else if (args[i] == "-iso") {
            isovalue = std::stof(args[++i]);
        } else if (args[i] == "-r") {
            renderer_type = args[++i];
        } else if (args[i] == "-camera") {
            cmdline_camera = true;
            cam_eye.x = std::stof(args[++i]);
            cam_eye.y = std::stof(args[++i]);
            cam_eye.z = std::stof(args[++i]);

            cam_at.x = std::stof(args[++i]);
            cam_at.y = std::stof(args[++i]);
            cam_at.z = std::stof(args[++i]);

            cam_up.x = std::stof(args[++i]);
            cam_up.y = std::stof(args[++i]);
            cam_up.z = std::stof(args[++i]);
        } else if (args[i] == "-tfn") {
            const std::string tfn_file = args[++i];
            const std::string tfn_name = get_file_basename(tfn_file);
            int x, y, n;
            uint8_t *data = stbi_load(tfn_file.c_str(), &x, &y, &n, 4);
            std::vector<uint8_t> img_data(data, data + x * 4);
            stbi_image_free(data);
            cmdline_colormaps.emplace_back(tfn_name, img_data, LINEAR, true);
        } else if (args[i] == "-bg") {
            background_color.x = std::stof(args[++i]);
            background_color.y = std::stof(args[++i]);
            background_color.z = std::stof(args[++i]);
        } else if (args[i] == "-iso-color") {
            isosurface_color.x = std::stof(args[++i]);
            isosurface_color.y = std::stof(args[++i]);
            isosurface_color.z = std::stof(args[++i]);
        } else if (args[i] == "-ambient") {
            light_params[0].intensity = std::stof(args[++i]);
        } else if (args[i] == "-dir1") {
            light_params[1].intensity = std::stof(args[++i]);
            light_params[1].direction.x = std::stof(args[++i]);
            light_params[1].direction.y = std::stof(args[++i]);
            light_params[1].direction.z = std::stof(args[++i]);
        } else if (args[i] == "-dir2") {
            light_params[2].intensity = std::stof(args[++i]);
            light_params[2].direction.x = std::stof(args[++i]);
            light_params[2].direction.y = std::stof(args[++i]);
            light_params[2].direction.z = std::stof(args[++i]);
        } else if (args[i] == "-density-scale") {
            density_scale = std::stof(args[++i]);
        } else if (args[i] == "-nf") {
            render_frame_count = std::stoi(args[++i]);
        } else if (args[i] == "-o") {
            output_image_file = args[++i];
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
                brick = load_raw_volume(config);
            } else {
#ifdef OPENVISUS_FOUND
                config = json();
                brick = load_idx_volume(args[i], config);
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
        if (voxel_type == "uint8") {
            value_range =
                compute_value_range(brick.voxel_data->data(), brick.voxel_data->size());
        } else if (voxel_type == "uint16") {
            value_range =
                compute_value_range(reinterpret_cast<uint16_t *>(brick.voxel_data->data()),
                                    brick.voxel_data->size() / sizeof(uint16_t));
        } else if (voxel_type == "float32") {
            value_range =
                compute_value_range(reinterpret_cast<float *>(brick.voxel_data->data()),
                                    brick.voxel_data->size() / sizeof(float));
        } else if (voxel_type == "float64") {
            value_range =
                compute_value_range(reinterpret_cast<double *>(brick.voxel_data->data()),
                                    brick.voxel_data->size() / sizeof(double));
        }
        std::cout << "Computed value range: " << value_range << "\n";
    }
    math::vec2f ui_value_range = value_range;

    const float world_diagonal = math::length(brick.bounds.size());
    if (!cmdline_camera) {
        const math::vec3f world_center = brick.bounds.center();
        cam_eye =
            glm::vec3(world_center.x, world_center.y, world_center.z - world_diagonal * 1.5);
        cam_at = glm::vec3(world_center.x, world_center.y, world_center.z);
        cam_up = glm::vec3(0.f, 1.f, 0.f);
    }
    ArcballCamera arcball(cam_eye, cam_at, cam_up);

    TransferFunctionWidget tfn_widget;
    for (const auto &cmap : cmdline_colormaps) {
        tfn_widget.add_colormap(cmap);
    }
    std::vector<float> tfn_colors;
    std::vector<float> tfn_opacities;
    tfn_widget.get_colormapf(tfn_colors, tfn_opacities);

    cpp::TransferFunction tfn("piecewiseLinear");
    tfn.setParam(
        "color",
        cpp::Data(
            tfn_colors.size() / 3, reinterpret_cast<math::vec3f *>(tfn_colors.data()), true));
    tfn.setParam("opacity", cpp::Data(tfn_opacities.size(), tfn_opacities.data(), true));
    tfn.setParam("valueRange", ui_value_range);
    tfn.commit();

    cpp::Renderer renderer(renderer_type);
    float sampling_rate = 1.f;
    renderer.setParam("volumeSamplingRate", sampling_rate);
    renderer.setParam("backgroundColor", background_color);
    renderer.commit();

    brick.model.setParam("densityScale", density_scale);
    brick.model.setParam("transferFunction", tfn);
    brick.model.commit();

    cpp::Group group;
    group.setParam("volume", cpp::Data(brick.model));

    if (std::isfinite(isovalue)) {
        std::cout << "isovalue: " << isovalue << "\n";
        auto geom = extract_isosurfaces(config, brick, isovalue);
        if (geom) {
            cpp::Material material(renderer_type, "obj");
            material.setParam("kd", isosurface_color);
            material.commit();

            cpp::GeometricModel geom_model;
            geom_model = cpp::GeometricModel(geom);
            geom_model.setParam("material", material);
            geom_model.commit();
            group.setParam("geometry", cpp::Data(geom_model));
        }
    }
    group.commit();

    cpp::Instance instance(group);
    instance.commit();

    std::vector<cpp::Light> lights;
    // create and setup an ambient light
    {
        cpp::Light light("ambient");
        light.setParam("intensity", light_params[0].intensity);
        light.commit();
        lights.push_back(light);
    }
    {
        cpp::Light light("distant");
        light.setParam("intensity", light_params[1].intensity);
        light.setParam("direction", light_params[1].direction);
        light.commit();
        lights.push_back(light);
    }
    {
        cpp::Light light("distant");
        light.setParam("intensity", light_params[2].intensity);
        light.setParam("direction", light_params[2].direction);
        light.commit();
        lights.push_back(light);
    }

    cpp::World world;
    world.setParam("instance", cpp::Data(instance));
    world.setParam("light", cpp::Data(lights));
    world.commit();

    cam_eye = arcball.eye();
    glm::vec3 cam_dir = arcball.dir();
    cam_up = arcball.up();

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

    int frame_id = 0;
    ImGuiIO &io = ImGui::GetIO();
    glm::vec2 prev_mouse(-2.f);
    bool done = false;
    bool camera_changed = true;
    bool window_changed = false;
    bool take_screenshot = false;
    bool lights_changed = false;
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
                    std::cout << "-camera " << eye.x << " " << eye.y << " " << eye.z << " "
                              << eye.x + dir.x << " " << eye.y + dir.y << " " << eye.z + dir.z
                              << " " << up.x << " " << up.y << " " << up.z << "\n";
                } else if (event.key.keysym.sym == SDLK_c) {
                    take_screenshot = true;
                } else if (event.key.keysym.sym == SDLK_l) {
                    std::cout << "-ambient " << light_params[0].intensity << " -dir1 "
                              << light_params[1].intensity << " "
                              << light_params[1].direction.x << " "
                              << light_params[1].direction.y << " "
                              << light_params[1].direction.z << " -dir2 "
                              << light_params[2].intensity << " "
                              << light_params[2].direction.x << " "
                              << light_params[2].direction.y << " "
                              << light_params[2].direction.z << "\n";
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
            if (ImGui::SliderFloat("Density Scale", &density_scale, 0.5f, 10.f)) {
                brick.model.setParam("densityScale", density_scale);
                pending_commits.push_back(brick.model.handle());
            }
            if (ImGui::SliderFloat("Sampling Rate", &sampling_rate, 0.1f, 5.f)) {
                renderer.setParam("volumeSamplingRate", sampling_rate);
                pending_commits.push_back(renderer.handle());
            }
            if (ImGui::SliderFloat2(
                    "Value Range", &ui_value_range.x, value_range.x, value_range.y)) {
                tfn.setParam("valueRange", ui_value_range);
                pending_commits.push_back(tfn.handle());
                pending_commits.push_back(brick.model.handle());
            }

            for (size_t i = 0; i < lights.size(); ++i) {
                ImGui::PushID(i);
                ImGui::Separator();

                if (i == 0) {
                    ImGui::Text("Ambient Light");
                } else {
                    ImGui::Text("Directional Light");
                }

                if (ImGui::SliderFloat("Intensity", &light_params[i].intensity, 0.f, 10.f)) {
                    lights[i].setParam("intensity", light_params[i].intensity);
                    pending_commits.push_back(lights[i].handle());
                    lights_changed = true;
                }
                if (i != 0) {
                    if (ImGui::SliderFloat3(
                            "Direction", &light_params[i].direction.x, -1.f, 1.f)) {
                        lights[i].setParam("direction", light_params[i].direction);
                        pending_commits.push_back(lights[i].handle());
                        lights_changed = true;
                    }
                }
                ImGui::PopID();
            }
            if (lights_changed) {
                world.setParam("light", cpp::Data(lights));
                pending_commits.push_back(world.handle());
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

        if (render_frame_count != -1 && frame_id == render_frame_count) {
            take_screenshot = true;
            done = true;
        }

        if (future.isReady()) {
            ++frame_id;
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
                    stbi_write_jpg(
                        output_image_file.c_str(), win_width, win_height, 4, img, 90);
                    std::cout << "Screenshot saved to '" << output_image_file << "'"
                              << std::endl;
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
                tfn.setParam("valueRange", ui_value_range);
                pending_commits.push_back(tfn.handle());
                pending_commits.push_back(brick.model.handle());
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
        lights_changed = false;
    }
}

