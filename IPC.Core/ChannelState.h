#pragma once 

#include <cstdint>

enum class ChannelState : std::uint8_t
{
	Empty = 0,
	Writing,
	Ready,
	Reading
};
