#include "ChannelState.h"
#include "FileChannel.h"
#include "FileStorage.h"
#include "Message.h"
#include "Serializer.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>


namespace
{
	constexpr std::size_t StateSize = sizeof(std::uint8_t);
	constexpr std::size_t PayloadSizeFieldSize = sizeof(std::uint32_t);
	constexpr std::size_t HeaderSize = StateSize + PayloadSizeFieldSize;

	std::string createPath(const std::string& name)
	{
		const auto directory = std::filesystem::temp_directory_path() / "IPCFileLabTests";

		std::filesystem::create_directories(directory);

		return (directory / (name + "_" + std::to_string(GetCurrentProcessId()) + ".dat")).string();
	}

	std::vector<std::byte> createData(ChannelState state, const std::string& payload, std::uint32_t payloadSize)
	{
		std::vector<std::byte> data(HeaderSize + payload.size());
		const auto rawState = static_cast<std::uint8_t>(state);

		std::memcpy(data.data(), &rawState, StateSize);
		std::memcpy(data.data() + StateSize, &payloadSize, PayloadSizeFieldSize);

		if (!payload.empty())
			std::memcpy(data.data() + HeaderSize, payload.data(), payload.size());

		return data;
	}

	class FileTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			path_ = createPath(::testing::UnitTest::GetInstance()->current_test_info()->name());
			std::filesystem::remove(path_);
		}

		void TearDown() override
		{
			std::filesystem::remove(path_);
		}

		std::string path_;
	};
}

TEST(SerializerTest, SerializesEmptyMessage)
{
	const Message message{};

	EXPECT_TRUE(Serializer::serialize(message).empty());
}

TEST(SerializerTest, RoundTripsNormalMessage)
{
	const Message message{ "Hello, IPC" };

	EXPECT_EQ(Serializer::deserialize(Serializer::serialize(message)).payload, message.payload);
}

TEST(SerializerTest, RoundTripsLongMessage)
{
	const Message message{ std::string(1024 * 1024, 'x') };

	EXPECT_EQ(Serializer::deserialize(Serializer::serialize(message)).payload, message.payload);
}

TEST_F(FileTest, FileStorageWritesAndReadsData)
{
	const std::vector<std::byte> data{
		std::byte{ 1 },
		std::byte{ 2 },
		std::byte{ 3 }
	};

	FileStorage::write(path_, data);

	EXPECT_EQ(FileStorage::read(path_), data);
}

TEST_F(FileTest, FileStorageWritesAndReadsEmptyData)
{
	FileStorage::write(path_, {});

	EXPECT_TRUE(FileStorage::read(path_).empty());
}

TEST_F(FileTest, FileStorageThrowsForMissingFile)
{
	EXPECT_THROW(FileStorage::read(path_), std::runtime_error);
}

TEST_F(FileTest, ChannelInitializesEmptyFile)
{
	FileChannel channel(path_);
	const auto data = FileStorage::read(path_);
	const auto expected = createData(ChannelState::Empty, "", 0);

	EXPECT_EQ(data, expected);
}

TEST_F(FileTest, ChannelPreservesExistingReadyMessage)
{
	FileStorage::write(path_, createData(ChannelState::Ready, "saved", 5));
	FileChannel channel(path_);

	EXPECT_EQ(channel.recieve().payload, "saved");
}

TEST_F(FileTest, ChannelRejectsInvalidState)
{
	auto data = createData(ChannelState::Empty, "", 0);
	data[0] = std::byte{ 254 };
	FileStorage::write(path_, data);

	EXPECT_THROW((void)FileChannel{ path_ }, std::runtime_error);
}

TEST_F(FileTest, ChannelRejectsInvalidPayloadSize)
{
	FileStorage::write(path_, createData(ChannelState::Ready, "short", 6));

	EXPECT_THROW((void)FileChannel{ path_ }, std::runtime_error);
}

TEST_F(FileTest, ChannelRecoversStaleWritingState)
{
	FileStorage::write(path_, createData(ChannelState::Writing, "partial", 7));
	FileChannel channel(path_);

	EXPECT_EQ(FileStorage::read(path_), createData(ChannelState::Empty, "", 0));
}

TEST_F(FileTest, ChannelRecoversStaleReadingState)
{
	FileStorage::write(path_, createData(ChannelState::Reading, "delivered", 9));
	FileChannel channel(path_);

	EXPECT_EQ(FileStorage::read(path_), createData(ChannelState::Empty, "", 0));
}

TEST_F(FileTest, ChannelSendsAndReceivesEmptyMessage)
{
	FileChannel channel(path_);

	channel.send(Message{});

	EXPECT_EQ(channel.recieve().payload, "");
}

TEST_F(FileTest, ChannelSendsAndReceivesLongMessage)
{
	FileChannel channel(path_);
	const Message message{ std::string(1024 * 1024, 'x') };

	channel.send(message);

	EXPECT_EQ(channel.recieve().payload, message.payload);
}

TEST_F(FileTest, ReceiverWaitsForData)
{
	FileChannel receiver(path_);
	FileChannel sender(path_);
	std::promise<void> started;
	const auto ready = started.get_future();
	auto message = std::async(std::launch::async, [&receiver, &started]
	{
		started.set_value();
		return receiver.recieve();
	});

	ready.wait();
	sender.send(Message{ "receiver waiting" });

	ASSERT_EQ(message.wait_for(std::chrono::seconds(5)), std::future_status::ready);
	EXPECT_EQ(message.get().payload, "receiver waiting");
}

TEST_F(FileTest, SenderWaitsForAvailableSpace)
{
	FileChannel sender(path_);
	FileChannel receiver(path_);

	sender.send(Message{ "first" });

	auto result = std::async(std::launch::async, [&sender]
	{
		sender.send(Message{ "second" });
		return true;
	});

	EXPECT_EQ(result.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
	EXPECT_EQ(receiver.recieve().payload, "first");
	ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
	EXPECT_TRUE(result.get());
	EXPECT_EQ(receiver.recieve().payload, "second");
}

TEST_F(FileTest, RepeatedExchangePreservesMessageOrder)
{
	FileChannel sender(path_);
	FileChannel receiver(path_);
	auto sendResult = std::async(std::launch::async, [&sender]
	{
		for (int index = 0; index < 25; ++index)
			sender.send(Message{ "message " + std::to_string(index) });
	});
	std::vector<std::string> messages;

	for (int index = 0; index < 25; ++index)
		messages.push_back(receiver.recieve().payload);

	ASSERT_EQ(sendResult.wait_for(std::chrono::seconds(5)), std::future_status::ready);
	sendResult.get();

	for (int index = 0; index < 25; ++index)
		EXPECT_EQ(messages[index], "message " + std::to_string(index));
}

TEST_F(FileTest, MultipleSendersDeliverEachMessageOnce)
{
	FileChannel receiver(path_);
	FileChannel firstSender(path_);
	FileChannel secondSender(path_);
	auto first = std::async(std::launch::async, [&firstSender]
	{
		firstSender.send(Message{ "first sender" });
	});
	auto second = std::async(std::launch::async, [&secondSender]
	{
		secondSender.send(Message{ "second sender" });
	});
	std::vector<std::string> messages{
		receiver.recieve().payload,
		receiver.recieve().payload
	};

	ASSERT_EQ(first.wait_for(std::chrono::seconds(5)), std::future_status::ready);
	ASSERT_EQ(second.wait_for(std::chrono::seconds(5)), std::future_status::ready);
	first.get();
	second.get();
	std::sort(messages.begin(), messages.end());

	EXPECT_EQ(messages[0], "first sender");
	EXPECT_EQ(messages[1], "second sender");
}

TEST_F(FileTest, MultipleReceiversDeliverEachMessageOnce)
{
	FileChannel sender(path_);
	FileChannel firstReceiver(path_);
	FileChannel secondReceiver(path_);
	auto first = std::async(std::launch::async, [&firstReceiver]
	{
		return firstReceiver.recieve().payload;
	});
	auto second = std::async(std::launch::async, [&secondReceiver]
	{
		return secondReceiver.recieve().payload;
	});

	sender.send(Message{ "first message" });
	sender.send(Message{ "second message" });

	ASSERT_EQ(first.wait_for(std::chrono::seconds(5)), std::future_status::ready);
	ASSERT_EQ(second.wait_for(std::chrono::seconds(5)), std::future_status::ready);
	std::vector<std::string> messages{ first.get(), second.get() };
	std::sort(messages.begin(), messages.end());

	EXPECT_EQ(messages[0], "first message");
	EXPECT_EQ(messages[1], "second message");
}
