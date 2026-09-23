#pragma once

class ChannelSynchronizer
{
public:
	explicit ChannelSynchronizer(const char* name);

	~ChannelSynchronizer();

	ChannelSynchronizer(const ChannelSynchronizer&) = delete;
	ChannelSynchronizer& operator=(const ChannelSynchronizer&) = delete;

	void lock();
	void unlock();
private:
	void* handle_;
};
