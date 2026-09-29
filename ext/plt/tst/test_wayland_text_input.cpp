#include "test.h"

#include <std/str/view.h>

#include <stdio.h>

namespace plt::test {
    bool textInput(int fd) {
        InputRecorder input;
        Client client(fd, 800, 1, nullptr, &input);
        command(fd, Command::KeyboardEnter);
        pump(*client.platform);
        command(fd, Command::TextInputEnter);
        pump(*client.platform);
        const Reply enabled = command(fd, Command::QueryTextInput);
        // zwp_text_input_v3 content purpose "terminal" is 13.
        if (enabled.count == 0 || enabled.first != 1 || enabled.second != 13) {
            fprintf(
                stderr,
                "text input: enable failed, commits=%u enabled=%d purpose=%d\n",
                enabled.count,
                enabled.first,
                enabled.second
            );
            return false;
        }

        client.window->requestTextInputRect(8, 16, 10, 20);
        pump(*client.platform);
        const Reply rect = command(fd, Command::QueryTextInputRect);
        if (rect.first != 8 || rect.second != 16 || rect.count != ((10u << 16) | 20u)) {
            fprintf(
                stderr,
                "text input: cursor rectangle mismatch, x=%d y=%d packed=%u\n",
                rect.first,
                rect.second,
                rect.count
            );
            return false;
        }

        command(fd, Command::TextInputPreedit);
        pump(*client.platform);
        if (input.preeditCount != 1
            || stl::StringView(input.lastPreedit) != stl::StringView(u8"ni")
            || input.lastPreeditCursorBegin != 0
            || input.lastPreeditCursorEnd != 2) {
            fprintf(stderr, "text input: preedit was not delivered\n");
            return false;
        }

        command(fd, Command::TextInputCommitString);
        pump(*client.platform);
        if (input.textCount != 1 || input.lastText.codepoint != 0xe9) {
            fprintf(
                stderr,
                "text input: commit string was not delivered, texts=%u codepoint=%x\n",
                input.textCount,
                input.lastText.codepoint
            );
            return false;
        }
        if (input.preeditCount != 2 || !input.lastPreedit.empty()) {
            fprintf(stderr, "text input: commit did not clear the preedit preview\n");
            return false;
        }

        command(fd, Command::TextInputCommitInvalid);
        pump(*client.platform);
        if (input.textCount != 2 || input.lastText.codepoint != 'A') {
            fprintf(
                stderr,
                "text input: invalid UTF-8 leaked into commit, texts=%u codepoint=%x\n",
                input.textCount,
                input.lastText.codepoint
            );
            return false;
        }

        command(fd, Command::TextInputPreedit);
        pump(*client.platform);
        if (input.lastPreedit.empty()) {
            fprintf(stderr, "text input: preedit before leave missing\n");
            return false;
        }
        // Focus leaving the text input clears the preview and every
        // pending part.
        command(fd, Command::TextInputLeave);
        pump(*client.platform);
        if (!input.lastPreedit.empty()) {
            fprintf(stderr, "text input: leave left a stale preedit preview\n");
            return false;
        }

        command(fd, Command::TextInputEnter);
        pump(*client.platform);
        command(fd, Command::TextInputPreedit);
        pump(*client.platform);
        if (input.lastPreedit.empty()) {
            fprintf(stderr, "text input: preedit before seat removal missing\n");
            return false;
        }
        command(fd, Command::RemoveSeat);
        pump(*client.platform);
        if (!input.lastPreedit.empty()) {
            fprintf(stderr, "text input: seat removal left a stale preedit preview\n");
            return false;
        }
        return true;
    }
}
