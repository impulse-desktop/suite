#include "app.h"
#include "poller.h"
#include "loop_wake.h"

#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Worker final: public App, public TimerCallback {
        void paint(Canvas& canvas) override;
        void ready() override;
        static void* work(void* data);
        LoopWake* bell = nullptr;
        int progress = 0;
        int shown = 0;
    };
}

void Worker::paint(Canvas& c) {
    c.clear(shown == 20 ? 0x40a060 : 0x204060);
    c.text(20, 40, StringView(u8"Background worker"));
    c.rectangle(20, 100, shown * 15, 40, 0xe0b040);
}

void Worker::ready() {
    shown = __atomic_load_n(&progress, __ATOMIC_ACQUIRE);
    printf("WORK %d\n", shown);
    window->requestFrame();
}

void* Worker::work(void* data) {
    Worker& app = *static_cast<Worker*>(data);
    for (int i = 1; i <= 20; ++i) {
        usleep(20'000);
        __atomic_store_n(&app.progress, i, __ATOMIC_RELEASE);
        app.bell->signal();
    }
    return nullptr;
}

int main() {
    Worker app;
    app.open("plt-worker");
    app.bell = app.platform->createLoopWake(*app.owner, app);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, Worker::work, &app) != 0) {
        return 1;
    }
    app.run();
    pthread_join(thread, nullptr);
}
