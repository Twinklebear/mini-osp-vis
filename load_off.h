#pragma once

#include <memory>
#include <vector>
#include <ospray/ospray.h>
#include <ospray/ospray_cpp.h>
#include <glm/glm.hpp>
#include "volume_data.h"

VolumeBrick load_off(const std::string &file_name);

