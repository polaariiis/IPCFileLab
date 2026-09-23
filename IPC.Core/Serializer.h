#pragma once

#include "Message.h"

#include <cstdint>
#include <vector>

class Serializer
{
public:
	static std::vector<std::byte> serialize(const Message& message);
	static Message deserialize(const std::vector<std::byte>& data);
};