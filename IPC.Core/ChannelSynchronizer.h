#pragma once

class ChannelSynchronizer
{
public:
	explicit ChannelSynchronizer(const char* name);

	~ChannelSynchronizer();

	ChannelSynchronizer(const ChannelSynchronizer&) = delete;
	ChannelSynchronizer& operator=(const ChannelSynchronizer&) = delete;

	bool lock();
	void unlock();
private:
	void* handle_;
};
