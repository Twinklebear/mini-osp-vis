#include "load_off.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <ospray/ospray.h>
#include <ospray/ospray_cpp.h>
#include <ospray/ospray_cpp/ext/rkcommon.h>
#include <tbb/parallel_for.h>
#include "loader.h"
#include "util.h"

VolumeBrick load_off(const std::string &file_name)
{
    VolumeBrick volume_data;
    std::ifstream fin(file_name.c_str());
    // First line is:
    // n_verts n_tets
    size_t n_verts = 0;
    size_t n_tets = 0;
    fin >> n_verts >> n_tets;
    std::cout << "OFF File " << file_name << " has " << n_verts << " verts, " << n_tets
              << " tets\n";

    std::vector<math::vec3f> vertex_positions;
    std::vector<uint64_t> vertex_indices;
    std::vector<uint64_t> cell_offsets;
    std::vector<float> cell_scalars;
    std::vector<uint8_t> cell_types;
    std::vector<float> vertex_scalars;

    vertex_positions.reserve(n_verts);
    vertex_scalars.reserve(n_verts);
    volume_data.value_range = math::vec2f(std::numeric_limits<float>::infinity(),
                                          -std::numeric_limits<float>::infinity());
    for (size_t i = 0; i < n_verts; ++i) {
        math::vec3f p;
        float value;
        fin >> p.x >> p.y >> p.z >> value;
        vertex_positions.push_back(p);
        vertex_scalars.push_back(value);

        volume_data.value_range.x = std::min(volume_data.value_range.x, value);
        volume_data.value_range.y = std::max(volume_data.value_range.y, value);

        volume_data.bounds.extend(p);
    }

    cell_types.resize(n_tets, OSP_TETRAHEDRON);
    cell_offsets.reserve(n_tets);
    vertex_indices.reserve(n_tets);
    uint64_t cell_offset = 0;
    for (size_t i = 0; i < n_tets; ++i) {
        uint64_t a, b, c, d;
        fin >> a >> b >> c >> d;
        // check and fix tet ordering
        math::vec3f verts[4];
        verts[0] = vertex_positions[a];
        verts[1] = vertex_positions[b];
        verts[2] = vertex_positions[c];
        verts[3] = vertex_positions[d];
        math::vec3f cross_prod = cross(verts[1] - verts[0], verts[2] - verts[0]);
        math::vec3f norm_v = verts[3] - verts[0];
        if (dot(cross_prod, norm_v) < 0.f) {
            uint32_t temp = b;
            b = c;
            c = temp;
        }
        vertex_indices.push_back(a);
        vertex_indices.push_back(b);
        vertex_indices.push_back(c);
        vertex_indices.push_back(d);
        cell_offsets.push_back(cell_offset);
        cell_offset += 4;
    }

    volume_data.brick = cpp::Volume("unstructured");
    volume_data.brick.setParam("vertex.position", cpp::CopiedData(vertex_positions));
    volume_data.brick.setParam("index", cpp::CopiedData(vertex_indices));
    volume_data.brick.setParam("cell.index", cpp::CopiedData(cell_offsets));
    volume_data.brick.setParam("cell.type", cpp::CopiedData(cell_types));
    volume_data.brick.setParam("vertex.data", cpp::CopiedData(vertex_scalars));
    volume_data.brick.commit();
    volume_data.model = cpp::VolumetricModel(volume_data.brick);

    return volume_data;
}
