// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <stdexcept>
#include <fstream>
#include "VulkanUtil.h"

std::vector<uint32_t> musevk::VulkanUtil::loadSpirv(std::string const &executable_dir, std::string const &filename) {
    std::string full_path = executable_dir + "/shaders/" + filename + ".spv";
    std::ifstream file_stream(full_path, std::ios::binary);
    if (file_stream.fail())
        throw std::runtime_error("Unable to open shader spirv file " + full_path);
    std::vector<char> buffer;
    buffer.insert(buffer.begin(), std::istreambuf_iterator<char>(file_stream), {});
    return {(uint32_t*)buffer.data(), (uint32_t*)(buffer.data() + buffer.size())};
}
