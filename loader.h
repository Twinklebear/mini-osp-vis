#pragma once

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <vector>
#include <ospray/ospray.h>
#include <ospray/ospray_cpp.h>
#include <ospray/ospray_cpp/ext/rkcommon.h>
#include <rkcommon/math/box.h>
#include <rkcommon/math/vec.h>
#include "json.hpp"
#include "load_off.h"
#include "volume_data.h"
#include <glm/glm.hpp>

using namespace ospray;
using namespace rkcommon;
using json = nlohmann::json;

VolumeBrick load_raw_volume(const json &config);

VolumeBrick load_idx_volume(const std::string &idx_file, json &config);

std::vector<cpp::Geometry> extract_isosurfaces(const json &config,
                                               const VolumeBrick &brick,
                                               const std::vector<float> &isovalues);
