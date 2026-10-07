#include "native.h"

#include "fiber.h"
#include "input.h"
#include "platform.h"

#include <std/dbg/insist.h>
#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/thr/runable.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct Desktop final: public FrameCallback, public WindowEvents, public InputSink, public Runable {
        Desktop();
        ~Desktop();
        bool frame(const WindowInfo& info) override;
        void close() override;
        void selection() override;
        void run() override;
        void command(const char* line);
        void key(const KeyInput& input) override;
        void text(const TextInput& input) override;
        void preedit(StringView text, i32 begin, i32 end) override;
        void pointerMotion(const PointerMotionInput& input) override;
        void pointerButton(const PointerButtonInput& input) override;
        void scroll(const ScrollInput& input) override;
        void focus(bool focused) override;
        void pointerPresence(bool present) override;
        void flush() override;

        ObjPool::Ref owner = ObjPool::fromMemory();
        Platform* platform;
        Window* window;
        MetalCanvas* canvas = nullptr;
        u32 color = 0x204060;
        int pipe = -1;
    };
}

Desktop::Desktop()
    : platform(Platform::create(*owner))
{
    window = platform->createWindow(*owner, {
        .appId = StringView(u8"plt-cocoa-desktop"),
        .title = StringView(u8"PLT desktop"),
        .width = 400,
        .height = 280,
        .minimumWidth = 160,
        .minimumHeight = 120,
        .decorations = true,
        .input = createFiberInputSink(*owner, *platform->scheduler(), *this),
        .events = this,
        .frame = this,
        .appName = StringView(u8"PLT E2E"),
    });
    const WindowInfo initial = window->info();
    STD_INSIST(initial.width == 400 && initial.height == 280);
    canvas = MetalCanvas::create(*owner, window->renderContext());
    pipe = ::open(getenv("PLT_COMMAND_PIPE"), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    STD_INSIST(pipe >= 0);
    platform->scheduler()->create(*owner, *this, 128 * 1024);
    window->requestShow();
}

Desktop::~Desktop() {
    ::close(pipe);
}

bool Desktop::frame(const WindowInfo& info) {
    if (canvas == nullptr || !canvas->paint(info, color)) {
        return false;
    }
    printf("FRAME %u %u %.3f %d %d %d %06x\n", info.width, info.height,
        info.contentScale, info.focused, info.maximized, info.fullscreen, color);
    return true;
}

void Desktop::selection() {
}

void Desktop::close() {
    puts("CLOSED");
    platform->stop();
}

void Desktop::run() {
    Buffer pending;
    for (;;) {
        platform->scheduler()->awaitReadable(pipe, 0);
        char data[512];
        const ssize_t count = read(pipe, data, sizeof(data));
        if (count <= 0) {
            continue;
        }
        for (ssize_t i = 0; i < count; ++i) {
            if (data[i] == '\n') {
                command(pending.cStr());
                pending.reset();
            } else {
                pending.append(data + i, 1);
            }
        }
    }
}

void Desktop::command(const char* line) {
    if (strcmp(line, "describe") == 0) {
        canvas->describe();
    } else if (strcmp(line, "resize") == 0) {
        window->requestMinimumSize(200, 160);
        window->requestResizeUnit(8, 16, 4, 6);
        window->requestResize(640, 360);
    } else if (strcmp(line, "title") == 0) {
        window->requestTitle(StringView(u8"Edited document"));
        window->requestAttention();
    } else if (strcmp(line, "move") == 0) {
        window->requestMove(80, 100);
    } else if (strcmp(line, "minimize") == 0) {
        window->requestIconify();
    } else if (strcmp(line, "restore") == 0) {
        window->requestRestore();
        window->requestFocus();
    } else if (strcmp(line, "maximize") == 0) {
        window->requestMaximized(true);
    } else if (strcmp(line, "unmaximize") == 0) {
        window->requestMaximized(false);
    } else if (strcmp(line, "fullscreen") == 0) {
        window->requestFullscreen(true);
    } else if (strcmp(line, "unfullscreen") == 0) {
        window->requestFullscreen(false);
    } else if (strncmp(line, "cursor ", 7) == 0) {
        window->requestPointerIcon(static_cast<PointerIcon>(atoi(line + 7)));
    } else if (strcmp(line, "caret") == 0) {
        window->requestTextInputRect(20, 40, 8, 18);
    } else if (strcmp(line, "copy") == 0 || strcmp(line, "primary") == 0) {
        Clipboard* clipboard = line[0] == 'c' ? window->secondary() : window->primary();
        Output* output = clipboard->write();
        const StringView payload(u8"plt clipboard: Привет 🌍");
        output->write(payload.data(), payload.length());
        output->finish();
        delete output;
        Input* input = clipboard->read();
        Buffer readback;
        input->readAll(readback);
        delete input;
        STD_INSIST(StringView(readback) == payload);
        color = 0xe0b040;
        printf("COPIED %s\n", readback.cStr());
    } else if (strcmp(line, "paste") == 0) {
        Input* input = window->secondary()->read();
        Buffer content;
        input->readAll(content);
        delete input;
        printf("PASTED %s\n", content.cStr());
        color = 0x40a060;
    } else if (strcmp(line, "abandon") == 0) {
        Output* output = window->secondary()->write();
        output->write("discard", 7);
        delete output;
    } else if (strcmp(line, "quit") == 0) {
        window->requestClose();
    } else {
        fprintf(stderr, "unknown command: %s\n", line);
        exit(1);
    }
    printf("COMMAND %s %d\n", line, window->inLiveResize());
    window->requestFrame();
}

void Desktop::key(const KeyInput& input) {
    printf("KEY %u %u %u %u\n", (u32)input.key, (u32)input.action, input.baseCodepoint, input.modifiers);
}

void Desktop::text(const TextInput& input) {
    printf("TEXT %u\n", input.codepoint);
    color = 0xc04040;
    window->requestFrame();
}

void Desktop::preedit(StringView text, i32 begin, i32 end) {
    printf("PREEDIT %zu %d %d\n", text.length(), begin, end);
}

void Desktop::pointerMotion(const PointerMotionInput& input) {
    printf("MOTION %d %d\n", input.pixelX, input.pixelY);
}

void Desktop::pointerButton(const PointerButtonInput& input) {
    printf("BUTTON %u %d\n", (u32)input.button, input.pressed);
    color = 0x8040a0;
    window->requestFrame();
}

void Desktop::scroll(const ScrollInput&) {
    puts("SCROLL");
}

void Desktop::focus(bool focused) {
    printf("FOCUS %d\n", focused);
}

void Desktop::pointerPresence(bool present) {
    printf("POINTER %d\n", present);
}

void Desktop::flush() {
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Desktop desktop;
    desktop.platform->run();
}
