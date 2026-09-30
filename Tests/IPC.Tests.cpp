// IPC.Tests: the C library through its public API (ipc.h) and its internal parts
// (protocol, file storage), with threads and with real processes (IPC.TestHelper).

#include "file_storage.h"
#include "ipc.h"
#include "protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define popen _popen
#define pclose _pclose
#define current_process_id() _getpid()
#else
#include <sys/wait.h>
#include <unistd.h>
#define current_process_id() getpid()
#endif

namespace
{
    using Bytes = std::vector<unsigned char>;

    Bytes channelFile(channel_state state, const std::string& payload, std::uint32_t payloadSize)
    {
        Bytes data(CHANNEL_HEADER_SIZE);
        const channel_header header{ state, payloadSize };

        protocol_encode_header(&header, data.data());
        data.insert(data.end(), payload.begin(), payload.end());
        return data;
    }

    Bytes readFile(const std::string& path)
    {
        unsigned char* data = nullptr;
        std::size_t size = 0;

        EXPECT_EQ(file_storage_read(path.c_str(), 64u * 1024u * 1024u, &data, &size), IPC_OK);
        Bytes bytes(data, data + size);
        std::free(data);
        return bytes;
    }

    void writeFile(const std::string& path, const Bytes& data)
    {
        ASSERT_EQ(file_storage_write(path.c_str(), data.data(), data.size()), IPC_OK);
    }

    // An open channel handle for a test.
    struct Channel
    {
        explicit Channel(const std::string& path)
        {
            EXPECT_EQ(ipc_channel_open(path.c_str(), &handle), IPC_OK);
        }
        ~Channel() { ipc_channel_close(handle); }
        Channel(const Channel&) = delete;
        Channel& operator=(const Channel&) = delete;

        ipc_status send(const std::string& text, int timeoutMs = 5000)
        {
            return ipc_channel_send(handle, text.data(), text.size(), timeoutMs);
        }

        std::string receive(int timeoutMs = 5000)
        {
            void* data = nullptr;
            std::size_t size = 0;
            const ipc_status status = ipc_channel_receive(handle, &data, &size, timeoutMs);

            EXPECT_EQ(status, IPC_OK) << ipc_status_string(status);
            std::string text(size > 0 ? static_cast<const char*>(data) : "", size);
            ipc_free(data);
            return text;
        }

        ipc_channel* handle = nullptr;
    };

    struct ProcessResult
    {
        int exitCode = -1;
        std::string output;
    };

    // Runs IPC.TestHelper with `arguments` and returns its exit code and standard output.
    ProcessResult runHelper(const std::string& arguments)
    {
        std::string command = std::string("\"") + IPC_TEST_HELPER + "\" " + arguments;
#ifdef _WIN32
        command = "\"" + command + "\""; // cmd /c removes the outer quotes
#endif
        ProcessResult result;
        FILE* pipe = popen(command.c_str(), "r");

        if (pipe == nullptr)
            return result;

        char buffer[256];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof buffer, pipe)) > 0)
            result.output.append(buffer, read);

        const int status = pclose(pipe);
#ifdef _WIN32
        result.exitCode = status;
#else
        result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
        return result;
    }

    std::string inQuotes(const std::string& text)
    {
        return "\"" + text + "\"";
    }

    class FileTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            directory_ = std::filesystem::temp_directory_path() / "IPCFileLabTests"
                / (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "_"
                   + std::to_string(current_process_id()));
            std::filesystem::remove_all(directory_);
            std::filesystem::create_directories(directory_);
            path_ = (directory_ / "ipc.dat").string();
        }

        void TearDown() override
        {
            std::error_code ignored;
            std::filesystem::remove_all(directory_, ignored);
        }

        std::filesystem::path directory_;
        std::string path_;
    };

    using Clock = std::chrono::steady_clock;

    long long millisecondsSince(Clock::time_point start)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    }
}

// ---------------------------------------------------------------- protocol (was Serializer)

TEST(ProtocolTest, HeaderIsStateThenLittleEndianSize)
{
    unsigned char data[CHANNEL_HEADER_SIZE];
    const channel_header header{ CHANNEL_READY, 0x01020304u };

    protocol_encode_header(&header, data);

    EXPECT_EQ(Bytes(data, data + CHANNEL_HEADER_SIZE), (Bytes{ 2, 0x04, 0x03, 0x02, 0x01 }));
}

TEST(ProtocolTest, DecodesWhatItEncodes)
{
    const Bytes file = channelFile(CHANNEL_READY, "Hello, IPC", 10);
    channel_header header{};

    ASSERT_EQ(protocol_decode_header(file.data(), file.size(), &header), IPC_OK);
    EXPECT_EQ(header.state, CHANNEL_READY);
    EXPECT_EQ(header.payload_size, 10u);
}

TEST(ProtocolTest, RejectsMalformedHeaders)
{
    channel_header header{};
    const Bytes empty = channelFile(CHANNEL_EMPTY, "", 0);
    Bytes unknownState = empty;
    unknownState[0] = 254;

    EXPECT_EQ(protocol_decode_header(empty.data(), 4, &header), IPC_CORRUPT_DATA);          // too small
    EXPECT_EQ(protocol_decode_header(unknownState.data(), 5, &header), IPC_CORRUPT_DATA);   // state
    EXPECT_EQ(protocol_decode_header(channelFile(CHANNEL_EMPTY, "x", 1).data(), 6, &header),
              IPC_CORRUPT_DATA);                                                          // EMPTY with payload
    EXPECT_EQ(protocol_decode_header(channelFile(CHANNEL_READY, "short", 6).data(), 10, &header),
              IPC_CORRUPT_DATA);                                                          // truncated
    EXPECT_EQ(protocol_decode_header(channelFile(CHANNEL_READY, "extra", 4).data(), 10, &header),
              IPC_CORRUPT_DATA);                                                          // extra bytes
    const Bytes huge = channelFile(CHANNEL_READY, "", IPC_MAX_PAYLOAD_SIZE + 1u);
    EXPECT_EQ(protocol_decode_header(huge.data(), CHANNEL_HEADER_SIZE + IPC_MAX_PAYLOAD_SIZE + 1u, &header),
              IPC_CORRUPT_DATA);                                                          // over the limit
}

TEST(ProtocolTest, OnlyTheCycleTransitionsAreValid)
{
    EXPECT_TRUE(protocol_is_valid_transition(CHANNEL_WRITING, CHANNEL_READY));
    EXPECT_TRUE(protocol_is_valid_transition(CHANNEL_READY, CHANNEL_READING));
    EXPECT_TRUE(protocol_is_valid_transition(CHANNEL_READING, CHANNEL_EMPTY));
    EXPECT_FALSE(protocol_is_valid_transition(CHANNEL_EMPTY, CHANNEL_READY));
    EXPECT_FALSE(protocol_is_valid_transition(CHANNEL_READY, CHANNEL_EMPTY));
    EXPECT_FALSE(protocol_is_valid_transition(CHANNEL_WRITING, CHANNEL_EMPTY));
}

// ---------------------------------------------------------------- file storage

TEST_F(FileTest, FileStorageWritesAndReadsData)
{
    writeFile(path_, Bytes{ 1, 2, 3 });

    EXPECT_EQ(readFile(path_), (Bytes{ 1, 2, 3 }));
}

TEST_F(FileTest, FileStorageWritesAndReadsEmptyData)
{
    ASSERT_EQ(file_storage_write(path_.c_str(), nullptr, 0), IPC_OK);

    EXPECT_TRUE(readFile(path_).empty());
}

TEST_F(FileTest, FileStorageFailsForMissingFile)
{
    unsigned char* data = nullptr;
    std::size_t size = 0;

    EXPECT_EQ(file_storage_read(path_.c_str(), 100, &data, &size), IPC_IO_ERROR);
}

TEST_F(FileTest, FileStorageRefusesOversizedFileBeforeReadingIt)
{
    writeFile(path_, Bytes(1000, 7));
    unsigned char* data = nullptr;
    std::size_t size = 0;

    EXPECT_EQ(file_storage_read(path_.c_str(), 999, &data, &size), IPC_CORRUPT_DATA);
    EXPECT_EQ(data, nullptr);
}

TEST_F(FileTest, FileStorageLeavesNoTemporaryFiles)
{
    for (int index = 0; index < 20; ++index)
        writeFile(path_, Bytes(static_cast<std::size_t>(index), 1));

    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory_))
        names.push_back(entry.path().filename().string());

    EXPECT_EQ(names, std::vector<std::string>{ "ipc.dat" });
}

// ---------------------------------------------------------------- opening and validation

TEST_F(FileTest, ChannelInitializesEmptyFile)
{
    Channel channel(path_);

    EXPECT_EQ(readFile(path_), channelFile(CHANNEL_EMPTY, "", 0));
}

TEST_F(FileTest, ChannelReopensAfterClose)
{
    {
        Channel channel(path_);
        ASSERT_EQ(channel.send("kept"), IPC_OK);
    }
    Channel reopened(path_);

    EXPECT_EQ(reopened.receive(), "kept");
}

TEST_F(FileTest, ChannelPreservesExistingReadyMessage)
{
    writeFile(path_, channelFile(CHANNEL_READY, "saved", 5));
    Channel channel(path_);

    EXPECT_EQ(channel.receive(), "saved");
}

TEST_F(FileTest, ChannelRejectsInvalidState)
{
    auto data = channelFile(CHANNEL_EMPTY, "", 0);
    data[0] = 254;
    writeFile(path_, data);
    ipc_channel* channel = nullptr;

    EXPECT_EQ(ipc_channel_open(path_.c_str(), &channel), IPC_CORRUPT_DATA);
    EXPECT_EQ(channel, nullptr);
}

TEST_F(FileTest, ChannelRejectsInvalidPayloadSize)
{
    writeFile(path_, channelFile(CHANNEL_READY, "short", 6));
    ipc_channel* channel = nullptr;

    EXPECT_EQ(ipc_channel_open(path_.c_str(), &channel), IPC_CORRUPT_DATA);
}

TEST_F(FileTest, ChannelRejectsTruncatedHeader)
{
    writeFile(path_, Bytes{ 0, 0 });
    ipc_channel* channel = nullptr;

    EXPECT_EQ(ipc_channel_open(path_.c_str(), &channel), IPC_CORRUPT_DATA);
}

TEST_F(FileTest, ChannelRejectsCorruptionWhileOpen)
{
    Channel channel(path_);
    writeFile(path_, channelFile(CHANNEL_READY, "truncated", 20));
    void* data = nullptr;
    std::size_t size = 0;

    EXPECT_EQ(ipc_channel_receive(channel.handle, &data, &size, 0), IPC_CORRUPT_DATA);
    EXPECT_EQ(channel.send("x", 0), IPC_CORRUPT_DATA);
}

TEST_F(FileTest, ChannelRecoversStaleWritingState)
{
    writeFile(path_, channelFile(CHANNEL_WRITING, "partial", 7));
    Channel channel(path_);

    EXPECT_EQ(readFile(path_), channelFile(CHANNEL_EMPTY, "", 0));
}

TEST_F(FileTest, ChannelRecoversStaleReadingState)
{
    writeFile(path_, channelFile(CHANNEL_READING, "delivered", 9));
    Channel channel(path_);

    EXPECT_EQ(readFile(path_), channelFile(CHANNEL_EMPTY, "", 0));
}

// A stale state that appears while channels are open, with no process having died holding
// the lock: an error on Windows (the mutex was not abandoned), recovered on POSIX (flock
// cannot tell, and a live lock holder never leaves these states).
TEST_F(FileTest, StaleStateWithoutAbandonedLockFollowsThePlatformRule)
{
    Channel channel(path_);
    writeFile(path_, channelFile(CHANNEL_WRITING, "stray", 5));

#ifdef _WIN32
    EXPECT_EQ(channel.send("next", 0), IPC_PROTOCOL_ERROR);
#else
    EXPECT_EQ(channel.send("next", 0), IPC_OK);
    EXPECT_EQ(channel.receive(), "next");
#endif
}

// Paths are UTF-8 on every platform: letters of several scripts that no single Windows
// code page holds.
TEST_F(FileTest, ChannelWorksWithUnicodePath)
{
    const auto directory = directory_ / std::filesystem::u8path(u8"ümläut-Кириллица-😀");
    std::filesystem::create_directories(directory);
    const std::string path = (directory / "ipc.dat").u8string();

    Channel channel(path);
    ASSERT_NE(channel.handle, nullptr);
    ASSERT_EQ(channel.send("unicode"), IPC_OK);
    EXPECT_EQ(channel.receive(), "unicode");
    EXPECT_TRUE(std::filesystem::exists(directory / "ipc.dat"));
}

// ---------------------------------------------------------------- messages and limits

TEST_F(FileTest, ChannelSendsAndReceivesEmptyMessage)
{
    Channel channel(path_);

    ASSERT_EQ(ipc_channel_send(channel.handle, nullptr, 0, 0), IPC_OK);
    EXPECT_EQ(channel.receive(), "");
}

TEST_F(FileTest, ChannelSendsAndReceivesLongMessage)
{
    Channel channel(path_);
    const std::string message(1024 * 1024, 'x');

    ASSERT_EQ(channel.send(message), IPC_OK);
    EXPECT_EQ(channel.receive(), message);
}

TEST_F(FileTest, ChannelCarriesArbitraryBytes)
{
    Channel channel(path_);
    std::string message(256, '\0');
    for (int index = 0; index < 256; ++index)
        message[static_cast<std::size_t>(index)] = static_cast<char>(index);

    ASSERT_EQ(channel.send(message), IPC_OK);
    EXPECT_EQ(channel.receive(), message);
}

TEST_F(FileTest, ChannelAcceptsTheMaximumPayload)
{
    Channel channel(path_);
    const std::string message(IPC_MAX_PAYLOAD_SIZE, 'm');

    ASSERT_EQ(channel.send(message), IPC_OK);
    EXPECT_EQ(channel.receive(), message);
}

TEST_F(FileTest, ChannelRejectsOversizedPayload)
{
    Channel channel(path_);
    const std::string message(IPC_MAX_PAYLOAD_SIZE + 1u, 'm');

    EXPECT_EQ(channel.send(message), IPC_PAYLOAD_TOO_LARGE);
    EXPECT_EQ(readFile(path_), channelFile(CHANNEL_EMPTY, "", 0));
}

TEST_F(FileTest, ChannelRejectsInvalidArguments)
{
    Channel channel(path_);
    void* data = nullptr;
    std::size_t size = 0;
    ipc_channel* none = nullptr;

    EXPECT_EQ(ipc_channel_open(nullptr, &none), IPC_INVALID_ARGUMENT);
    EXPECT_EQ(ipc_channel_open("", &none), IPC_INVALID_ARGUMENT);
    EXPECT_EQ(ipc_channel_send(nullptr, "x", 1, 0), IPC_INVALID_ARGUMENT);
    EXPECT_EQ(ipc_channel_send(channel.handle, nullptr, 1, 0), IPC_INVALID_ARGUMENT);
    EXPECT_EQ(ipc_channel_send(channel.handle, "x", 1, -2), IPC_INVALID_ARGUMENT);
    EXPECT_EQ(ipc_channel_receive(channel.handle, nullptr, &size, 0), IPC_INVALID_ARGUMENT);
    EXPECT_EQ(ipc_channel_receive(channel.handle, &data, nullptr, 0), IPC_INVALID_ARGUMENT);
    EXPECT_STREQ(ipc_status_string(IPC_TIMEOUT), "timed out");
    ipc_channel_close(nullptr);
    ipc_free(nullptr);
}

TEST_F(FileTest, ChannelLeavesNoTemporaryFiles)
{
    {
        Channel channel(path_);
        for (int index = 0; index < 10; ++index)
        {
            ASSERT_EQ(channel.send("message " + std::to_string(index)), IPC_OK);
            EXPECT_EQ(channel.receive(), "message " + std::to_string(index));
        }
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory_))
    {
        const std::string name = entry.path().filename().string();
        // The channel file, and on POSIX its lock file and notification FIFOs.
        EXPECT_TRUE(name == "ipc.dat" || name == "ipc.dat.lock" || name == "ipc.dat.data.fifo"
                    || name == "ipc.dat.space.fifo")
            << name;
    }
}

// ---------------------------------------------------------------- timeouts

TEST_F(FileTest, ReceiveTimesOutOnAnEmptyChannel)
{
    Channel channel(path_);
    void* data = nullptr;
    std::size_t size = 0;
    const auto start = Clock::now();

    EXPECT_EQ(ipc_channel_receive(channel.handle, &data, &size, 200), IPC_TIMEOUT);
    EXPECT_GE(millisecondsSince(start), 150);
    EXPECT_LT(millisecondsSince(start), 3000);
    EXPECT_EQ(ipc_channel_receive(channel.handle, &data, &size, 0), IPC_TIMEOUT);
}

TEST_F(FileTest, SendTimesOutOnAFullChannel)
{
    Channel channel(path_);
    ASSERT_EQ(channel.send("first"), IPC_OK);
    const auto start = Clock::now();

    EXPECT_EQ(channel.send("second", 200), IPC_TIMEOUT);
    EXPECT_GE(millisecondsSince(start), 150);
    EXPECT_EQ(channel.receive(), "first"); // the waiting message was not replaced
}

// ---------------------------------------------------------------- threads

TEST_F(FileTest, ReceiverWaitsForData)
{
    Channel receiver(path_);
    Channel sender(path_);
    std::promise<void> started;
    const auto ready = started.get_future();
    auto message = std::async(std::launch::async, [&receiver, &started]
    {
        started.set_value();
        return receiver.receive(IPC_WAIT_FOREVER);
    });

    ready.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_EQ(sender.send("receiver waiting"), IPC_OK);

    ASSERT_EQ(message.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(message.get(), "receiver waiting");
}

TEST_F(FileTest, SenderWaitsForAvailableSpace)
{
    Channel sender(path_);
    Channel receiver(path_);

    ASSERT_EQ(sender.send("first"), IPC_OK);

    auto result = std::async(std::launch::async, [&sender] { return sender.send("second", IPC_WAIT_FOREVER); });

    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
    EXPECT_EQ(receiver.receive(), "first");
    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(result.get(), IPC_OK);
    EXPECT_EQ(receiver.receive(), "second");
}

TEST_F(FileTest, RepeatedExchangePreservesMessageOrder)
{
    Channel sender(path_);
    Channel receiver(path_);
    auto sendResult = std::async(std::launch::async, [&sender]
    {
        for (int index = 0; index < 25; ++index)
            if (sender.send("message " + std::to_string(index)) != IPC_OK)
                return false;
        return true;
    });
    std::vector<std::string> messages;

    for (int index = 0; index < 25; ++index)
        messages.push_back(receiver.receive());

    ASSERT_EQ(sendResult.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(sendResult.get());
    for (int index = 0; index < 25; ++index)
        EXPECT_EQ(messages[static_cast<std::size_t>(index)], "message " + std::to_string(index));
}

TEST_F(FileTest, MultipleSendersDeliverEachMessageOnce)
{
    Channel receiver(path_);
    Channel firstSender(path_);
    Channel secondSender(path_);
    auto first = std::async(std::launch::async, [&firstSender] { return firstSender.send("first sender"); });
    auto second = std::async(std::launch::async, [&secondSender] { return secondSender.send("second sender"); });
    std::vector<std::string> messages{ receiver.receive(), receiver.receive() };

    ASSERT_EQ(first.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(second.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(first.get(), IPC_OK);
    EXPECT_EQ(second.get(), IPC_OK);
    std::sort(messages.begin(), messages.end());

    EXPECT_EQ(messages[0], "first sender");
    EXPECT_EQ(messages[1], "second sender");
}

TEST_F(FileTest, MultipleReceiversDeliverEachMessageOnce)
{
    Channel sender(path_);
    Channel firstReceiver(path_);
    Channel secondReceiver(path_);
    auto first = std::async(std::launch::async, [&firstReceiver] { return firstReceiver.receive(); });
    auto second = std::async(std::launch::async, [&secondReceiver] { return secondReceiver.receive(); });

    ASSERT_EQ(sender.send("first message"), IPC_OK);
    ASSERT_EQ(sender.send("second message"), IPC_OK);

    ASSERT_EQ(first.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(second.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    std::vector<std::string> messages{ first.get(), second.get() };
    std::sort(messages.begin(), messages.end());

    EXPECT_EQ(messages[0], "first message");
    EXPECT_EQ(messages[1], "second message");
}

// ---------------------------------------------------------------- processes

TEST_F(FileTest, SenderProcessFirst)
{
    const ProcessResult sent = runHelper("send " + inQuotes(path_) + " \"from another process\"");

    ASSERT_EQ(sent.exitCode, 0);
    Channel receiver(path_);
    EXPECT_EQ(receiver.receive(), "from another process");
}

TEST_F(FileTest, ReceiverProcessFirst)
{
    Channel sender(path_);
    auto received = std::async(std::launch::async, [this] { return runHelper("receive " + inQuotes(path_) + " 10000"); });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_EQ(sender.send("to another process"), IPC_OK);

    ASSERT_EQ(received.wait_for(std::chrono::seconds(15)), std::future_status::ready);
    const ProcessResult result = received.get();
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.output, "to another process");
}

TEST_F(FileTest, SenderProcessesDeliverEachMessageOnce)
{
    Channel receiver(path_);
    std::vector<std::future<ProcessResult>> senders;
    for (int index = 0; index < 3; ++index)
        senders.push_back(std::async(std::launch::async, [this, index]
        {
            return runHelper("send " + inQuotes(path_) + " \"process " + std::to_string(index) + "\" 10000");
        }));

    std::vector<std::string> messages;
    for (int index = 0; index < 3; ++index)
        messages.push_back(receiver.receive(10000));

    for (auto& sender : senders)
    {
        ASSERT_EQ(sender.wait_for(std::chrono::seconds(15)), std::future_status::ready);
        EXPECT_EQ(sender.get().exitCode, 0);
    }
    std::sort(messages.begin(), messages.end());
    EXPECT_EQ(messages, (std::vector<std::string>{ "process 0", "process 1", "process 2" }));
}

TEST_F(FileTest, ReceiverProcessesDeliverEachMessageOnce)
{
    Channel sender(path_);
    auto first = std::async(std::launch::async, [this] { return runHelper("receive " + inQuotes(path_) + " 10000"); });
    auto second = std::async(std::launch::async, [this] { return runHelper("receive " + inQuotes(path_) + " 10000"); });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_EQ(sender.send("one", 10000), IPC_OK);
    ASSERT_EQ(sender.send("two", 10000), IPC_OK);

    ASSERT_EQ(first.wait_for(std::chrono::seconds(15)), std::future_status::ready);
    ASSERT_EQ(second.wait_for(std::chrono::seconds(15)), std::future_status::ready);
    std::vector<std::string> messages{ first.get().output, second.get().output };
    std::sort(messages.begin(), messages.end());
    EXPECT_EQ(messages, (std::vector<std::string>{ "one", "two" }));
}

// A sender that dies after publishing WRITING, holding the lock: its message is discarded,
// and the channel keeps working for everyone else.
TEST_F(FileTest, SenderDyingMidWriteLosesOnlyItsMessage)
{
    Channel channel(path_);

    EXPECT_EQ(runHelper("die-writing " + inQuotes(path_) + " \"never published\"").exitCode, 3);

    ASSERT_EQ(channel.send("after the crash"), IPC_OK);
    EXPECT_EQ(channel.receive(), "after the crash");
}

// A receiver that dies after marking the message READING: the message is dropped
// (at-most-once delivery), the channel is empty again and usable.
TEST_F(FileTest, ReceiverDyingMidReadDropsTheMessage)
{
    Channel channel(path_);
    ASSERT_EQ(channel.send("in delivery"), IPC_OK);

    EXPECT_EQ(runHelper("die-reading " + inQuotes(path_)).exitCode, 3);

    void* data = nullptr;
    std::size_t size = 0;
    EXPECT_EQ(ipc_channel_receive(channel.handle, &data, &size, 300), IPC_TIMEOUT);
    EXPECT_EQ(readFile(path_), channelFile(CHANNEL_EMPTY, "", 0));
    ASSERT_EQ(channel.send("next"), IPC_OK);
    EXPECT_EQ(channel.receive(), "next");
}

// A sender that dies after publishing READY but before notifying: the message is kept, and
// a receiver already waiting still gets it (waits re-check the channel at least once a
// second).
TEST_F(FileTest, SenderDyingBeforeNotifyingStillDelivers)
{
    Channel receiver(path_);
    auto received = std::async(std::launch::async, [&receiver] { return receiver.receive(10000); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(runHelper("die-ready " + inQuotes(path_) + " \"published\"").exitCode, 3);

    ASSERT_EQ(received.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(received.get(), "published");
}
