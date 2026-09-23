#include "FileChannel.h"
#include "Message.h"

#include <iostream>

int main()
{
    FileChannel channel("ipc.dat");

    try
    {
        const Message message = channel.recieve();

        std::cout << "Received: " << message.payload << '\n';
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to receive message: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
