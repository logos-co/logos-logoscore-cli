// A child for the Windows unit tests, which have no fork(): `sleep-ms N`
// stays up N milliseconds and exits 0.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "sleep-ms") == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(argv[2])));
    return 0;
}
