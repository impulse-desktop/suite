#include "app.h"
#include "fiber.h"
#include "mutex.h"
#include "poller.h"

#include <std/dbg/insist.h>
#include <std/thr/runable.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Transfer;

    struct Sender final: public Runable {
        void run() override;
        Transfer* app = nullptr;
        const char* path = nullptr;
    };

    struct Transfer final: public App, public Runable, public TimerCallback {
        ~Transfer();
        void start();
        void run() override;
        void ready() override;
        void paint(Canvas& canvas) override;
        int sockets[2] = {-1, -1};
        int output = -1;
        FiberMutex* lock = nullptr;
        Sender senders[2];
        int sent = 0;
        size_t received = 0;
        bool complete = false;
    };
}

Transfer::~Transfer() {
    for (int fd : sockets) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    if (output >= 0) {
        ::close(output);
    }
}

void Transfer::start() {
    STD_INSIST(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0);
    const int capacity = 4096;
    STD_INSIST(setsockopt(sockets[1], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0);
    output = ::open(setting("PLT_TRANSFER_OUTPUT", ""), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    STD_INSIST(output >= 0);
    lock = platform->scheduler()->createMutex(*owner);
    senders[0].app = senders[1].app = this;
    senders[0].path = setting("PLT_TRANSFER_FIRST", "");
    senders[1].path = setting("PLT_TRANSFER_SECOND", "");
    platform->scheduler()->create(*owner, *this, 128 * 1024);
    platform->poller()->timeout(20'000, *this);
}

void Transfer::ready() {
    for (auto& sender : senders) {
        platform->scheduler()->create(*owner, sender, 128 * 1024);
    }
}

void Sender::run() {
    const int input = ::open(path, O_RDONLY | O_CLOEXEC);
    STD_INSIST(input >= 0);
    if (!app->lock->tryLock()) {
        puts("WAITING FOR TRANSACTION");
        app->lock->lock();
    }
    STD_INSIST(app->lock->locked() && app->lock->heldByCurrent());
    char bytes[16384];
    for (;;) {
        const ssize_t count = read(input, bytes, sizeof(bytes));
        STD_INSIST(count >= 0);
        if (count == 0) {
            break;
        }
        ssize_t offset = 0;
        while (offset < count) {
            const ssize_t written = write(app->sockets[1], bytes + offset, count - offset);
            if (written < 0) {
                STD_INSIST(errno == EAGAIN || errno == EWOULDBLOCK);
                STD_INSIST(app->platform->scheduler()->awaitWritable(app->sockets[1], 2'000'000));
            } else {
                offset += written;
            }
        }
        app->platform->scheduler()->yield();
    }
    ::close(input);
    ++app->sent;
    app->lock->unlock();
    if (app->sent == 2 && app->sockets[1] >= 0) {
        ::close(app->sockets[1]);
        app->sockets[1] = -1;
    }
}

void Transfer::run() {
    STD_INSIST(!platform->scheduler()->awaitReadable(sockets[0], 1000));
    puts("IDLE TIMEOUT");
    for (;;) {
        STD_INSIST(platform->scheduler()->awaitReadable(sockets[0], 2'000'000));
        char bytes[8192];
        const ssize_t count = read(sockets[0], bytes, sizeof(bytes));
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        STD_INSIST(count >= 0);
        if (count == 0) {
            break;
        }
        STD_INSIST(write(output, bytes, count) == count);
        received += count;
        window->requestFrame();
    }
    STD_INSIST(fsync(output) == 0);
    complete = true;
    printf("TRANSFERRED %zu\n", received);
    window->requestFrame();
}

void Transfer::paint(Canvas& c) {
    c.clear(complete ? 0x40a060 : 0x204060);
    c.text(20, 40, StringView(u8"Two documents, one transaction stream"));
}

int main() {
    Transfer app;
    app.open("plt-file-transfer");
    app.start();
    app.App::run();
}
