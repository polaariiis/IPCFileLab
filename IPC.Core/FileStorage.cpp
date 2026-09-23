#include "FileStorage.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

void FileStorage::write(const std::string& path, const std::vector<std::byte>& data)
{
	const std::filesystem::path target(path);
	const std::string directory = target.has_parent_path() ? target.parent_path().string() : ".";
	char temporaryPath[MAX_PATH]{};

	if (GetTempFileNameA(directory.c_str(), "ipc", 0, temporaryPath) == 0)
		throw std::runtime_error("Failed to create temporary file");

	try
	{
		std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);

		if (!file)
			throw std::runtime_error("Failed to open temporary file for writing");

		if (!data.empty())
			file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));

		file.flush();

		if (!file)
			throw std::runtime_error("Failed to write temporary file");

		file.close();

		if (!file)
			throw std::runtime_error("Failed to close temporary file");

		if (!MoveFileExA(temporaryPath, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			throw std::runtime_error("Failed to replace file");
	}
	catch (...)
	{
		DeleteFileA(temporaryPath);
		throw;
	}
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

	if (!data.empty())
		file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));

	if (!file)
		throw std::runtime_error("Failed to read file");

	return data;
}
