#pragma once

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <vector>
#include <ospray/ospray.h>
#include <ospray/ospray_cpp.h>
#include "json.hpp"
#include <glm/glm.hpp>

using namespace ospray;
using namespace ospcommon;
using json = nlohmann::json;

struct VolumeBrick {
    cpp::Volume brick;
    math::box3f bounds;
    math::vec3i dims;
    std::shared_ptr<std::vector<uint8_t>> voxel_data;
};

VolumeBrick load_raw_volume(const json &config);

VolumeBrick load_idx_volume(const std::string &idx_file, json &config);

cpp::Geometry extract_isosurfaces(const json &config,
                                  const VolumeBrick &brick,
                                  float isovalue);

