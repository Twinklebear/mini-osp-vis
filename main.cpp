#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <limits>
#include <thread>
#include <ospray/ospray.h>
#include <ospray/ospray_cpp.h>
#include "arcball_camera.h"
#include "loader.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include "util/arcball_camera.h"
#include "util/json.hpp"
#include "util/util.h"

using namespace ospray;
using namespace rkcommon;
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
    "  -tfn [ignore_opacity] <tfcn.png/jpg>\n"
    "                           Load the saved RGBA transfer function from the provided "
    "image\n"
    "                           file. If you optionally set ignore_opacity as the first arg\n"
    "                           the opacity in the file will not be used\n"
    "\n"
    "  -bg <r> <g> <b>          Set the desired background color (default white)\n"
    "\n"
    "  -iso-color <r> <g> <b>   Set the desired isosurface color (default light gray)\n"
    "\n"
    "  -iso-opacity <x>         Set the desired isosurface opacity (default opaque)\n"
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

struct ClippingPlane {
    int axis = 0;
    bool flip_plane = false;
    bool enabled = false;
    math::vec3f position;

    cpp::Geometry geom;
    cpp::GeometricModel model;
    cpp::Group group;
    cpp::Instance instance;

    ClippingPlane(int axis = 0, const math::vec3f &pos = math::vec3f(0.f))
        : axis(axis), position(pos), geom("plane")
    {
        math::vec4f normal(0.f);
        normal[axis] = 1.f;
        geom.setParam("plane.coefficients", cpp::CopiedData(normal));
        geom.commit();

        model = cpp::GeometricModel(geom);
        model.commit();

        group.setParam("clippingGeometry", cpp::CopiedData(model));
        group.commit();

        instance = cpp::Instance(group);
        const math::affine3f xfm =
            math::affine3f::translate(math::vec3f(position.x, position.y, position.z));
        instance.setParam("xfm", xfm);
        instance.commit();
    }

    void flip_direction(bool flip_dir, std::vector<OSPObject> &pending_commits)
    {
        flip_plane = flip_dir;
        model.setParam("invertNormals", flip_plane);
        pending_commits.push_back(model.handle());
        pending_commits.push_back(group.handle());
        pending_commits.push_back(instance.handle());
    }

    void set_position(const float &pos, std::vector<OSPObject> &pending_commits)
    {
        position[axis] = pos;
        const math::affine3f xfm =
            math::affine3f::translate(math::vec3f(position.x, position.y, position.z));
        instance.setParam("xfm", xfm);
        pending_commits.push_back(instance.handle());
    }
};

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

void run_app(const std::vector<std::string> &args);

int main(int argc, const char **argv)
{
    if (argc < 2) {
        std::cout << "[error]: A volume config JSON file is required. Fetch one from "
                     "OpenScivisDatasets using the provided script\n";
        std::cout << USAGE << "\n";
        return 1;
    }

    OSPError init_err = ospInit(&argc, argv);
    if (init_err != OSP_NO_ERROR) {
        throw std::runtime_error("Failed to initialize OSPRay");
    }

    OSPDevice device = ospGetCurrentDevice();
    if (!device) {
        throw std::runtime_error("OSPRay device could not be fetched!");
    }
    ospDeviceSetErrorCallback(
        device,
        [](void *, OSPError, const char *errorDetails) {
            std::cerr << "OSPRay error: " << errorDetails << std::endl;
            throw std::runtime_error(errorDetails);
        },
        nullptr);
    ospDeviceSetStatusCallback(
        device, [](void *, const char *msg) { std::cout << msg; }, nullptr);

    bool warnAsErrors = true;
    auto logLevel = OSP_LOG_WARNING;

    ospDeviceSetParam(device, "warnAsError", OSP_BOOL, &warnAsErrors);
    ospDeviceSetParam(device, "logLevel", OSP_INT, &logLevel);

    ospDeviceCommit(device);
    ospDeviceRelease(device);

    run_app(std::vector<std::string>(argv, argv + argc));

    ospShutdown();

    return 0;
}

void run_app(const std::vector<std::string> &args)
{
    json config;
    std::string idx_file;
    math::vec2f value_range(std::numeric_limits<float>::infinity());
    std::vector<float> isovalues;
    std::string renderer_type = "scivis";
    VolumeBrick brick;
    bool cmdline_camera = false;
    glm::vec3 cam_eye;
    glm::vec3 cam_at;
    glm::vec3 cam_up;
    math::vec3f background_color(1.f);
    std::vector<math::vec4f> isosurface_colors;
    float isosurface_opacity = 1.f;
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
            isovalues.push_back(std::stof(args[++i]));
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
        } else if (args[i] == "-bg") {
            background_color.x = std::stof(args[++i]);
            background_color.y = std::stof(args[++i]);
            background_color.z = std::stof(args[++i]);
        } else if (args[i] == "-iso-color") {
            math::vec4f c(1.f);
            c.x = std::stof(args[++i]);
            c.y = std::stof(args[++i]);
            c.z = std::stof(args[++i]);
            isosurface_colors.push_back(c);
        } else if (args[i] == "-iso-opacity") {
            isosurface_opacity = std::stof(args[++i]);
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

    const math::vec3f world_center = brick.bounds.center();
    const math::box3f world_bounds = brick.bounds;
    const float world_diagonal = math::length(brick.bounds.size());
    if (!cmdline_camera) {
        cam_eye =
            glm::vec3(world_center.x, world_center.y, world_center.z - world_diagonal * 1.5);
        cam_at = glm::vec3(world_center.x, world_center.y, world_center.z);
        cam_up = glm::vec3(0.f, 1.f, 0.f);
    }
    ArcballCamera arcball(cam_eye, cam_at, cam_up);

    cpp::Renderer renderer(renderer_type);
    float sampling_rate = 1.f;
    renderer.setParam("volumeSamplingRate", sampling_rate);
    renderer.setParam("backgroundColor", background_color);
    renderer.commit();

    cpp::Group group;
    if (!isovalues.empty()) {
        cpp::Material material(renderer_type, "obj");
        material.setParam("kd", math::vec3f(1.f));
        material.setParam("d", isosurface_opacity);
        material.commit();

        auto geom = extract_isosurfaces(config, brick, isovalues);
        std::vector<cpp::GeometricModel> geom_models;
        // If using VTK for multiple isosurfaces we'll get a bunch of triangle meshes, one
        // per-isovalue
        for (size_t i = 0; i < geom.size(); ++i) {
            const auto &g = geom[i];
            cpp::GeometricModel geom_model;
            geom_model = cpp::GeometricModel(g);
            geom_model.setParam("material", material);
            if (!isosurface_colors.empty()) {
                if (geom.size() > 1) {
                    geom_model.setParam("color", cpp::CopiedData(isosurface_colors[i]));
                } else {
                    geom_model.setParam("color", cpp::CopiedData(isosurface_colors));
                }
            }
            geom_model.commit();
            geom_models.push_back(geom_model);
        }
        if (!geom_models.empty()) {
            group.setParam("geometry", cpp::CopiedData(geom_models));
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

    std::array<ClippingPlane, 3> clipping_planes = {ClippingPlane(0, world_center),
                                                    ClippingPlane(1, world_center),
                                                    ClippingPlane(2, world_center)};

    cpp::World world;
    world.setParam("instance", cpp::CopiedData(instance));
    world.setParam("light", cpp::CopiedData(lights));
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

    cpp::FrameBuffer fb(win_width, win_height, OSP_FB_SRGBA, OSP_FB_COLOR | OSP_FB_ACCUM);
    fb.clear();

    // Start rendering asynchronously
    cpp::Future future = fb.renderFrame(renderer, camera, world);
    std::vector<OSPObject> pending_commits;

    future.wait();
    uint32_t *img = (uint32_t*)fb.map(OSP_FB_COLOR);
    stbi_flip_vertically_on_write(1);
    stbi_write_jpg(output_image_file.c_str(), win_width, win_height, 4, img, 90);
    std::cout << "Screenshot saved to '" << output_image_file << "'" << std::endl;
    stbi_flip_vertically_on_write(0);
    fb.unmap(img);
}

