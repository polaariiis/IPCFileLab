cmake_minimum_required(VERSION 3.20)

project(IPCFileLab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(IPC.Core STATIC
	IPC.Core/ChannelNotifier.cpp
	IPC.Core/ChannelSynchronizer.cpp
	IPC.Core/FileChannel.cpp
	IPC.Core/FileStorage.cpp
	IPC.Core/Serializer.cpp
)

target_include_directories(IPC.Core PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/IPC.Core
)

target_link_libraries(IPC.Core PUBLIC kernel32)

add_executable(IPC.Sender
	IPC.Sender/main.cpp
)

target_link_libraries(IPC.Sender PRIVATE IPC.Core)

add_executable(IPC.Receiver
	IPC.Receiver/main.cpp
)

target_link_libraries(IPC.Receiver PRIVATE IPC.Core)

include(FetchContent)

FetchContent_Declare(
	googletest
	URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(IPC.Tests
	Tests/IPC.Tests.cpp
)

target_link_libraries(IPC.Tests PRIVATE IPC.Core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(IPC.Tests)
