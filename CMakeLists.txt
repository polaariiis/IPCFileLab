cmake_minimum_required(VERSION 3.20)

project(IPCFileLab LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# When another project adds IPCFileLab (add_subdirectory / FetchContent), only the library
# is built; the console programs and the tests are for this repository.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(IPC_TOP_LEVEL ON)
else()
	set(IPC_TOP_LEVEL OFF)
endif()
option(IPC_BUILD_PROGRAMS "Build IPC.Sender and IPC.Receiver" ${IPC_TOP_LEVEL})
option(IPC_BUILD_TESTS "Build IPC.Tests (downloads GoogleTest)" ${IPC_TOP_LEVEL})
option(IPC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(ipc_warnings target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 $<$<BOOL:${IPC_WARNINGS_AS_ERRORS}>:/WX>)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
			$<$<BOOL:${IPC_WARNINGS_AS_ERRORS}>:-Werror>)
	endif()
endfunction()

# ---------------------------------------------------------------- library

if(WIN32)
	set(IPC_PLATFORM_SOURCE IPC.Core/platform_win32.c)
else()
	set(IPC_PLATFORM_SOURCE IPC.Core/platform_posix.c)
endif()

add_library(IPC.Core STATIC
	IPC.Core/ipc.h
	IPC.Core/channel.c
	IPC.Core/file_storage.c
	IPC.Core/file_storage.h
	IPC.Core/platform.h
	${IPC_PLATFORM_SOURCE}
	IPC.Core/protocol.c
	IPC.Core/protocol.h
)
add_library(IPCFileLab::IPC ALIAS IPC.Core)

target_include_directories(IPC.Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/IPC.Core)
if(MSVC)
	target_compile_definitions(IPC.Core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
ipc_warnings(IPC.Core)

# ---------------------------------------------------------------- programs

if(IPC_BUILD_PROGRAMS)
	add_executable(IPC.Sender IPC.Sender/main.c)
	target_link_libraries(IPC.Sender PRIVATE IPC.Core)
	ipc_warnings(IPC.Sender)

	add_executable(IPC.Receiver IPC.Receiver/main.c)
	target_link_libraries(IPC.Receiver PRIVATE IPC.Core)
	ipc_warnings(IPC.Receiver)
endif()

# ---------------------------------------------------------------- tests

if(IPC_BUILD_TESTS)
	enable_language(CXX)
	set(CMAKE_CXX_STANDARD 17)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	set(CMAKE_CXX_EXTENSIONS OFF)

	include(FetchContent)
	FetchContent_Declare(
		googletest
		URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	)
	set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
	FetchContent_MakeAvailable(googletest)

	enable_testing()

	# A separate program the tests start as real processes (sender, receiver, and processes
	# that die in the middle of an operation).
	add_executable(IPC.TestHelper Tests/ipc_test_helper.c)
	target_link_libraries(IPC.TestHelper PRIVATE IPC.Core)
	if(MSVC)
		target_compile_definitions(IPC.TestHelper PRIVATE _CRT_SECURE_NO_WARNINGS)
	endif()
	ipc_warnings(IPC.TestHelper)

	add_executable(IPC.Tests Tests/IPC.Tests.cpp)
	target_link_libraries(IPC.Tests PRIVATE IPC.Core GTest::gtest_main)
	target_compile_definitions(IPC.Tests PRIVATE
		IPC_TEST_HELPER="$<TARGET_FILE:IPC.TestHelper>"
		$<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS>)
	add_dependencies(IPC.Tests IPC.TestHelper)

	include(GoogleTest)
	gtest_discover_tests(IPC.Tests DISCOVERY_TIMEOUT 60)
endif()
