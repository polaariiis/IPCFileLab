#include "FileStorage.h"

#include <fstream>
#include <stdexcept>

void FileStorage::write(const std::string& path, const std::vector<std::byte>& data)
{
	std::ofstream file(path, std::ios::binary);

	if (!file)
		throw std::runtime_error("Failed to open file for writing");

	file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));

	if (!file)
		throw std::runtime_error("Failed to write file");
}

std::vector<std::byte> FileStorage::read(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);

	if (!file)
		throw std::runtime_error("Failed to open file for reading");

	file.seekg(0, std::ios::end);

	const auto fileSize = file.tellg();
	
	if (fileSize < 0)
		throw std::runtime_error("Failed to determine file size");

	file.seekg(0, std::ios::beg);

	std::vector<std::byte> data(static_cast<std::size_t>(fileSize));

	file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));

	if (!file)
		throw std::runtime_error("Failed to read file");

	return data;
}
