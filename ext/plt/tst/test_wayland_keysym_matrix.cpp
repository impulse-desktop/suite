#include "test.h"

#include <stdio.h>

namespace plt::test {
    namespace {
        // Records the InputKey of every press, in order; everything else
        // is noise for this scenario.
        struct KeySequenceSink final: InputSink {
            void key(const KeyInput& input) override {
                if (input.action != InputAction::Press) {
                    ++releaseCount;
                    return;
                }
                if (pressCount < sizeof(keys) / sizeof(keys[0])) {
                    keys[pressCount] = input.key;
                }
                ++pressCount;
            }

            void text(const TextInput&) override {
            }

            void preedit(stl::StringView, i32, i32) override {
            }

            void pointerMotion(const PointerMotionInput&) override {
            }

            void pointerButton(const PointerButtonInput&) override {
            }

            void scroll(const ScrollInput&) override {
            }

            void focus(bool) override {
            }

            void pointerPresence(bool) override {
            }

            void flush() override {
            }

            InputKey keys[160]{};
            u32 pressCount = 0;
            u32 releaseCount = 0;
        };

        bool runSweep(int fd, Platform& platform, KeySequenceSink& input, Command sweep, const KeysymSweepKey* table, u32 count, const char* name) {
            input.pressCount = 0;
            input.releaseCount = 0;
            const Reply reply = command(fd, sweep);
            if (reply.count != count) {
                fprintf(stderr, "keysym matrix: %s delivered %u of %u keys\n", name, reply.count, count);
                return false;
            }
            for (u32 attempt = 0; attempt != 50 && input.pressCount < count; ++attempt) {
                pump(platform);
            }
            if (input.pressCount != count || input.releaseCount != count) {
                fprintf(stderr, "keysym matrix: %s saw %u presses, %u releases of %u\n", name, input.pressCount, input.releaseCount, count);
                return false;
            }
            for (u32 at = 0; at < count; ++at) {
                if (table[at].checked && input.keys[at] != table[at].key) {
                    fprintf(stderr, "keysym matrix: %s keycode %u translated to %u, expected %u\n", name, table[at].keycode, (u32)(input.keys[at]), (u32)(table[at].key));
                    return false;
                }
            }
            return true;
        }
    }

    bool keysymMatrix(int fd) {
        KeySequenceSink input;
        EventSink events;
        Client client(fd, 800, 1, nullptr, &input, true, &events);
        command(fd, Command::KeyboardEnter);
        pump(*client.platform);

        if (!runSweep(fd, *client.platform, input, Command::KeyboardKeysymSweep, keysymSweepKeys, sizeof(keysymSweepKeys) / sizeof(keysymSweepKeys[0]), "sweep")) {
            return false;
        }

        // The same pad keys mean digits once NumLock is locked; the
        // modifiers event drives the client's xkb state.
        const Reply numLock = command(fd, Command::KeyboardNumLock);
        if (numLock.count == 0) {
            fprintf(stderr, "keysym matrix: keymap has no NumLock modifier\n");
            return false;
        }
        pump(*client.platform);
        return runSweep(fd, *client.platform, input, Command::KeyboardNumpadSweep, keysymNumpadKeys, sizeof(keysymNumpadKeys) / sizeof(keysymNumpadKeys[0]), "numpad");
    }
}
