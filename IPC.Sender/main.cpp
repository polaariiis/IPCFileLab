//#include "FileChannel.h"
//#include "Message.h"
//
//#include <iostream>
//
//int main()
//{
//	FileChannel channel("ipc.dat");
//
//	Message  message{ "Hi from sender" };
//
//	try
//	{
//		channel.send(message);
//		std::cout << "Message is sent!\n";
//	}
//	catch (const std::exception& e)
//	{
//		std::cerr << "Failed to send message: " << e.what() << "\n";
//		return 1;
//	}
//
//	return 0;
//}
//

#include <iostream>
#include <string>
#include <windows.h>

#include "FileChannel.h"
#include "Message.h"

int main()
{
	FileChannel channel("ipc.dat");

	Message message{
		"Message from sender " +
		std::to_string(GetCurrentProcessId())
	};

	try
	{
		channel.send(message);

		std::cout << "Message sent successfully: "
			<< message.payload
			<< '\n';
	}
	catch (const std::exception& e)
	{
		std::cerr << "Failed to send message: "
			<< e.what()
			<< '\n';

		return 1;
	}

	return 0;
}