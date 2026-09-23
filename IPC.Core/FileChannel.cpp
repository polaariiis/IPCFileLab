#include "FileChannel.h"

#include "ChannelHeader.h"
#include "FileStorage.h"
#include "Serializer.h"

#include <iostream>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>


namespace
{
	constexpr std::size_t StateSize = sizeof(std::uint8_t);
	constexpr std::size_t PayloadSizeFieldSize = sizeof(std::uint32_t);
	constexpr std::size_t HeaderSize = StateSize + PayloadSizeFieldSize;
}

FileChannel::FileChannel(const std::string& path)
	: path_(path),
	synchronizer_("IPCFileLabMutex"),
	dataNotifier_("IPCFileLabDataEvent"),
	spaceNotifier_("IPCFileLabSpaceEvent")
{
	initialize();
}

void FileChannel::initialize()
{
	synchronizer_.lock();

	try
	{
		if (!std::filesystem::exists(path_))
		{
			std::vector<std::byte> data(HeaderSize);
			const auto rawState = static_cast<std::uint8_t>(ChannelState::Empty);
			const std::uint32_t payloadSize{};

			std::memcpy(data.data(), &rawState, StateSize);
			std::memcpy(data.data() + StateSize, &payloadSize, PayloadSizeFieldSize);

			FileStorage::write(path_, data);
		}

		synchronizer_.unlock();
	}
	catch (...)
	{
		synchronizer_.unlock();
		throw;
	}
}

void FileChannel::send(const Message& message)
{
	while (true)
	{
		synchronizer_.lock();

		try
		{
			const auto state = getState();
			std::cout << "Sender state: "
				<< static_cast<int>(state)
				<< '\n';

			if (state != ChannelState::Empty)
			{
				synchronizer_.unlock();
			}
			else
			{
				const auto payload = Serializer::serialize(message);

				if (payload.size() > static_cast<std::size_t>(UINT32_MAX))
					throw std::runtime_error("Message is too large");

				ChannelHeader header;
				header.state = ChannelState::Writing;
				header.payloadSize = static_cast<std::uint32_t>(payload.size());

				std::vector<std::byte> data(HeaderSize + payload.size());

				const auto rawState = static_cast<std::uint8_t>(header.state);

				std::memcpy(data.data(), &rawState, StateSize);
				std::memcpy(data.data() + StateSize, &header.payloadSize, PayloadSizeFieldSize);
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
		synchronizer_.lock();

		try
		{
			const auto data = FileStorage::read(path_);

			if (data.size() < HeaderSize)
				throw std::runtime_error("IPC data is too small");

			std::uint8_t rawState{};
			std::uint32_t payloadSize{};

			std::memcpy(&rawState, data.data(), StateSize);
			std::memcpy(&payloadSize, data.data() + StateSize, PayloadSizeFieldSize);

			const auto state = getState();
			std::cout << "Receiver state: "
				<< static_cast<int>(rawState)
				<< '\n';

			if (state == ChannelState::Ready)
			{
				if (data.size() != HeaderSize + payloadSize)
					throw std::runtime_error("IPC data has invalid payload size");

				setState(ChannelState::Reading);

				std::vector<std::byte> payload(payloadSize);

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

	switch (rawState)
	{
	case static_cast<std::uint8_t>(ChannelState::Empty):
	case static_cast<std::uint8_t>(ChannelState::Writing):
	case static_cast<std::uint8_t>(ChannelState::Ready):
	case static_cast<std::uint8_t>(ChannelState::Reading):
		return static_cast<ChannelState>(rawState);
	default:
		throw std::runtime_error("IPC data has invalid channel state");
	}
}

void FileChannel::setState(ChannelState state)
{
	auto data = FileStorage::read(path_);

	if (data.size() < HeaderSize)
		throw std::runtime_error("IPC data is too small to contain channel header");

	const auto rawState = static_cast<std::uint8_t>(state);

	std::memcpy(data.data(), &rawState, StateSize);
	FileStorage::write(path_, data);
}
