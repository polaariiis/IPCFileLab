#pragma once

class ChannelNotifier
{
public:
	explicit ChannelNotifier(const char* name);
	~ChannelNotifier();

	ChannelNotifier(const ChannelNotifier&) = delete;
	ChannelNotifier& operator=(const ChannelNotifier&) = delete;

	void notify();
	void wait();
private:
	void* handle_;
};