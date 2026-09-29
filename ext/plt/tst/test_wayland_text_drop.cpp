#include "test.h"

#include <std/ios/input.h>
#include <std/ptr/scoped.h>

#include <stdio.h>

namespace plt::test {
    namespace {
        // Exercises the raw session API: records hover positions, picks an
        // explicit mime over the canonical preference and can abandon the
        // payload stream after the first chunk.
        struct RawTarget final: DropTarget {
            DropReply dragOver(const DropOffer& offer, i32 x, i32 y) override {
                ++overCount;
                lastFormats = offer.formats();
                lastX = x;
                lastY = y;
                return {
                    .mime = stl::StringView(u8"text/plain;charset=utf-8"),
                    .action = DropAction::Copy,
                };
            }

            void dragLeft() override {
                ++leftCount;
            }

            void dropped(Drop& drop) override {
                ++droppedCount;
                const stl::ScopedPtr<stl::Input> stream{drop.read(stl::StringView(u8"text/plain;charset=utf-8"))};
                for (;;) {
                    u8 chunk[4096];
                    const size_t count = stream->read(chunk, sizeof(chunk));
                    if (count == 0) {
                        break;
                    }
                    if (abortNext) {
                        // Destruction before end of stream abandons the offer.
                        break;
                    }
                    content.append(chunk, count);
                }
                abortNext = false;
            }

            stl::Buffer content;
            size_t lastFormats = 0;
            i32 lastX = 0;
            i32 lastY = 0;
            u32 overCount = 0;
            u32 leftCount = 0;
            u32 droppedCount = 0;
            bool abortNext = false;
        };

        // The canonical consumer shape: prefers a file list over text,
        // buffers one payload whole and splits uri lists into entries.
        struct BufferingTarget final: DropTarget {
            stl::StringView preferred(const DropOffer& offer) const {
                bool uriList = false;
                bool utf8 = false;
                bool utf8String = false;
                bool plain = false;
                for (size_t index = 0; index != offer.formats(); ++index) {
                    const stl::StringView mime = offer.format(index);
                    uriList = uriList || mime == stl::StringView(u8"text/uri-list");
                    utf8 = utf8 || mime == stl::StringView(u8"text/plain;charset=utf-8");
                    utf8String = utf8String || mime == stl::StringView(u8"UTF8_STRING");
                    plain = plain || mime == stl::StringView(u8"text/plain");
                }
                if (uriList) {
                    return stl::StringView(u8"text/uri-list");
                }
                if (utf8) {
                    return stl::StringView(u8"text/plain;charset=utf-8");
                }
                if (utf8String) {
                    return stl::StringView(u8"UTF8_STRING");
                }
                if (plain) {
                    return stl::StringView(u8"text/plain");
                }
                return {};
            }

            DropReply dragOver(const DropOffer& offer, i32, i32) override {
                return {
                    .mime = preferred(offer),
                    .action = DropAction::Copy,
                };
            }

            void dragLeft() override {
            }

            void dropped(Drop& drop) override {
                const stl::StringView mime = preferred(*drop.what());
                if (mime.empty()) {
                    return;
                }
                const stl::ScopedPtr<stl::Input> stream{drop.read(mime)};
                stl::Buffer content;
                for (;;) {
                    u8 chunk[4096];
                    const size_t count = stream->read(chunk, sizeof(chunk));
                    if (count == 0) {
                        break;
                    }
                    content.append(chunk, count);
                }
                if (!(mime == stl::StringView(u8"text/uri-list"))) {
                    text.append(content.data(), content.length());
                    ++dropCount;
                    return;
                }
                stl::StringView payload(content);
                stl::StringView entry;
                stl::Buffer path;
                while (nextUriListEntry(payload, entry)) {
                    path.reset();
                    if (fileUriToPath(entry, path)) {
                        paths.append(path.data(), path.length());
                    } else {
                        paths.append(entry.data(), entry.length());
                    }
                    paths.append("\n", 1);
                    ++pathCount;
                }
            }

            stl::Buffer text;
            stl::Buffer paths;
            u32 dropCount = 0;
            u32 pathCount = 0;
        };

        struct RejectingTarget final: DropTarget {
            DropReply dragOver(const DropOffer&, i32, i32) override {
                ++overCount;
                return {};
            }

            void dragLeft() override {
            }

            void dropped(Drop&) override {
                ++droppedCount;
            }

            u32 overCount = 0;
            u32 droppedCount = 0;
        };
    }

    bool textDrop(int fd) {
        InputRecorder input;
        BufferingTarget target;
        Client client(fd, 800, 1, nullptr, &input, true, nullptr, &target);
        if (command(fd, Command::DragEnter).count != 1) {
            fprintf(stderr, "text drop: drag enter was not delivered\n");
            return false;
        }
        pump(*client.platform);

        const Reply accept = command(fd, Command::QueryDragAccept);
        if (accept.count != 1 || accept.first != 1 || accept.second != 1) {
            fprintf(
                stderr,
                "text drop: offer was not accepted with the utf-8 mime and the copy action\n"
            );
            return false;
        }

        command(fd, Command::DragDrop);
        pump(*client.platform);
        if (command(fd, Command::DragData).count != 1) {
            fprintf(stderr, "text drop: no transfer fd was available after drop\n");
            return false;
        }
        pump(*client.platform);

        if (target.dropCount != 1 || stl::StringView(target.text) != stl::StringView(u8"hermetic Wayland drop")) {
            fprintf(stderr, "text drop: payload was not delivered to the target\n");
            return false;
        }
        const Reply finish = command(fd, Command::QueryDragFinish);
        if (finish.count != 1 || finish.first != 1) {
            fprintf(stderr, "text drop: the offer was not received and finished as utf-8\n");
            return false;
        }
        return true;
    }

    bool utf8StringDrop(int fd) {
        InputRecorder input;
        BufferingTarget target;
        Client client(fd, 800, 1, nullptr, &input, true, nullptr, &target);
        if (command(fd, Command::DragEnterUtf8String).count != 1) {
            fprintf(stderr, "UTF8_STRING drop: drag enter was not delivered\n");
            return false;
        }
        pump(*client.platform);

        // The source offers only UTF8_STRING; accept and receive must name
        // that exact mime instead of a preferred spelling.
        const Reply accept = command(fd, Command::QueryDragAccept);
        if (accept.count != 1 || accept.first != 3) {
            fprintf(stderr, "UTF8_STRING drop: offer was not accepted as UTF8_STRING\n");
            return false;
        }

        command(fd, Command::DragDrop);
        pump(*client.platform);
        if (command(fd, Command::DragData).count != 1) {
            fprintf(stderr, "UTF8_STRING drop: no transfer fd was available after drop\n");
            return false;
        }
        pump(*client.platform);

        if (target.dropCount != 1 || stl::StringView(target.text) != stl::StringView(u8"hermetic Wayland drop")) {
            fprintf(stderr, "UTF8_STRING drop: payload was not delivered to the target\n");
            return false;
        }
        const Reply finish = command(fd, Command::QueryDragFinish);
        if (finish.count != 1 || finish.first != 3) {
            fprintf(stderr, "UTF8_STRING drop: the offer was not received as UTF8_STRING\n");
            return false;
        }
        return true;
    }

    bool uriListDrop(int fd) {
        InputRecorder input;
        BufferingTarget target;
        Client client(fd, 800, 1, nullptr, &input, true, nullptr, &target);
        if (command(fd, Command::DragEnterUriList).count != 1) {
            fprintf(stderr, "uri-list drop: drag enter was not delivered\n");
            return false;
        }
        pump(*client.platform);

        // Both text and uri-list are on offer; the canonical target must
        // prefer the file list.
        const Reply accept = command(fd, Command::QueryDragAccept);
        if (accept.count != 1 || accept.first != 4) {
            fprintf(stderr, "uri-list drop: offer was not accepted as text/uri-list\n");
            return false;
        }

        command(fd, Command::DragDrop);
        pump(*client.platform);
        if (command(fd, Command::DragUriData).count != 1) {
            fprintf(stderr, "uri-list drop: no transfer fd was available after drop\n");
            return false;
        }
        pump(*client.platform);

        // The file URI decodes to a local path, the comment is skipped and
        // the foreign scheme passes through verbatim.
        if (target.pathCount != 2 || target.dropCount != 0 || stl::StringView(target.paths) != stl::StringView(u8"/tmp/plt drop.txt\nhttps://example.com/plt\n")) {
            fprintf(stderr, "uri-list drop: entries were not delivered as paths\n");
            return false;
        }
        const Reply finish = command(fd, Command::QueryDragFinish);
        if (finish.count != 1 || finish.first != 4) {
            fprintf(stderr, "uri-list drop: the offer was not received as text/uri-list\n");
            return false;
        }
        return true;
    }

    bool rawDropApi(int fd) {
        RawTarget target;
        Client client(fd, 800, 1, nullptr, nullptr, true, nullptr, &target);
        command(fd, Command::DragEnterUriList);
        pump(*client.platform);
        if (target.overCount != 1 || target.lastFormats != 2 || target.lastX != 15 || target.lastY != 25) {
            fprintf(stderr, "raw drop API: enter did not reach the target intact\n");
            return false;
        }

        // The target picks utf-8 over the offered uri-list: the consumer's
        // choice must win over any built-in preference.
        Reply accept = command(fd, Command::QueryDragAccept);
        if (accept.count != 1 || accept.first != 1) {
            fprintf(stderr, "raw drop API: the chosen mime was not accepted\n");
            return false;
        }

        command(fd, Command::DragMotion);
        pump(*client.platform);
        accept = command(fd, Command::QueryDragAccept);
        if (target.overCount != 2 || target.lastX != 30 || target.lastY != 35 || accept.count != 1) {
            fprintf(stderr, "raw drop API: motion re-sent an unchanged reply\n");
            return false;
        }

        // First drop: the stream is aborted from data(), so the offer must
        // be destroyed without finish.
        target.abortNext = true;
        command(fd, Command::DragDrop);
        pump(*client.platform);
        command(fd, Command::DragData);
        pump(*client.platform);
        if (target.droppedCount != 1 || !target.content.empty()) {
            fprintf(stderr, "raw drop API: aborted transfer delivered a payload\n");
            return false;
        }
        Reply finish = command(fd, Command::QueryDragFinish);
        if (finish.count != 0) {
            fprintf(stderr, "raw drop API: an aborted transfer was finished\n");
            return false;
        }

        // Second session completes normally.
        command(fd, Command::DragEnterUriList);
        pump(*client.platform);
        command(fd, Command::DragDrop);
        pump(*client.platform);
        command(fd, Command::DragData);
        pump(*client.platform);
        finish = command(fd, Command::QueryDragFinish);
        if (target.droppedCount != 2 || finish.count != 1 || stl::StringView(target.content) != stl::StringView(u8"hermetic Wayland drop")) {
            fprintf(stderr, "raw drop API: second transfer did not complete\n");
            return false;
        }
        return true;
    }

    bool rejectedDrag(int fd) {
        RejectingTarget target;
        Client client(fd, 800, 1, nullptr, nullptr, true, nullptr, &target);
        command(fd, Command::DragEnter);
        pump(*client.platform);
        const Reply accept = command(fd, Command::QueryDragAccept);
        if (target.overCount != 1 || accept.count != 1 || accept.first != 0 || accept.second != 0) {
            fprintf(stderr, "rejected drag: the rejection did not reach the source\n");
            return false;
        }

        command(fd, Command::DragDrop);
        pump(*client.platform);
        if (target.droppedCount != 1 || command(fd, Command::DragData).count != 0) {
            fprintf(stderr, "rejected drag: an unread drop started a transfer\n");
            return false;
        }
        return true;
    }

    bool cancelledDrag(int fd) {
        RawTarget target;
        Client client(fd, 800, 1, nullptr, nullptr, true, nullptr, &target);
        command(fd, Command::DragEnter);
        pump(*client.platform);
        command(fd, Command::DragLeave);
        pump(*client.platform);
        if (target.leftCount != 1) {
            fprintf(stderr, "cancelled drag: leave did not reach the target\n");
            return false;
        }

        // A drop after leave belongs to no session; nothing may be received
        // or delivered.
        command(fd, Command::DragDrop);
        pump(*client.platform);
        if (target.droppedCount != 0 || command(fd, Command::DragData).count != 0) {
            fprintf(stderr, "cancelled drag: a transfer was started after leave\n");
            return false;
        }
        return true;
    }
}
