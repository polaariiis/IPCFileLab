#include "ChannelNotifier.h"

#include <Windows.h>

#include <stdexcept>

ChannelNotifier::ChannelNotifier(const char* name) : handle_(CreateEventA(nullptr, FALSE, FALSE, name))
{
	if (handle_ == nullptr)
		throw std::runtime_error("Failed to create named event");
}

ChannelNotifier::~ChannelNotifier()
{
	if (handle_ != nullptr)
		CloseHandle(static_cast<HANDLE>(handle_));
}

void ChannelNotifier::notify()
{
	if (!SetEvent(static_cast<HANDLE>(handle_)))
		throw std::runtime_error("Failed to signal event");
}

void ChannelNotifier::wait()
{
	const DWORD result = WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);

	if (result != WAIT_OBJECT_0)
		throw std::runtime_error("Failed to wait for event");
}