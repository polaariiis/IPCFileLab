#pragma once

#include <cstddef>
#include <string>
#include <vector>

class FileStorage
{
public:
	static void write(const std::string& path, const std::vector<std::byte>& data);
	static std::vector<std::byte> read(const std::string& path);
};
