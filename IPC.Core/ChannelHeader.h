#pragma once

#include "ChannelState.h"

#include <cstdint>

struct ChannelHeader
{
	ChannelState state;
	std::uint32_t payloadSize;
};
