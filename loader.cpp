#include "loader.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <ospray/ospray.h>
#include <ospray/ospray_cpp.h>
#include "json.hpp"
#include "stb_image.h"
#include "util.h"

#ifdef USE_EXPLICIT_ISOSURFACE
#include <vtkDoubleArray.h>
#include <vtkFloatArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkSmartPointer.h>
#include <vtkTriangle.h>
#include <vtkUnsignedCharArray.h>
#include <vtkUnsignedShortArray.h>
#endif

#ifdef OPENVISUS_FOUND
#include <Visus/ApplicationInfo.h>
#include <Visus/IdxDataset.h>
#endif

VolumeBrick load_raw_volume(const json &config)
{
    VolumeBrick brick;

    const std::string volume_file = config["volume"].get<std::string>();
    const math::vec3f grid_spacing = get_vec<float, 3>(config["spacing"]);
    brick.dims = get_vec<int, 3>(config["size"]);
    brick.bounds = math::box3f(math::vec3f(0), brick.dims * grid_spacing);

    brick.brick = cpp::Volume("structuredRegular");
    brick.brick.setParam("dimensions", brick.dims);
    brick.brick.setParam("gridSpacing", grid_spacing);

    size_t voxel_size = 0;
    const std::string voxel_type_string = config["type"].get<std::string>();
    if (voxel_type_string == "uint8") {
        voxel_size = 1;
        brick.brick.setParam("voxelType", int(OSP_UCHAR));
    } else if (voxel_type_string == "uint16") {
        voxel_size = 2;
        brick.brick.setParam("voxelType", int(OSP_USHORT));
    } else if (voxel_type_string == "float32") {
        voxel_size = 4;
        brick.brick.setParam("voxelType", int(OSP_FLOAT));
    } else if (voxel_type_string == "float64") {
        voxel_size = 8;
        brick.brick.setParam("voxelType", int(OSP_DOUBLE));
    } else {
        throw std::runtime_error("Unrecognized voxel type " + voxel_type_string);
    }

    const size_t n_voxels = brick.dims.long_product();
    brick.voxel_data = std::make_shared<std::vector<uint8_t>>(n_voxels * voxel_size, 0);

    std::ifstream fin(volume_file.c_str(), std::ios::binary);
    if (!fin.read(reinterpret_cast<char *>(brick.voxel_data->data()),
                  brick.voxel_data->size())) {
        throw std::runtime_error("Failed to read volume " + volume_file);
    }

    cpp::SharedData osp_data;
    if (voxel_type_string == "uint8") {
        osp_data = cpp::SharedData(brick.voxel_data->data(), math::vec3ul(brick.dims));
    } else if (voxel_type_string == "uint16") {
        osp_data = cpp::SharedData(reinterpret_cast<uint16_t *>(brick.voxel_data->data()),
                                   math::vec3ul(brick.dims));
    } else if (voxel_type_string == "float32") {
        osp_data = cpp::SharedData(reinterpret_cast<float *>(brick.voxel_data->data()),
                                   math::vec3ul(brick.dims));
    } else if (voxel_type_string == "float64") {
        osp_data = cpp::SharedData(reinterpret_cast<double *>(brick.voxel_data->data()),
                                   math::vec3ul(brick.dims));
    }
    brick.brick.setParam("data", osp_data);
    brick.brick.commit();
    brick.model = cpp::VolumetricModel(brick.brick);
    return brick;
}

VolumeBrick load_idx_volume(const std::string &idx_file, json &config)
{
    VolumeBrick brick;
#ifdef OPENVISUS_FOUND
    using namespace Visus;

    IdxModule::attach();

    auto dataset = LoadDataset(idx_file);
    auto access = dataset->createAccess();
    const auto bounds = dataset->getLogicBox();
    auto field = dataset->getDefaultField();
    auto query = std::make_shared<BoxQuery>(dataset.get(), field, 0, 'r');
    query->logic_box = bounds;
    query->setResolutionRange(0, dataset->getMaxResolution());

    dataset->beginQuery(query);
    if (!dataset->executeQuery(access, query)) {
        std::cerr << "[error]: OpenVisus failed to execute query on " << idx_file << "\n";
        throw std::runtime_error("[error]: OpenVisus failed to execute query");
    }

    brick.bounds =
        math::box3f(math::vec3f(0), math::vec3f(bounds.p2[0], bounds.p2[1], bounds.p2[2]));
    brick.dims =
        math::vec3i(query->buffer.dims[0], query->buffer.dims[1], query->buffer.dims[2]);
    config["dims"] = {brick.dims.x, brick.dims.y, brick.dims.z};
    config["spacing"] = {1, 1, 1};

    brick.brick = cpp::Volume("structuredRegular");
    brick.brick.setParam("dimensions", brick.dims);

    size_t voxel_size = 0;
    std::string voxel_type;
    const auto visus_dtype = query->field.dtype;
    if (visus_dtype == DTypes::UINT8) {
        voxel_type = "uint8";
        voxel_size = 1;
        brick.brick.setParam("voxelType", int(OSP_UCHAR));
    } else if (visus_dtype == DTypes::UINT16) {
        voxel_type = "uint16";
        voxel_size = 2;
        brick.brick.setParam("voxelType", int(OSP_USHORT));
    } else if (visus_dtype == DTypes::FLOAT32) {
        voxel_type = "float32";
        voxel_size = 4;
        brick.brick.setParam("voxelType", int(OSP_FLOAT));
    } else if (visus_dtype == DTypes::FLOAT64) {
        voxel_type = "float64";
        voxel_size = 8;
        brick.brick.setParam("voxelType", int(OSP_DOUBLE));
    }
    config["type"] = voxel_type;

    const size_t n_voxels = size_t(brick.dims.x) * size_t(brick.dims.y) * size_t(brick.dims.z);
    brick.voxel_data = std::make_shared<std::vector<uint8_t>>(n_voxels * voxel_size, 0);
    std::memcpy(brick.voxel_data->data(), query->buffer.c_ptr(), brick.voxel_data->size());

    cpp::SharedData osp_data;
    if (voxel_type == "uint8") {
        osp_data = cpp::SharedData(brick.voxel_data->data(), math::vec3ul(brick.dims));
    } else if (voxel_type == "uint16") {
        osp_data = cpp::SharedData(reinterpret_cast<uint16_t *>(brick.voxel_data->data()),
                                   math::vec3ul(brick.dims));
    } else if (voxel_type == "float32") {
        osp_data = cpp::SharedData(reinterpret_cast<float *>(brick.voxel_data->data()),
                                   math::vec3ul(brick.dims));
    } else if (voxel_type == "float64") {
        osp_data = cpp::SharedData(reinterpret_cast<double *>(brick.voxel_data->data()),
                                   math::vec3ul(brick.dims));
    }
    brick.brick.setParam("data", osp_data);
    brick.brick.commit();
    brick.model = cpp::VolumetricModel(brick.brick);
    IdxModule::detach();
#else
    std::cerr << "[error]: Compile with OpenVisus to include support for loading IDX files\n";
    throw std::runtime_error("OpenVisus is required for IDX support");
#endif
    return brick;
}

std::vector<cpp::Geometry> extract_isosurfaces(const json &config,
                                               const VolumeBrick &brick,
                                               const std::vector<float> &isovalues)
{
    std::vector<cpp::Geometry> isosurfaces;
#ifdef USE_EXPLICIT_ISOSURFACE
    const std::string voxel_type_string = config["type"].get<std::string>();
    vtkSmartPointer<vtkDataArray> data_array = nullptr;
    if (voxel_type_string == "uint8") {
        auto arr = vtkSmartPointer<vtkUnsignedCharArray>::New();
        arr->SetArray(brick.voxel_data->data(), brick.voxel_data->size(), 1);
        data_array = arr;
    } else if (voxel_type_string == "uint16") {
        auto arr = vtkSmartPointer<vtkUnsignedShortArray>::New();
        arr->SetArray(reinterpret_cast<uint16_t *>(brick.voxel_data->data()),
                      brick.voxel_data->size() / sizeof(uint16_t),
                      1);
        data_array = arr;
    } else if (voxel_type_string == "float32") {
        auto arr = vtkSmartPointer<vtkFloatArray>::New();
        arr->SetArray(reinterpret_cast<float *>(brick.voxel_data->data()),
                      brick.voxel_data->size() / sizeof(float),
                      1);
        data_array = arr;
    } else if (voxel_type_string == "float64") {
        auto arr = vtkSmartPointer<vtkDoubleArray>::New();
        arr->SetArray(reinterpret_cast<double *>(brick.voxel_data->data()),
                      brick.voxel_data->size() / sizeof(double),
                      1);
        data_array = arr;
    } else {
        throw std::runtime_error("Unrecognized voxel type " + voxel_type_string);
    }

    const math::vec3f grid_spacing = get_vec<float, 3>(config["spacing"]);
    vtkSmartPointer<vtkImageData> img_data = vtkSmartPointer<vtkImageData>::New();
    img_data->SetDimensions(brick.dims.x, brick.dims.y, brick.dims.z);
    img_data->SetSpacing(grid_spacing.x, grid_spacing.y, grid_spacing.z);
    img_data->SetOrigin(brick.bounds.lower.x, brick.bounds.lower.y, brick.bounds.lower.z);
    img_data->GetPointData()->SetScalars(data_array);

    for (const auto &v : isovalues) {
        vtkSmartPointer<vtkFlyingEdges3D> fedges = vtkSmartPointer<vtkFlyingEdges3D>::New();
        fedges->SetInputData(img_data);
        fedges->SetNumberOfContours(1);
        fedges->SetValue(0, v);
        fedges->SetComputeNormals(false);
        fedges->Update();
        vtkPolyData *isosurf = fedges->GetOutput();

        std::vector<math::vec3f> vertices;
        std::vector<math::vec3ui> indices;
        vertices.reserve(isosurf->GetNumberOfCells());
        indices.reserve(isosurf->GetNumberOfCells());
        for (size_t i = 0; i < isosurf->GetNumberOfCells(); ++i) {
            vtkTriangle *tri = dynamic_cast<vtkTriangle *>(isosurf->GetCell(i));
            if (tri->ComputeArea() == 0.0) {
                continue;
            }
            math::vec3ui tids;
            for (size_t v = 0; v < 3; ++v) {
                const double *pt = isosurf->GetPoint(tri->GetPointId(v));
                const math::vec3f vert(pt[0], pt[1], pt[2]);
                tids[v] = vertices.size();
                vertices.push_back(vert);
            }
            indices.push_back(tids);
        }
        if (!indices.empty()) {
            std::cout << "Isosurface has " << indices.size() << " triangles\n";
            cpp::Geometry isosurface("mesh");
            isosurface.setParam("vertex.position", cpp::CopiedData(vertices));
            isosurface.setParam("index", cpp::CopiedData(indices));
            isosurface.commit();
            isosurfaces.push_back(isosurface);
        } else {
            std::cout << "Isosurface at " << v << " is empty\n";
        }
    }
#else
    cpp::Geometry isosurface("isosurface");
    isosurface.setParam("isovalue", cpp::CopiedData(isovalues));
    isosurface.setParam("volume", brick.brick);
    isosurface.commit();
    isosurfaces.push_back(isosurface);
#endif
    return isosurfaces;
}

