#include "ChannelSynchronizer.h"

#include <Windows.h>

#include <stdexcept>

ChannelSynchronizer::ChannelSynchronizer(const char* name) : handle_(CreateMutexA(nullptr, FALSE, name))
{
	if (handle_ == nullptr)
		throw std::runtime_error("Failed to create named mutex");
}

ChannelSynchronizer::~ChannelSynchronizer()
{
	if (handle_ != nullptr)
		CloseHandle(static_cast<HANDLE>(handle_));
}

bool ChannelSynchronizer::lock()
{
	const DWORD result = WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);

	if (result == WAIT_OBJECT_0)
		return false;

	if (result == WAIT_ABANDONED)
		return true;

	throw std::runtime_error("Failed to acquire mutex");
}

void ChannelSynchronizer::unlock()
{
	if (!ReleaseMutex(static_cast<HANDLE>(handle_)))
		throw std::runtime_error("Failed to release mutex");
}
