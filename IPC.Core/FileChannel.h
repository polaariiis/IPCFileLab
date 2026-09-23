#pragma once

#include "ChannelState.h"
#include "ChannelNotifier.h"
#include "ChannelSynchronizer.h"
#include "Message.h"

#include <string>

class FileChannel
{
public:
	explicit FileChannel(const std::string& path);
	void send(const Message& message);
	Message recieve();
private:
	std::string path_;

	ChannelSynchronizer synchronizer_;
	ChannelNotifier dataNotifier_;
	ChannelNotifier spaceNotifier_;

	void initialize();
	ChannelState getState() const;
	void setState(ChannelState state);
};
