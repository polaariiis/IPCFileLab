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
