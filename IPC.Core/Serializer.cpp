#include "Serializer.h"

std::vector<std::byte> Serializer::serialize(const Message& message)
{
	std::vector<std::byte> data(message.payload.size());

	std::memcpy(data.data(), message.payload.data(), message.payload.size());
	return data;
}

Message Serializer::deserialize(const std::vector<std::byte>& data)
{
	Message message;

	message.payload.resize(data.size());
	std::memcpy(message.payload.data(), data.data(), data.size());
	return message;
}