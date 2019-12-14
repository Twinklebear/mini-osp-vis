#include "util.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

std::string get_file_content(const std::string &fname)
{
    std::ifstream file{fname};
    if (!file.is_open()) {
        std::cout << "Failed to open file: " << fname << std::endl;
        return "";
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string get_file_extension(const std::string &fname)
{
    const size_t fnd = fname.find_last_of('.');
    if (fnd == std::string::npos) {
        return "";
    }
    return fname.substr(fnd + 1);
}

std::string get_file_basename(const std::string &path)
{
    size_t fname_offset = path.find_last_of('/');
    if (fname_offset == std::string::npos) {
        return path;
    }
    return path.substr(fname_offset + 1);
}

std::string get_file_basepath(const std::string &path)
{
    size_t end = path.find_last_of('/');
    if (end == std::string::npos) {
        return path;
    }
    return path.substr(0, end);
}

bool starts_with(const std::string &str, const std::string &prefix)
{
    return std::strncmp(str.c_str(), prefix.c_str(), prefix.size()) == 0;
}

