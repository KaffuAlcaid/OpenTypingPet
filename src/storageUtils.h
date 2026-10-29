#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Storage {
std::string utf8(const std::wstring &text);
std::wstring wide(const std::string &text);
std::string hash(const std::vector<uint8_t> &bytes);
std::string newId();
std::vector<uint8_t> read(const std::filesystem::path &path, uint64_t limit = 64 * 1024 * 1024);
void write(const std::filesystem::path &path, const std::vector<uint8_t> &bytes, bool backup = false);
}
