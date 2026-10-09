#include <thread>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int* volatile nowhere = nullptr;

[[gnu::noinline]] static void fault() {
    *nowhere = 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "fault") == 0) {
        std::thread(fault).join();
    }

    printf("fault: none\n");

    return argc > 2 ? atoi(argv[2]) : 0;
}
