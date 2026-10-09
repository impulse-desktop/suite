#include <thread>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

[[gnu::noinline]] static void fault() {
    size_t size = 1 << 16;
    void* page = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    munmap(page, size);
    *(volatile int*)page = 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "fault") == 0) {
        std::thread(fault).join();
    }

    printf("fault: none\n");

    return argc > 2 ? atoi(argv[2]) : 0;
}
