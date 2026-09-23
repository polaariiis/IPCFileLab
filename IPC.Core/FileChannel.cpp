#include "FileChannel.h"

#include "ChannelHeader.h"
#include "FileStorage.h"
#include "Serializer.h"

#include <iostream>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <vector>


namespace
{
	constexpr std::size_t StateSize = sizeof(std::uint8_t);
	constexpr std::size_t PayloadSizeFieldSize = sizeof(std::uint32_t);
	constexpr std::size_t HeaderSize = StateSize + PayloadSizeFieldSize;
	constexpr std::size_t MaxPayloadSize = 16 * 1024 * 1024;

	std::string createName(const char* prefix, const std::string& path)
	{
		return std::string(prefix) + std::to_string(std::hash<std::string>{}(path));
	}

	void writeEmpty(const std::string& path)
	{
		std::vector<std::byte> data(HeaderSize);
		const auto rawState = static_cast<std::uint8_t>(ChannelState::Empty);
		const std::uint32_t payloadSize{};

		std::memcpy(data.data(), &rawState, StateSize);
		std::memcpy(data.data() + StateSize, &payloadSize, PayloadSizeFieldSize);

		FileStorage::write(path, data);
	}

	bool isValidTransition(ChannelState current, ChannelState next)
	{
		return (current == ChannelState::Writing && next == ChannelState::Ready)
			|| (current == ChannelState::Ready && next == ChannelState::Reading)
			|| (current == ChannelState::Reading && next == ChannelState::Empty);
	}

	bool isRecoverableState(ChannelState state)
	{
		return state == ChannelState::Writing || state == ChannelState::Reading;
	}
}

FileChannel::FileChannel(const std::string& path)
	: path_(std::filesystem::absolute(path).lexically_normal().string()),
	synchronizer_(createName("IPCFileLabMutex", path_).c_str()),
	dataNotifier_(createName("IPCFileLabDataEvent", path_).c_str()),
	spaceNotifier_(createName("IPCFileLabSpaceEvent", path_).c_str())
{
	initialize();
}

void FileChannel::initialize()
{
	synchronizer_.lock();

	try
	{
		if (!std::filesystem::exists(path_))
			writeEmpty(path_);
		else
		{
			const auto state = getState();

			if (isRecoverableState(state))
				recover();
		}

		synchronizer_.unlock();
	}
	catch (...)
	{
		synchronizer_.unlock();
		throw;
	}
}

void FileChannel::recover()
{
	writeEmpty(path_);
}

void FileChannel::send(const Message& message)
{
	while (true)
	{
		const bool abandoned = synchronizer_.lock();

		try
		{
			const auto state = getState();

			if (isRecoverableState(state))
			{
				if (!abandoned)
					throw std::runtime_error("IPC data has stale channel state");

				recover();
			}

			const auto currentState = getState();
			std::cout << "Sender state: "
				<< static_cast<int>(currentState)
				<< '\n';

			if (currentState != ChannelState::Empty)
			{
				synchronizer_.unlock();
			}
			else
			{
				const auto payload = Serializer::serialize(message);

				if (payload.size() > MaxPayloadSize)
					throw std::runtime_error("Message is too large");

				ChannelHeader header;
				header.state = ChannelState::Writing;
				header.payloadSize = static_cast<std::uint32_t>(payload.size());

				std::vector<std::byte> data(HeaderSize + payload.size());

				const auto rawState = static_cast<std::uint8_t>(header.state);

				std::memcpy(data.data(), &rawState, StateSize);
				std::memcpy(data.data() + StateSize, &header.payloadSize, PayloadSizeFieldSize);
				if (!payload.empty())
					std::memcpy(data.data() + HeaderSize, payload.data(), payload.size());

				FileStorage::write(path_, data);

				setState(ChannelState::Ready);
				dataNotifier_.notify();

				synchronizer_.unlock();

				return;
			}
		}
		catch (...)
		{
			synchronizer_.unlock();
			throw;
		}

		spaceNotifier_.wait();
	}
}

Message FileChannel::recieve()
{
	while (true)
	{
		const bool abandoned = synchronizer_.lock();

		try
		{
			const auto state = getState();

			if (isRecoverableState(state))
			{
				if (!abandoned)
					throw std::runtime_error("IPC data has stale channel state");

				recover();
			}

			const auto data = FileStorage::read(path_);

			if (data.size() < HeaderSize)
				throw std::runtime_error("IPC data is too small");

			std::uint8_t rawState{};
			std::uint32_t payloadSize{};

			std::memcpy(&rawState, data.data(), StateSize);
			std::memcpy(&payloadSize, data.data() + StateSize, PayloadSizeFieldSize);

			const auto currentState = getState();
			std::cout << "Receiver state: "
				<< static_cast<int>(rawState)
				<< '\n';

			if (currentState == ChannelState::Ready)
			{
				if (data.size() != HeaderSize + payloadSize)
					throw std::runtime_error("IPC data has invalid payload size");

				setState(ChannelState::Reading);

				std::vector<std::byte> payload(payloadSize);

				if (!payload.empty())
					std::memcpy(payload.data(), data.data() + HeaderSize, payloadSize);

				Message message = Serializer::deserialize(payload);

				setState(ChannelState::Empty);
				spaceNotifier_.notify();

				synchronizer_.unlock();

				return message;
			}

			synchronizer_.unlock();
		}
		catch (...)
		{
			synchronizer_.unlock();
			throw;
		}

		dataNotifier_.wait();
	}
}

ChannelState FileChannel::getState() const
{
	const auto data = FileStorage::read(path_);

	if (data.size() < HeaderSize)
		throw std::runtime_error("IPC data is too small to contain channel header");

	std::uint8_t rawState{};

	std::memcpy(&rawState, data.data(), StateSize);

	std::uint32_t payloadSize{};

	std::memcpy(&payloadSize, data.data() + StateSize, PayloadSizeFieldSize);

	switch (rawState)
	{
	case static_cast<std::uint8_t>(ChannelState::Empty):
		if (payloadSize != 0 || data.size() != HeaderSize)
			throw std::runtime_error("IPC data has invalid empty channel size");

		return ChannelState::Empty;
	case static_cast<std::uint8_t>(ChannelState::Writing):
	case static_cast<std::uint8_t>(ChannelState::Ready):
	case static_cast<std::uint8_t>(ChannelState::Reading):
		if (payloadSize > MaxPayloadSize || data.size() != HeaderSize + payloadSize)
			throw std::runtime_error("IPC data has invalid payload size");

		return static_cast<ChannelState>(rawState);
	default:
		throw std::runtime_error("IPC data has invalid channel state");
	}
}

void FileChannel::setState(ChannelState state)
{
	const auto current = getState();

	if (!isValidTransition(current, state))
		throw std::runtime_error("IPC data has invalid channel state transition");

	if (state == ChannelState::Empty)
	{
		writeEmpty(path_);
		return;
	}

	auto data = FileStorage::read(path_);

	if (data.size() < HeaderSize)
		throw std::runtime_error("IPC data is too small to contain channel header");

	const auto rawState = static_cast<std::uint8_t>(state);

	std::memcpy(data.data(), &rawState, StateSize);
	FileStorage::write(path_, data);
}
