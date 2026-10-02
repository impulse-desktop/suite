#include "play.h"

#include "ui.h"
#include "error.h"
#include "pooled.h"

#include <std/ios/sys.h>
#include <std/sys/crt.h>
#include <std/alg/defer.h>
#include <std/sys/throw.h>
#include <std/lib/vector.h>
#include <std/ptr/scoped.h>
#include <std/thr/thread.h>
#include <std/str/builder.h>
#include <std/thr/channel.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <AL/al.h>
#include <imgui.h>
#include <AL/alc.h>
#include <string.h>
#include <unistd.h>
#include <AL/alext.h>
#include <sys/mman.h>
#include <plt/fiber.h>
#include <plt/poller.h>
#include <video_codes.h>
#include <plt/platform.h>
#include <plt/loop_wake.h>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

using namespace stl;

namespace {
    constexpr Design windowWidth = 960_d;
    constexpr Design windowHeight = 600_d;
    constexpr Design barPadding = 8_d;
    constexpr Design buttonWidth = 72_d;
    constexpr size_t framePermits = 10;
    constexpr size_t planeAlignment = 64;
    constexpr size_t channelCapacity = 1024;
    constexpr size_t threadStack = 8u << 20;
    constexpr size_t controllerStack = 8u << 20;
    constexpr int audioBuffers = 6;
    constexpr int bufferRate = 20;
    constexpr size_t sampleBytes = 4;
    constexpr double seekStep = 10.;

    enum class Kind : u8 {
        Control,
        Stop,
        Surface,
        Pulse,
        Frame,
        End,
        Clock,
        Failure
    };

    struct Message {
        virtual ~Message() noexcept;
        virtual Kind messageKind() const = 0;
    };

    template <Kind K>
    struct Typed: public Message {
        static constexpr Kind kind = K;
        Kind messageKind() const override;
    };

    template <typename T>
    T* cast(Message* message) {
        return message->messageKind() == T::kind ? static_cast<T*>(message) : nullptr;
    }

    struct Player;

    struct VideoImage final: public Runable {
        Player* player;
        AVFrame* frame;
        ObjPool* pool = nullptr;
        RenderImage* render = nullptr;
        u32 draws = 0;

        explicit VideoImage(Player* player);
        void run() override;
        void forget();
    };

    struct FrameUniform {
        u32 planeWord[4];
        u32 lineWords[4];
        u32 size[4];
        u32 tags[4];
        u32 sites[4];
        float white[4];
    };

    struct FrameShader {
        const char* layout = nullptr;
        const char* color = nullptr;
        const char* transfer = nullptr;
        FrameUniform uniform = {};
    };

    struct CompiledShader {
        const VideoShaderCode* code;
        RenderShader* shader;
    };

    struct Control final: public Typed<Kind::Control> {
        u64 generation;
        double position;
        bool playing;

        Control(u64 generation, double position, bool playing);
    };

    struct Stop final: public Typed<Kind::Stop> {};

    struct Surface final: public Typed<Kind::Surface> {
        VideoImage* image;

        explicit Surface(VideoImage* image);
    };

    struct Pulse final: public Typed<Kind::Pulse> {};

    struct Frame final: public Typed<Kind::Frame> {
        VideoImage* image;
        u64 generation;
        double pts;
        double aspect;

        Frame(VideoImage* image, u64 generation, double pts, double aspect);
    };

    struct End final: public Typed<Kind::End> {
        u64 generation;

        explicit End(u64 generation);
    };

    struct Clock final: public Typed<Kind::Clock> {
        u64 generation;
        double position;
        u64 at;
        bool running;
        bool ended;

        Clock(u64 generation, double position, u64 at, bool running, bool ended);
    };

    struct Failure final: public Typed<Kind::Failure> {
        Buffer text;

        explicit Failure(StringView text);
    };

    struct Stream {
        AVFormatContext* format = nullptr;
        AVCodecContext* codec = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        int index = -1;
        double start = 0.;
        double duration = 0.;
        bool eof = false;

        Stream(Player* player, AVMediaType type, void* slots = nullptr);
        void seek(double position);
        int receive();
        double seconds(i64 timestamp) const;
        double timeBase() const;
    };

    struct Slots {
        AVBufferPool* pool = nullptr;
        size_t size = 0;

        void fill(AVFrame* frame, int width, int height, const int* align);
    };

    struct Video final: public Runable {
        Player* player;
        Slots slots;
        Stream stream;
        Vector<VideoImage*> idle;
        AVFrame* last;
        u64 generation = 1;
        double target = 0.;
        double pts = 0.;
        double lastPts = 0.;
        double next = 0.;
        bool decoded = false;
        bool ended = false;

        explicit Video(Player* player);
        void run() override;
        void apply(const Control& control);
        bool step();
        bool deliver();
        void copy(AVFrame* to, const AVFrame* from);
    };

    struct Audio final: public Runable {
        Player* player;
        Stream stream;
        ALCdevice* device = nullptr;
        ALCcontext* context = nullptr;
        ALuint source = 0;
        ALuint buffers[audioBuffers] = {};
        LPALEVENTCALLBACKSOFT eventCallback = nullptr;
        LPALGETSOURCEDVSOFT sourceOffsets = nullptr;
        SwrContext* resampler = nullptr;
        AVChannelLayout layout{};
        int format = -1;
        int inputRate = 0;
        int rate = 0;
        Vector<ALuint> idle;
        size_t ring[audioBuffers] = {};
        size_t ringHead = 0;
        size_t ringLength = 0;
        Buffer pcm;
        double pcmStart = 0.;
        double queuedStart = 0.;
        u64 generation = 1;
        double target = 0.;
        bool playing = true;
        bool skipping = true;
        bool drained = false;
        bool ended = false;

        explicit Audio(Player* player);
        void run() override;
        void apply(const Control& control);
        void service();
        bool step();
        void convert();
        void drop(size_t taken);
        void enqueue();
        void start();
        void finish();
        void report();
        ALint state();
        size_t samples() const;
        size_t bufferSamples() const;
    };

    struct Screen final: public Runable {
        Player* player;
        plt::Fiber* fiber = nullptr;
        bool hasVideo;
        bool hasAudio;
        double duration;
        const char* output = "sdr";
        Vector<CompiledShader> compiled;
        int shownWidth = 0;
        int shownHeight = 0;
        int shownFormat = AV_PIX_FMT_NONE;
        Vector<Frame*> waiting;
        Frame* shown = nullptr;
        u64 generation = 1;
        double target = 0.;
        bool playing = true;
        bool ended = false;
        bool videoEnded = false;
        bool audioEnded = false;
        double clockBase = 0.;
        u64 clockAt = 0;
        bool clockRunning = false;
        i64 drawnSecond = -1;
        bool scrubbing = false;
        float scrub = 0.f;
        bool fullscreen = false;
        bool failed = false;
        Buffer error;

        explicit Screen(Player* player);
        ~Screen() noexcept;
        void run() override;
        void resume();
        bool frame();
        void drain();
        u64 present();
        void finishIfEnded(u64 now);
        void show(Frame* frame);
        Frame* takeFirst();
        void release(VideoImage* image);
        void retired(VideoImage* image);
        void applyClock(const Clock& clock);
        void makeRender(VideoImage* image);
        RenderShader& compile(const VideoShaderCode& code);
        void seek(double to, bool play);
        void toggle();
        void sendControl(Channel* to);
        void halt(StringView text);
        bool audioMaster() const;
        double position(u64 now) const;
        void setClock(double base, bool running, u64 at);
        void keys();
        void draw();
    };

    struct CallScreen final: public plt::TimerCallback {
        Player* player;

        explicit CallScreen(Player* player);
        void ready() override;
    };

    struct Player {
        ObjPool* pool;
        Ui* ui;
        const char* path;
        Channel* videoInbox;
        Channel* audioInbox;
        Channel* screenInbox;
        plt::LoopWake* wake;
        Video* video;
        Audio* audio;
        Screen* screen;
        Thread* videoThread;
        Thread* audioThread;

        Player(ObjPool& pool, Ui& ui, const char* path);
        ~Player() noexcept;
        void post(Message* message);
    };

    [[noreturn]] static void failAv(StringView what, int error) {
        char text[AV_ERROR_MAX_STRING_SIZE];

        av_strerror(error, text, sizeof(text));
        raiseError(StringView(StringBuilder() << what << StringView(u8": ") << StringView(text)));
    }

    static void checkAl() {
        ALenum error = alGetError();

        if (error != AL_NO_ERROR) {
            const ALchar* text = alGetString(error);

            raiseError(StringView(StringBuilder() << StringView(u8"OpenAL: ") << StringView(text ? text : "error")));
        }
    }

    static void AL_APIENTRY audioEvent(ALenum, ALuint, ALuint, ALsizei, const ALchar*, void* user) noexcept {
        Pulse* pulse = new Pulse();

        if (!((Player*)user)->audioInbox->tryEnqueue(pulse)) {
            delete pulse;
        }
    }

    static void appendTime(StringBuilder& text, double seconds) {
        i64 whole = seconds > 0. ? (i64)seconds : 0;

        text << whole / 60 << StringView(u8":") << (whole % 60 < 10 ? StringView(u8"0") : StringView(u8"")) << whole % 60;
    }

    static i64 milliseconds(double seconds) {
        return (i64)llround(seconds * 1000.);
    }

    static u64 microseconds(double seconds) {
        return seconds > 0. ? (u64)ceil(seconds * 1e6) : 1;
    }

    static void freeSlot(void* size, uint8_t* data) {
        munmap(data, (size_t)(uintptr_t)size);
    }

    static AVBufferRef* allocateSlot(void*, size_t size) {
        void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (memory == MAP_FAILED) {
            return nullptr;
        }
#if defined(__linux__)
        madvise(memory, size, MADV_HUGEPAGE);
#endif
        AVBufferRef* buffer = av_buffer_create((uint8_t*)memory, size, freeSlot, (void*)(uintptr_t)size, 0);

        if (!buffer) {
            munmap(memory, size);
        }

        return buffer;
    }

    static int getSlot(AVCodecContext* codec, AVFrame* frame, int flags) {
        if (!(codec->codec->capabilities & AV_CODEC_CAP_DR1)) {
            return avcodec_default_get_buffer2(codec, frame, flags);
        }

        try {
            int width = frame->width;
            int height = frame->height;
            int align[AV_NUM_DATA_POINTERS] = {};

            avcodec_align_dimensions2(codec, &width, &height, align);
            ((Slots*)codec->opaque)->fill(frame, width, height, align);

            return 0;
        } catch (...) {
            return AVERROR(ENOMEM);
        }
    }

    static bool listed(const uint8_t* codes, size_t count, int code) {
        for (size_t i = 0; i < count; i++) {
            if (codes[i] == code) {
                return true;
            }
        }

        return false;
    }

    [[noreturn]] static void unsupported(const char* what, int code) {
        raiseError(StringView(StringBuilder() << StringView(u8"video ") << StringView(what) << StringView(u8" ") << (i64)code << StringView(u8" is not supported")));
    }

    static const char* kindOf(const VideoCode* codes, size_t count, int code) {
        for (size_t i = 0; i < count; i++) {
            if (codes[i].code == code) {
                return codes[i].kind;
            }
        }

        return nullptr;
    }

    static const VideoLayout* layoutNamed(const char* name) {
        for (const VideoFormat& format : videoFormats) {
            if (name && !strcmp(format.name, name)) {
                return &videoLayouts[format.layout];
            }
        }

        return nullptr;
    }

    static FrameShader describeFrame(const AVFrame* frame, float sdrWhiteNits) {
        AVPixelFormat format = (AVPixelFormat)frame->format;
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
        const AVBufferRef* buffer = frame->buf[0];
        const VideoLayout* layout = layoutNamed(av_get_pix_fmt_name(format));

        if (!descriptor || !layout || !buffer || frame->buf[1] || buffer->size % 4) {
            raiseError(StringView(StringBuilder() << StringView(u8"video frames of format ") << StringView(av_get_pix_fmt_name(format)) << StringView(u8" cannot be shown")));
        }

        FrameShader out;
        FrameUniform& u = out.uniform;
        StringView model(layout->model);
        ptrdiff_t lines[4];
        size_t sizes[4];

        for (int p = 0; p < 4; p++) {
            lines[p] = frame->linesize[p];
        }

        int e = av_image_fill_plane_sizes(sizes, format, frame->height, lines);

        if (e < 0) {
            failAv(StringView(u8"video frame layout"), e);
        }

        for (int p = 0; p < 4; p++) {
            if (!sizes[p]) {
                continue;
            }

            const uint8_t* data = frame->data[p];
            u64 offset = (u64)(data - buffer->data);

            if (!data || data < buffer->data || lines[p] <= 0 || offset + sizes[p] > buffer->size) {
                raiseError(StringView(u8"video frame planes exceed their buffer"));
            }

            if (offset % 4 || lines[p] % 4) {
                raiseError(StringView(u8"video frame planes are not aligned to words"));
            }

            u.planeWord[p] = (u32)(offset / 4);
            u.lineWords[p] = (u32)(lines[p] / 4);
        }

        int range = frame->color_range == AVCOL_RANGE_UNSPECIFIED && StringView(descriptor->name).startsWith(StringView(u8"yuvj")) ? AVCOL_RANGE_JPEG : frame->color_range;
        bool xyz = model == StringView(u8"xyz");
        int transfer = xyz && frame->color_trc == AVCOL_TRC_UNSPECIFIED ? AVCOL_TRC_SMPTE428 : frame->color_trc;
        const char* pattern = StringView(descriptor->name).startsWith(StringView(u8"bayer_")) ? descriptor->name + 6 : "r";
        u32 red = 0;

        while (pattern[red] != 'r') {
            red++;
        }

        u.size[0] = (u32)frame->width;
        u.size[1] = (u32)frame->height;
        u.size[2] = descriptor->log2_chroma_w;
        u.size[3] = descriptor->log2_chroma_h;
        u.tags[0] = (u32)frame->colorspace;
        u.tags[1] = (u32)range;
        u.tags[2] = (u32)transfer;
        u.tags[3] = (u32)frame->color_primaries;
        u.sites[0] = (u32)frame->chroma_location;
        u.sites[1] = red % 2;
        u.sites[2] = red / 2;
        u.white[0] = sdrWhiteNits;

        out.layout = layout->name;
        out.color = model == StringView(u8"yuv") ? kindOf(videoMatrices, sizeof(videoMatrices) / sizeof(videoMatrices[0]), frame->colorspace) : layout->model;
        out.transfer = kindOf(videoTransfers, sizeof(videoTransfers) / sizeof(videoTransfers[0]), transfer);

        if (!out.color) {
            unsupported("matrix", frame->colorspace);
        }

        if (!listed(videoRanges, sizeof(videoRanges), range)) {
            unsupported("range", range);
        }

        if (!out.transfer) {
            unsupported("transfer", transfer);
        }

        if (!xyz && !listed(videoPrimaries, sizeof(videoPrimaries), frame->color_primaries)) {
            unsupported("primaries", frame->color_primaries);
        }

        if (!listed(videoLocations, sizeof(videoLocations), frame->chroma_location)) {
            unsupported("chroma location", frame->chroma_location);
        }

        return out;
    }

    static const VideoShaderCode& shaderCode(const FrameShader& frame, const char* output) {
        for (const VideoShaderPart& part : videoShaderParts) {
            for (size_t i = 0; i < part.count; i++) {
                const VideoShaderCode& code = part.codes[i];

                if (!strcmp(code.layout, frame.layout) && !strcmp(code.color, frame.color) && !strcmp(code.transfer, frame.transfer) && !strcmp(code.output, output)) {
                    return code;
                }
            }
        }

        raiseError(StringView(StringBuilder() << StringView(u8"video frames of ") << StringView(frame.color) << StringView(u8" color with a ") << StringView(frame.transfer) << StringView(u8" transfer cannot be shown")));
    }
}

Message::~Message() noexcept {
}

template <Kind K>
Kind Typed<K>::messageKind() const {
    return K;
}

Control::Control(u64 generation_, double position_, bool playing_)
    : generation(generation_)
    , position(position_)
    , playing(playing_)
{
}

Surface::Surface(VideoImage* image_)
    : image(image_)
{
}

Frame::Frame(VideoImage* image_, u64 generation_, double pts_, double aspect_)
    : image(image_)
    , generation(generation_)
    , pts(pts_)
    , aspect(aspect_)
{
}

End::End(u64 generation_)
    : generation(generation_)
{
}

Clock::Clock(u64 generation_, double position_, u64 at_, bool running_, bool ended_)
    : generation(generation_)
    , position(position_)
    , at(at_)
    , running(running_)
    , ended(ended_)
{
}

Failure::Failure(StringView text_)
    : text(text_)
{
}

VideoImage::VideoImage(Player* player_)
    : player(player_)
    , frame(av_frame_alloc())
{
    pooledGuard(*player->pool, [this] {
        forget();
        av_frame_free(&frame);
    });

    if (!frame) {
        fail(StringView(u8"cannot allocate a video frame"));
    }
}

void VideoImage::run() {
    player->screen->retired(this);
}

void VideoImage::forget() {
    delete pool;
    pool = nullptr;
    render = nullptr;
    av_frame_unref(frame);
}

void Slots::fill(AVFrame* frame, int width, int height, const int* align) {
    AVPixelFormat format = (AVPixelFormat)frame->format;
    int lines[4] = {};
    int e = av_image_fill_linesizes(lines, format, width);

    if (e < 0) {
        failAv(StringView(u8"video frame layout"), e);
    }

    ptrdiff_t strides[4];

    for (int i = 0; i < 4; i++) {
        int alignment = align[i] > (int)planeAlignment ? align[i] : (int)planeAlignment;

        lines[i] = (lines[i] + alignment - 1) / alignment * alignment;
        strides[i] = lines[i];
    }

    size_t sizes[4];

    e = av_image_fill_plane_sizes(sizes, format, height, strides);

    if (e < 0) {
        failAv(StringView(u8"video frame layout"), e);
    }

    size_t offsets[4];
    size_t total = 0;

    for (int i = 0; i < 4; i++) {
        offsets[i] = total;
        total += (sizes[i] + planeAlignment - 1) / planeAlignment * planeAlignment;
    }

    size_t page = (size_t)sysconf(_SC_PAGESIZE);

    total = (total + planeAlignment + page - 1) / page * page;

    if (!pool || total != size) {
        av_buffer_pool_uninit(&pool);
        pool = av_buffer_pool_init2(total, nullptr, allocateSlot, nullptr);
        size = total;

        if (!pool) {
            fail(StringView(u8"cannot create the video frame pool"));
        }
    }

    AVBufferRef* buffer = av_buffer_pool_get(pool);

    if (!buffer) {
        fail(StringView(u8"cannot map a video frame"));
    }

    frame->buf[0] = buffer;

    for (int i = 0; i < 4; i++) {
        frame->data[i] = sizes[i] ? buffer->data + offsets[i] : nullptr;
        frame->linesize[i] = sizes[i] ? lines[i] : 0;
    }

    frame->extended_data = frame->data;
}

Stream::Stream(Player* player, AVMediaType type, void* slots) {
    StringView path(player->path);
    int e = avformat_open_input(&format, player->path, nullptr, nullptr);

    if (e < 0) {
        failAv(path, e);
    }

    pooledGuard(*player->pool, [this] {
        avformat_close_input(&format);
    });

    e = avformat_find_stream_info(format, nullptr);

    if (e < 0) {
        failAv(path, e);
    }

    duration = format->duration > 0 ? (double)format->duration / AV_TIME_BASE : 0.;
    start = format->start_time != AV_NOPTS_VALUE ? (double)format->start_time / AV_TIME_BASE : 0.;

    int found = av_find_best_stream(format, type, -1, -1, nullptr, 0);

    if (found < 0 || (format->streams[found]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        return;
    }

    index = found;

    for (unsigned i = 0; i < format->nb_streams; i++) {
        if ((int)i != index) {
            format->streams[i]->discard = AVDISCARD_ALL;
        }
    }

    const AVCodecParameters* parameters = format->streams[index]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(parameters->codec_id);

    if (!decoder) {
        raiseError(StringView(StringBuilder() << path << StringView(u8": no decoder for the ") << StringView(av_get_media_type_string(type)) << StringView(u8" codec ") << StringView(avcodec_get_name(parameters->codec_id))));
    }

    codec = avcodec_alloc_context3(decoder);
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    pooledGuard(*player->pool, [this] {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
    });

    if (!codec || !packet || !frame) {
        fail(StringView(u8"cannot allocate a decoder"));
    }

    e = avcodec_parameters_to_context(codec, parameters);

    if (e < 0) {
        failAv(path, e);
    }

    codec->pkt_timebase = format->streams[index]->time_base;
    codec->thread_count = 0;

    if (slots) {
        codec->get_buffer2 = getSlot;
        codec->opaque = slots;
    }

    e = avcodec_open2(codec, decoder, nullptr);

    if (e < 0) {
        failAv(path, e);
    }
}

void Stream::seek(double position) {
    i64 ts = av_rescale_q((i64)((position + start) * AV_TIME_BASE), AVRational{1, AV_TIME_BASE}, format->streams[index]->time_base);

    if (avformat_seek_file(format, index, INT64_MIN, ts, ts, 0) < 0) {
        av_seek_frame(format, index, ts, AVSEEK_FLAG_BACKWARD);
    }

    avcodec_flush_buffers(codec);
    av_frame_unref(frame);
    eof = false;
}

int Stream::receive() {
    int e = avcodec_receive_frame(codec, frame);

    if (e != AVERROR(EAGAIN)) {
        return e;
    }

    if (eof) {
        return AVERROR_EOF;
    }

    if (av_read_frame(format, packet) < 0) {
        eof = true;
        avcodec_send_packet(codec, nullptr);

        return e;
    }

    if (packet->stream_index == index) {
        avcodec_send_packet(codec, packet);
    }

    av_packet_unref(packet);

    return e;
}

double Stream::seconds(i64 timestamp) const {
    return (double)timestamp * timeBase() - start;
}

double Stream::timeBase() const {
    return av_q2d(format->streams[index]->time_base);
}

Video::Video(Player* player_)
    : player(player_)
    , stream(player_, AVMEDIA_TYPE_VIDEO, &slots)
    , last(av_frame_alloc())
{
    pooledGuard(*player->pool, [this] {
        av_frame_free(&last);
        av_buffer_pool_uninit(&slots.pool);
    });

    if (!last) {
        fail(StringView(u8"cannot allocate a video frame"));
    }

    for (size_t i = 0; i < framePermits; i++) {
        idle.pushBack(player->pool->make<VideoImage>(player));
    }
}

void Video::run() {
    try {
        for (;;) {
            void* item;

            if (!player->videoInbox->tryDequeue(&item)) {
                if (step()) {
                    continue;
                }

                player->videoInbox->dequeue(&item);
            }

            ScopedPtr<Message> message{(Message*)item};

            if (cast<Stop>(message.ptr)) {
                return;
            }

            if (Control* control = cast<Control>(message.ptr)) {
                apply(*control);
            } else if (Surface* surface = cast<Surface>(message.ptr)) {
                idle.pushBack(surface->image);
            }
        }
    } catch (...) {
        player->post(new Failure(Exception::current()));
    }
}

void Video::apply(const Control& control) {
    if (control.generation == generation) {
        return;
    }

    generation = control.generation;
    target = control.position;
    next = target;

    if (stream.index < 0) {
        return;
    }

    stream.seek(target);
    av_frame_unref(last);
    decoded = false;
    ended = false;
}

bool Video::step() {
    if (stream.index < 0 || ended) {
        return false;
    }

    if (decoded) {
        return deliver();
    }

    int e = stream.receive();

    if (e == AVERROR(EAGAIN)) {
        return true;
    }

    if (e == AVERROR_EOF && last->buf[0]) {
        av_frame_move_ref(stream.frame, last);
        pts = lastPts;
        decoded = true;
        deliver();

        return true;
    }

    if (e == AVERROR_EOF) {
        ended = true;
        player->post(new End(generation));

        return false;
    }

    if (e < 0) {
        failAv(StringView(u8"video decoding"), e);
    }

    AVFrame* frame = stream.frame;
    double length = frame->duration > 0 ? (double)frame->duration * stream.timeBase() : 0.;

    pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? stream.seconds(frame->best_effort_timestamp) : next;
    next = pts + length;

    if (length > 0. ? pts + length <= target : pts < target) {
        av_frame_unref(last);
        av_frame_move_ref(last, frame);
        lastPts = pts;

        return true;
    }

    av_frame_unref(last);
    decoded = true;
    deliver();

    return true;
}

bool Video::deliver() {
    if (idle.empty()) {
        return false;
    }

    VideoImage* image = idle.popBack();
    AVFrame* frame = stream.frame;
    AVRational sar = frame->sample_aspect_ratio;
    double aspect = (double)frame->width / frame->height * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.);

    if (stream.codec->codec->capabilities & AV_CODEC_CAP_DR1) {
        av_frame_move_ref(image->frame, frame);
    } else {
        copy(image->frame, frame);
        av_frame_unref(frame);
    }

    player->post(new Frame(image, generation, pts, aspect));
    decoded = false;

    return true;
}

void Video::copy(AVFrame* to, const AVFrame* from) {
    const int align[4] = {};

    to->format = from->format;
    to->width = from->width;
    to->height = from->height;
    slots.fill(to, from->width, from->height, align);
    av_image_copy2(to->data, to->linesize, from->data, from->linesize, (AVPixelFormat)from->format, from->width, from->height);

    int e = av_frame_copy_props(to, from);

    if (e < 0) {
        failAv(StringView(u8"video frame copy"), e);
    }
}

Audio::Audio(Player* player_)
    : player(player_)
    , stream(player_, AVMEDIA_TYPE_AUDIO)
{
    pooledGuard(*player->pool, [this] {
        swr_free(&resampler);
        av_channel_layout_uninit(&layout);
    });

    if (stream.index < 0) {
        return;
    }

    rate = stream.codec->sample_rate;

    if (rate <= 0) {
        raiseError(StringView(StringBuilder() << StringView(player->path) << StringView(u8": the audio stream has no sample rate")));
    }

    device = alcOpenDevice(nullptr);

    if (!device) {
        raiseError(StringView(u8"cannot open the audio device"));
    }

    pooledGuard(*player->pool, [this] {
        alcCloseDevice(device);
    });

    context = alcCreateContext(device, nullptr);

    if (!context) {
        raiseError(StringView(u8"cannot create an audio context"));
    }

    pooledGuard(*player->pool, [this] {
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
    });

    if (!alcMakeContextCurrent(context)) {
        raiseError(StringView(u8"cannot make the audio context current"));
    }

    if (!alIsExtensionPresent("AL_SOFT_source_latency") || !alIsExtensionPresent("AL_SOFT_events")) {
        raiseError(StringView(u8"OpenAL lacks AL_SOFT_source_latency or AL_SOFT_events"));
    }

    LPALEVENTCONTROLSOFT eventControl = (LPALEVENTCONTROLSOFT)alGetProcAddress("alEventControlSOFT");

    eventCallback = (LPALEVENTCALLBACKSOFT)alGetProcAddress("alEventCallbackSOFT");

    sourceOffsets = (LPALGETSOURCEDVSOFT)alGetProcAddress("alGetSourcedvSOFT");

    if (!eventControl || !eventCallback || !sourceOffsets) {
        raiseError(StringView(u8"OpenAL lacks the functions of its extensions"));
    }

    alGenBuffers(audioBuffers, buffers);
    checkAl();
    pooledGuard(*player->pool, [this] {
        alDeleteBuffers(audioBuffers, buffers);
    });
    alGenSources(1, &source);
    checkAl();
    pooledGuard(*player->pool, [this] {
        alSourceStop(source);
        alDeleteSources(1, &source);
    });

    const ALenum types[] = {AL_EVENT_TYPE_BUFFER_COMPLETED_SOFT, AL_EVENT_TYPE_SOURCE_STATE_CHANGED_SOFT};

    eventControl(2, types, AL_TRUE);
    eventCallback(audioEvent, player);
    pooledGuard(*player->pool, [this] {
        eventCallback(nullptr, nullptr);
    });
    checkAl();

    for (ALuint buffer : buffers) {
        idle.pushBack(buffer);
    }
}

void Audio::run() {
    try {
        for (;;) {
            void* item;

            if (!player->audioInbox->tryDequeue(&item)) {
                if (step()) {
                    continue;
                }

                player->audioInbox->dequeue(&item);
            }

            ScopedPtr<Message> message{(Message*)item};

            if (cast<Stop>(message.ptr)) {
                if (stream.index >= 0) {
                    alSourceStop(source);
                    eventCallback(nullptr, nullptr);
                }

                return;
            }

            if (stream.index < 0) {
                continue;
            }

            if (Control* control = cast<Control>(message.ptr)) {
                apply(*control);
            } else if (cast<Pulse>(message.ptr)) {
                service();
            }
        }
    } catch (...) {
        player->post(new Failure(Exception::current()));
    }
}

void Audio::apply(const Control& control) {
    if (control.generation != generation) {
        generation = control.generation;
        target = control.position;
        alSourceStop(source);
        alSourcei(source, AL_BUFFER, 0);
        checkAl();
        idle.clear();

        for (ALuint buffer : buffers) {
            idle.pushBack(buffer);
        }

        ringHead = 0;
        ringLength = 0;
        stream.seek(target);
        swr_free(&resampler);
        pcm.reset();
        pcmStart = target;
        queuedStart = target;
        skipping = true;
        drained = false;
        ended = false;
    }

    playing = control.playing;

    if (!playing && state() == AL_PLAYING) {
        alSourcePause(source);
        checkAl();
    }

    start();
    report();
}

void Audio::service() {
    ALint processed = 0;

    alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
    checkAl();

    for (; processed > 0 && ringLength > 0; processed--) {
        ALuint buffer;

        alSourceUnqueueBuffers(source, 1, &buffer);
        checkAl();
        queuedStart += (double)ring[ringHead] / rate;
        ringHead = (ringHead + 1) % audioBuffers;
        ringLength--;
        idle.pushBack(buffer);
    }

    finish();

    if (!ended) {
        report();
    }
}

bool Audio::step() {
    if (stream.index < 0 || ended) {
        return false;
    }

    size_t have = samples();

    if (have >= bufferSamples() || (drained && have > 0)) {
        if (idle.empty()) {
            return false;
        }

        enqueue();

        return true;
    }

    if (drained) {
        finish();

        return false;
    }

    int e = stream.receive();

    if (e == AVERROR(EAGAIN)) {
        return true;
    }

    if (e == AVERROR_EOF) {
        drained = true;
        start();

        return true;
    }

    if (e < 0) {
        failAv(StringView(u8"audio decoding"), e);
    }

    convert();

    return true;
}

void Audio::convert() {
    AVFrame* frame = stream.frame;

    if (!resampler || frame->format != format || frame->sample_rate != inputRate || av_channel_layout_compare(&frame->ch_layout, &layout)) {
        AVChannelLayout stereo;

        swr_free(&resampler);
        av_channel_layout_default(&stereo, 2);

        int e = swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, rate, &frame->ch_layout, (AVSampleFormat)frame->format, frame->sample_rate, 0, nullptr);

        if (e >= 0) {
            e = swr_init(resampler);
        }

        if (e < 0) {
            failAv(StringView(u8"audio conversion"), e);
        }

        format = frame->format;
        inputRate = frame->sample_rate;
        av_channel_layout_uninit(&layout);
        av_channel_layout_copy(&layout, &frame->ch_layout);
    }

    if (pcm.empty() && frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        pcmStart = stream.seconds(frame->best_effort_timestamp);
    }

    int room = swr_get_out_samples(resampler, frame->nb_samples);
    size_t used = pcm.used();

    pcm.grow(used + (size_t)room * sampleBytes);

    uint8_t* out = (uint8_t*)pcm.mutData() + used;
    int converted = swr_convert(resampler, &out, room, (const uint8_t**)frame->extended_data, frame->nb_samples);

    if (converted < 0) {
        failAv(StringView(u8"audio conversion"), converted);
    }

    pcm.seekAbsolute(used + (size_t)converted * sampleBytes);

    if (!skipping) {
        return;
    }

    if (pcmStart + (double)samples() / rate <= target) {
        pcm.reset();

        return;
    }

    if (pcmStart < target) {
        drop((size_t)((target - pcmStart) * rate));
    }

    skipping = false;
}

void Audio::drop(size_t taken) {
    size_t bytes = taken * sampleBytes;

    memmove(pcm.mutData(), (const uint8_t*)pcm.data() + bytes, pcm.used() - bytes);
    pcm.seekAbsolute(pcm.used() - bytes);
    pcmStart += (double)taken / rate;
}

void Audio::enqueue() {
    size_t taken = samples() < bufferSamples() ? samples() : bufferSamples();
    ALuint buffer = idle.popBack();

    alBufferData(buffer, AL_FORMAT_STEREO16, pcm.data(), (ALsizei)(taken * sampleBytes), rate);
    alSourceQueueBuffers(source, 1, &buffer);
    checkAl();

    if (ringLength == 0) {
        queuedStart = pcmStart;
    }

    ring[(ringHead + ringLength) % audioBuffers] = taken;
    ringLength++;
    drop(taken);
    start();
}

void Audio::start() {
    if (!playing || ringLength == 0) {
        return;
    }

    ALint now = state();

    if (now == AL_PLAYING) {
        return;
    }

    if (now == AL_PAUSED || idle.empty() || drained) {
        alSourcePlay(source);
        checkAl();
        report();
    }
}

void Audio::finish() {
    if (drained && !ended && pcm.empty() && ringLength == 0) {
        ended = true;
        report();
    }
}

void Audio::report() {
    ALint now = state();
    double offsets[2] = {0., 0.};

    if (ringLength > 0) {
        sourceOffsets(source, AL_SEC_OFFSET_LATENCY_SOFT, offsets);
        checkAl();
    }

    bool running = now == AL_PLAYING;

    player->post(new Clock(generation, queuedStart + offsets[0] - (running ? offsets[1] : 0.), monotonicNowUs(), running, ended));
}

ALint Audio::state() {
    ALint value = AL_INITIAL;

    alGetSourcei(source, AL_SOURCE_STATE, &value);
    checkAl();

    return value;
}

size_t Audio::samples() const {
    return pcm.used() / sampleBytes;
}

size_t Audio::bufferSamples() const {
    return (size_t)rate / bufferRate;
}

Screen::Screen(Player* player_)
    : player(player_)
    , hasVideo(player_->video->stream.index >= 0)
    , hasAudio(player_->audio->stream.index >= 0)
    , duration(fmax(player_->video->stream.duration, player_->audio->stream.duration))
{
    if (!hasVideo && !hasAudio) {
        raiseError(StringView(StringBuilder() << StringView(player->path) << StringView(u8": no audio or video")));
    }

    fiber = player->ui->platform()->scheduler()->create(*player->pool, *this, controllerStack);
    player->ui->trace(StringView(StringBuilder() << StringView(u8"opened duration_ms=") << milliseconds(duration) << StringView(u8" video=") << (i64)hasVideo << StringView(u8" audio=") << (i64)hasAudio));
}

Screen::~Screen() noexcept {
    for (Frame* frame : waiting) {
        delete frame;
    }

    delete shown;
}

void Screen::run() {
    plt::Scheduler& scheduler = *player->ui->platform()->scheduler();

    for (;;) {
        u64 wait = 0;

        try {
            drain();
            wait = present();
        } catch (...) {
            halt(Exception::current());
        }

        if (wait) {
            scheduler.current()->parkFor(wait);
        } else {
            scheduler.current()->park();
        }
    }
}

void Screen::resume() {
    fiber->wake();
}

bool Screen::frame() {
    if (failed) {
        return !player->ui->drawErrorPanel(StringView(error));
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Q)) {
        return false;
    }

    keys();
    draw();

    return true;
}

void Screen::drain() {
    void* item;

    while (player->screenInbox->tryDequeue(&item)) {
        ScopedPtr<Message> message{(Message*)item};

        if (failed) {
            continue;
        }

        if (Frame* frame = cast<Frame>(message.ptr)) {
            if (frame->generation == generation) {
                waiting.pushBack(frame);
                message.drop();
            } else {
                release(frame->image);
            }
        } else if (Surface* surface = cast<Surface>(message.ptr)) {
            release(surface->image);
        } else if (Clock* clock = cast<Clock>(message.ptr)) {
            applyClock(*clock);
        } else if (End* end = cast<End>(message.ptr)) {
            videoEnded = videoEnded || end->generation == generation;
        } else if (Failure* failure = cast<Failure>(message.ptr)) {
            halt(StringView(failure->text));
        }
    }
}

u64 Screen::present() {
    if (failed) {
        return 0;
    }

    u64 now = monotonicNowUs();

    if (!waiting.empty() && (!shown || shown->generation != generation)) {
        show(takeFirst());
    }

    if (playing && !ended && !clockRunning && !audioMaster() && shown && shown->generation == generation) {
        setClock(clockBase > shown->pts ? clockBase : shown->pts, true, now);
    }

    double at = position(now);

    if (clockRunning) {
        while (!waiting.empty() && waiting[0]->pts <= at) {
            Frame* frame = takeFirst();

            if (!waiting.empty() && waiting[0]->pts <= at) {
                release(frame->image);
                delete frame;
            } else {
                show(frame);
            }
        }
    }

    finishIfEnded(now);
    at = position(now);

    i64 second = (i64)floor(at);

    if (second != drawnSecond) {
        drawnSecond = second;
        player->ui->requestFrame();
    }

    if (!clockRunning) {
        return 0;
    }

    u64 wait = microseconds((double)(second + 1) - at);

    if (!waiting.empty()) {
        u64 next = microseconds(waiting[0]->pts - at);

        wait = next < wait ? next : wait;
    }

    return wait;
}

void Screen::finishIfEnded(u64 now) {
    if (!playing || ended) {
        return;
    }

    if (hasVideo && !(videoEnded && waiting.empty())) {
        return;
    }

    if (hasAudio && !audioEnded) {
        return;
    }

    ended = true;
    playing = false;
    setClock(position(now), false, now);
    player->ui->trace(StringView(StringBuilder() << StringView(u8"ended generation=") << (i64)generation));
    player->ui->requestFrame();
}

void Screen::show(Frame* frame) {
    Frame* previous = shown;

    shown = frame;

    if (previous) {
        if (previous->image->draws == 0) {
            release(previous->image);
        }

        delete previous;
    }

    if (!frame->image->render) {
        makeRender(frame->image);
    }

    player->ui->trace(StringView(StringBuilder() << StringView(u8"show generation=") << (i64)frame->generation << StringView(u8" position_ms=") << milliseconds(frame->pts)));
    player->ui->requestFrame();
}

Frame* Screen::takeFirst() {
    Frame* first = waiting[0];

    for (size_t i = 1; i < waiting.length(); i++) {
        waiting.mut(i - 1) = waiting[i];
    }

    waiting.popBack();

    return first;
}

void Screen::release(VideoImage* image) {
    if (failed) {
        return;
    }

    image->forget();
    player->videoInbox->enqueue(new Surface(image));
}

void Screen::retired(VideoImage* image) {
    if (--image->draws == 0 && (!shown || shown->image != image)) {
        release(image);
    }
}

void Screen::applyClock(const Clock& clock) {
    if (clock.generation != generation) {
        return;
    }

    audioEnded = clock.ended;
    setClock(clock.position, clock.running && !clock.ended, clock.at);
}

void Screen::makeRender(VideoImage* image) {
    AVFrame* frame = image->frame;

    if (frame->width != shownWidth || frame->height != shownHeight || frame->format != shownFormat) {
        shownWidth = frame->width;
        shownHeight = frame->height;
        shownFormat = frame->format;
        player->ui->trace(StringView(StringBuilder() << StringView(u8"video ") << (i64)frame->width << StringView(u8"x") << (i64)frame->height << StringView(u8" ") << StringView(av_get_pix_fmt_name((AVPixelFormat)frame->format))));
    }

    FrameShader shading = describeFrame(frame, RendererOptions{}.sdrWhiteNits);
    RenderShader& shader = compile(shaderCode(shading, output));
    ScopedPtr<ObjPool> owner{ObjPool::fromMemoryRaw()};

    image->render = player->ui->shadeImage(*owner.ptr, shader, (u32)frame->width, (u32)frame->height, frame->buf[0]->data, frame->buf[0]->size, &shading.uniform, sizeof(shading.uniform), *image);
    image->render->prepare();
    image->pool = owner.ptr;
    owner.drop();
}

RenderShader& Screen::compile(const VideoShaderCode& code) {
    for (const CompiledShader& known : compiled) {
        if (known.code == &code) {
            return *known.shader;
        }
    }

    RenderShader* shader = player->ui->compileShader(*player->pool, code.code, code.size);

    compiled.pushBack(CompiledShader{&code, shader});
    player->ui->trace(StringView(StringBuilder() << StringView(u8"compiled video shader ") << StringView(code.layout) << StringView(u8" ") << StringView(code.color) << StringView(u8" ") << StringView(code.transfer) << StringView(u8" ") << StringView(code.output)));

    return *shader;
}

void Screen::seek(double to, bool play) {
    if (failed) {
        return;
    }

    double end = duration > 0. ? duration : to;

    to = to < 0. ? 0. : to > end ? end : to;
    generation++;
    target = to;
    playing = play;
    ended = false;
    videoEnded = false;
    audioEnded = false;
    setClock(to, false, monotonicNowUs());

    for (Frame* frame : waiting) {
        release(frame->image);
        delete frame;
    }

    waiting.clear();
    player->ui->trace(StringView(StringBuilder() << StringView(u8"seek generation=") << (i64)generation << StringView(u8" position_ms=") << milliseconds(to)));
    sendControl(player->videoInbox);
    sendControl(player->audioInbox);
    player->ui->requestFrame();
    resume();
}

void Screen::toggle() {
    if (failed) {
        return;
    }

    if (ended) {
        seek(0., true);

        return;
    }

    u64 now = monotonicNowUs();

    playing = !playing;

    if (!audioMaster()) {
        setClock(position(now), false, now);
    }

    player->ui->trace(StringView(StringBuilder() << (playing ? StringView(u8"play generation=") : StringView(u8"pause generation=")) << (i64)generation));
    sendControl(player->audioInbox);
    player->ui->requestFrame();
    resume();
}

void Screen::sendControl(Channel* to) {
    to->enqueue(new Control(generation, target, playing));
}

void Screen::halt(StringView text) {
    if (failed) {
        return;
    }

    failed = true;
    error = Buffer(text);
    sysE << StringView(u8"im play: ") << text << endL;
    player->ui->trace(StringView(StringBuilder() << StringView(u8"failed: ") << text));
    player->videoInbox->enqueue(new Stop());
    player->audioInbox->enqueue(new Stop());
    player->ui->requestFrame();
}

bool Screen::audioMaster() const {
    return hasAudio && !audioEnded;
}

double Screen::position(u64 now) const {
    if (!clockRunning || now <= clockAt) {
        return clockBase;
    }

    return clockBase + (double)(now - clockAt) / 1e6;
}

void Screen::setClock(double base, bool running, u64 at) {
    clockBase = base;
    clockAt = at;
    clockRunning = running;
}

void Screen::keys() {
    double at = position(monotonicNowUs());

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        toggle();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        seek(at + seekStep, playing);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        seek(at - seekStep, playing);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
        seek(0., false);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F, false) || ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
        fullscreen = !fullscreen;
        player->ui->requestFullscreen(fullscreen);
    }
}

void Screen::draw() {
    Ui& ui = *player->ui;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float pad = ui.px(barPadding);
    float bar = ImGui::GetFrameHeight() + 2.f * pad;
    double at = position(monotonicNowUs());

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##play", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 lo = vp->Pos;
    ImVec2 hi(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y - bar);
    float width = hi.x - lo.x;
    float height = hi.y - lo.y;

    dl->AddRectFilled(lo, hi, IM_COL32(0, 0, 0, 255));

    if (shown && width >= 1.f && height >= 1.f) {
        float aspect = (float)shown->aspect;
        float w = width;
        float h = w / aspect;

        if (h > height) {
            h = height;
            w = h * aspect;
        }

        ImVec2 p0(floorf(lo.x + (width - w) / 2.f), floorf(lo.y + (height - h) / 2.f));

        shown->image->draws++;
        shown->image->render->draw(*dl, p0, ImVec2(p0.x + floorf(w), p0.y + floorf(h)));
    } else {
        const char* text = hasVideo ? "opening" : "no video";
        ImVec2 extent = ImGui::CalcTextSize(text);

        dl->AddText(ImVec2(lo.x + (width - extent.x) / 2.f, lo.y + (height - extent.y) / 2.f), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
    }

    ImGui::SetCursorScreenPos(ImVec2(lo.x + pad, hi.y + pad));

    if (ImGui::Button(playing ? "Pause##toggle" : "Play##toggle", ImVec2(ui.px(buttonWidth), 0.f))) {
        toggle();
    }

    ImGui::SameLine();

    if (ImGui::Button("Stop", ImVec2(ui.px(buttonWidth), 0.f))) {
        seek(0., false);
    }

    StringBuilder time;

    appendTime(time, at);
    time << StringView(u8" / ");
    appendTime(time, duration);

    float label = ImGui::CalcTextSize(time.cStr()).x;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(fmaxf(1.f, ImGui::GetContentRegionAvail().x - label - ImGui::GetStyle().ItemSpacing.x - pad));
    ImGui::BeginDisabled(duration <= 0.);

    float value = scrubbing ? scrub : (float)at;

    ImGui::SliderFloat("##position", &value, 0.f, (float)fmax(duration, 0.), "", ImGuiSliderFlags_NoInput);

    if (ImGui::IsItemActive()) {
        scrubbing = true;
        scrub = value;
    }

    if (ImGui::IsItemDeactivated()) {
        scrubbing = false;

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            seek(scrub, playing);
        }
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(time.cStr());
    ImGui::End();
}

CallScreen::CallScreen(Player* player_)
    : player(player_)
{
}

void CallScreen::ready() {
    player->screen->resume();
}

Player::Player(ObjPool& pool_, Ui& ui_, const char* path_)
    : pool(&pool_)
    , ui(&ui_)
    , path(path_)
    , videoInbox(Channel::create(pool, channelCapacity))
    , audioInbox(Channel::create(pool, channelCapacity))
    , screenInbox(Channel::create(pool, channelCapacity))
    , wake(ui->platform()->createLoopWake(*pool, *pool->make<CallScreen>(this)))
    , video(pool->make<Video>(this))
    , audio(pool->make<Audio>(this))
    , screen(pool->make<Screen>(this))
    , videoThread(Thread::create(pool, *video, pool->allocateOverAligned(threadStack, (size_t)sysconf(_SC_PAGESIZE)), threadStack))
    , audioThread(Thread::create(pool, *audio, pool->allocateOverAligned(threadStack, (size_t)sysconf(_SC_PAGESIZE)), threadStack))
{
}

Player::~Player() noexcept {
    videoInbox->enqueue(new Stop());
    audioInbox->enqueue(new Stop());
    videoThread->join();
    audioThread->join();

    Channel* inboxes[] = {videoInbox, audioInbox, screenInbox};

    for (Channel* inbox : inboxes) {
        void* item;

        while (inbox->tryDequeue(&item)) {
            delete (Message*)item;
        }
    }
}

void Player::post(Message* message) {
    screenInbox->enqueue(message);
    wake->signal();
}

#if defined(IM_FOR_TESTS)
namespace {
    constexpr int sampleWidth = 63;
    constexpr int sampleHeight = 47;
    constexpr size_t samplePixels = (size_t)sampleWidth * sampleHeight;
    constexpr double readback = 1. / 512.;

    struct Chromaticity {
        int code;
        double rx, ry, gx, gy, bx, by, wx, wy;
    };

    constexpr Chromaticity chromaticities[] = {
        {1, 0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290},
        {2, 0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290},
        {4, 0.670, 0.330, 0.210, 0.710, 0.140, 0.080, 0.3100, 0.3160},
        {5, 0.640, 0.330, 0.290, 0.600, 0.150, 0.060, 0.3127, 0.3290},
        {6, 0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290},
        {7, 0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290},
        {8, 0.681, 0.319, 0.243, 0.692, 0.145, 0.049, 0.3100, 0.3160},
        {9, 0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290},
        {11, 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3140, 0.3510},
        {12, 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290},
        {22, 0.630, 0.340, 0.295, 0.605, 0.155, 0.077, 0.3127, 0.3290},
    };

    constexpr int lumaBytes[4] = {1, 2, 4, 5};

    struct Matrix {
        double m[3][3];
    };

    struct Ignored final: public Runable {
        void run() override;
    };

    enum class Model {
        Yuv,
        Rgb,
        Gray,
        Xyz,
        Palette,
        Bayer,
    };

    struct Case {
        AVPixelFormat format = AV_PIX_FMT_NONE;
        AVColorSpace matrix = AVCOL_SPC_BT709;
        AVColorRange range = AVCOL_RANGE_MPEG;
        AVColorTransferCharacteristic transfer = AVCOL_TRC_BT709;
        AVColorPrimaries primaries = AVCOL_PRI_BT709;
        AVChromaLocation location = AVCHROMA_LOC_LEFT;
    };

    struct Levels {
        double offset;
        double scale;
    };

    struct FormatCheck {
        ObjPool* pool;
        Ui* ui;
        Slots slots;
        Vector<CompiledShader> compiled;
        Ignored retired;
        Vector<double> expected;
        int checked = 0;
        int failed = 0;

        FormatCheck(ObjPool* pool, Ui* ui);
        RenderShader& compile(const VideoShaderCode& code);
        AVFrame* frame(const Case& kase);
        void write(AVFrame* frame, const Case& kase);
        void writePalette(AVFrame* frame);
        void linearize(const Case& kase);
        void shade(AVFrame* frame, const char* output, Vector<double>& out);
        void compare(StringView what, StringView output, const Vector<double>& got, double tolerance, bool relative);
        void verify(AVFrame* frame, StringView what, double tolerance);
        void verifyLinear(AVFrame* frame, StringView what, double tolerance);
        void check(const Case& kase, StringView what);
        void formats();
        void matrices();
        void locations();
        void systems();
        void transfers();
        void primaries();
    };

    static double sampleValue(double x, double y, int c) {
        double u = x / (sampleWidth - 1);
        double v = y / (sampleHeight - 1);
        const double values[4] = {0.15 + 0.7 * u, 0.2 + 0.6 * v, 0.25 + 0.25 * (u + v), 0.3 + 0.7 * u};

        return values[c];
    }

    static double srgbDecode(double v) {
        return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    }

    static double srgbEncode(double v) {
        return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1. / 2.4) - 0.055;
    }

    static Matrix multiply(const Matrix& a, const Matrix& b) {
        Matrix out{};

        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                for (int k = 0; k < 3; k++) {
                    out.m[i][j] += a.m[i][k] * b.m[k][j];
                }
            }
        }

        return out;
    }

    static Matrix invert(const Matrix& a) {
        const double (&m)[3][3] = a.m;
        double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);

        return {{
            {(m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det},
            {(m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det},
            {(m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det},
        }};
    }

    static void apply(const Matrix& m, const double* in, double* out) {
        for (int i = 0; i < 3; i++) {
            out[i] = m.m[i][0] * in[0] + m.m[i][1] * in[1] + m.m[i][2] * in[2];
        }
    }

    static Matrix rgbToXyz(const Chromaticity& c) {
        auto xyz = [](double x, double y, double* out) {
            out[0] = x / y;
            out[1] = 1.;
            out[2] = (1. - x - y) / y;
        };
        double r[3], g[3], b[3], w[3], scale[3];

        xyz(c.rx, c.ry, r);
        xyz(c.gx, c.gy, g);
        xyz(c.bx, c.by, b);
        xyz(c.wx, c.wy, w);

        Matrix primaries = {{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};

        apply(invert(primaries), w, scale);

        Matrix out = multiply(primaries, Matrix{{{scale[0], 0., 0.}, {0., scale[1], 0.}, {0., 0., scale[2]}}});

        if (c.wx != 0.3127 || c.wy != 0.3290) {
            const Matrix bradford = {{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
            double d65[3], from[3], to[3];

            xyz(0.3127, 0.3290, d65);
            apply(bradford, w, from);
            apply(bradford, d65, to);
            out = multiply(multiply(invert(bradford), multiply(Matrix{{{to[0] / from[0], 0., 0.}, {0., to[1] / from[1], 0.}, {0., 0., to[2] / from[2]}}}, bradford)), out);
        }

        return out;
    }

    static Matrix toBt2020(int code) {
        Matrix fromXyz = invert(rgbToXyz(chromaticities[7]));

        if (code == 10) {
            return fromXyz;
        }

        for (const Chromaticity& c : chromaticities) {
            if (c.code == code) {
                return multiply(fromXyz, rgbToXyz(c));
            }
        }

        fail(StringView(u8"no chromaticities for these primaries"));
    }

    static double pqEncode(double nits) {
        const double m1 = 2610. / 16384.;
        const double m2 = 2523. / 32.;
        const double c1 = 3424. / 4096.;
        const double c2 = 2413. / 128.;
        const double c3 = 2392. / 128.;
        double y = pow(nits / 10000., m1);

        return pow((c1 + c2 * y) / (1. + c3 * y), m2);
    }

    static double hlgEncode(double scene) {
        const double a = 0.17883277;
        const double b = 0.28466892;
        const double c = 0.55991073;

        return scene <= 1. / 12. ? sqrt(3. * scene) : a * log(12. * scene - b) + c;
    }

    static double encodeTransfer(int code, double linear, double white) {
        switch (code) {
            case 4:
                return pow(linear, 1. / 2.2);
            case 5:
                return pow(linear, 1. / 2.8);
            case 8:
                return linear;
            case 9:
                return 1. + log10(linear) / 2.;
            case 10:
                return 1. + log10(linear) / 2.5;
            case 13:
                return srgbEncode(linear);
            case 16:
                return pqEncode(linear * white);
            case 17:
                return pow(linear * 48. / 52.37, 1. / 2.6);
            case 18:
                return hlgEncode(pow(linear * white / 1000., 1. / 1.2));
            default:
                return pow(linear, 1. / 2.4);
        }
    }


    static double pqDecode(double signal) {
        const double m1 = 2610. / 16384.;
        const double m2 = 2523. / 32.;
        const double c1 = 3424. / 4096.;
        const double c2 = 2413. / 128.;
        const double c3 = 2392. / 128.;
        double p = pow(signal, 1. / m2);

        return pow(fmax(p - c1, 0.) / (c2 - c3 * p), 1. / m1) * 10000.;
    }

    static double hlgDecode(double signal) {
        const double a = 0.17883277;
        const double b = 0.28466892;
        const double c = 0.55991073;

        return signal <= 0.5 ? signal * signal / 3. : (exp((signal - c) / a) + b) / 12.;
    }

    static double bt709Encode(double light, double alpha, double beta, double slope) {
        return light < beta ? slope * light : alpha * pow(light, 0.45) - (alpha - 1.);
    }

    static double bt709Decode(double signal, double alpha, double beta, double slope) {
        return signal < slope * beta ? signal / slope : pow((signal + alpha - 1.) / alpha, 1. / 0.45);
    }

    static double oetf(int code, double light) {
        switch (code) {
            case 4:
                return pow(light, 1. / 2.2);
            case 5:
                return pow(light, 1. / 2.8);
            case 7:
                return bt709Encode(light, 1.1115, 0.0228, 4.);
            case 8:
                return light;
            case 9:
                return light < 0.01 ? 0. : 1. + log10(light) / 2.;
            case 10:
                return light < pow(10., -2.5) ? 0. : 1. + log10(light) / 2.5;
            case 13:
                return srgbEncode(light);
            case 16:
                return pqEncode(light * 10000.);
            case 17:
                return pow(light * 48. / 52.37, 1. / 2.6);
            case 18:
                return hlgEncode(light);
            default:
                return bt709Encode(light, 1.099296826809442, 0.018053968510807, 4.5);
        }
    }

    static double inverseOetf(int code, double signal) {
        switch (code) {
            case 4:
                return pow(signal, 2.2);
            case 5:
                return pow(signal, 2.8);
            case 7:
                return bt709Decode(signal, 1.1115, 0.0228, 4.);
            case 8:
                return signal;
            case 9:
                return signal <= 0. ? 0. : pow(10., (signal - 1.) * 2.);
            case 10:
                return signal <= 0. ? 0. : pow(10., (signal - 1.) * 2.5);
            case 13:
                return srgbDecode(signal);
            case 16:
                return pqDecode(signal) / 10000.;
            case 17:
                return pow(signal, 2.6) * 52.37 / 48.;
            case 18:
                return hlgDecode(signal);
            default:
                return bt709Decode(signal, 1.099296826809442, 0.018053968510807, 4.5);
        }
    }

    static Matrix primariesToXyz(int code) {
        for (const Chromaticity& c : chromaticities) {
            if (c.code == code) {
                return rgbToXyz(c);
            }
        }

        return rgbToXyz(chromaticities[0]);
    }

    static void displayLight(int transfer, int primaries, const double* signal, double* light) {
        double white = RendererOptions{}.sdrWhiteNits;
        Matrix toXyz = primariesToXyz(primaries);

        for (int c = 0; c < 3; c++) {
            double v = fmax(signal[c], 0.);

            switch (transfer) {
                case 4:
                    light[c] = pow(v, 2.2);
                    break;
                case 5:
                    light[c] = pow(v, 2.8);
                    break;
                case 8:
                    light[c] = signal[c];
                    break;
                case 9:
                case 10:
                    light[c] = inverseOetf(transfer, signal[c]);
                    break;
                case 13:
                    light[c] = srgbDecode(v);
                    break;
                case 16:
                    light[c] = pqDecode(v) / white;
                    break;
                case 17:
                    light[c] = pow(v, 2.6) * 52.37 / 48.;
                    break;
                case 18:
                    light[c] = hlgDecode(v);
                    break;
                default:
                    light[c] = pow(v, 2.4);
                    break;
            }
        }

        if (transfer == 18) {
            double luminance = fmax(toXyz.m[1][0] * light[0] + toXyz.m[1][1] * light[1] + toXyz.m[1][2] * light[2], 0.);

            for (int c = 0; c < 3; c++) {
                light[c] = 1000. * pow(luminance, 0.2) * light[c] / white;
            }
        }
    }

    static Model modelOf(const VideoLayout& layout) {
        StringView model(layout.model);

        return model == StringView(u8"yuv") ? Model::Yuv
             : model == StringView(u8"rgb") ? Model::Rgb
             : model == StringView(u8"gray") ? Model::Gray
             : model == StringView(u8"xyz") ? Model::Xyz
             : model == StringView(u8"palette") ? Model::Palette
             : Model::Bayer;
    }

    static double lumaWeight(const Case& kase, int channel) {
        switch (kase.matrix) {
            case AVCOL_SPC_BT709:
                return channel == 0 ? 0.2126 : 0.0722;
            case AVCOL_SPC_FCC:
                return channel == 0 ? 0.30 : 0.11;
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:
                return channel == 0 ? 0.299 : 0.114;
            case AVCOL_SPC_SMPTE240M:
                return channel == 0 ? 0.212 : 0.087;
            case AVCOL_SPC_BT2020_NCL:
            case AVCOL_SPC_BT2020_CL:
                return channel == 0 ? 0.2627 : 0.0593;
            case AVCOL_SPC_CHROMA_DERIVED_NCL:
            case AVCOL_SPC_CHROMA_DERIVED_CL:
                return primariesToXyz(kase.primaries).m[1][channel == 0 ? 0 : 2];
            default:
                return sampleHeight > 576 ? (channel == 0 ? 0.2126 : 0.0722) : (channel == 0 ? 0.299 : 0.114);
        }
    }

    static void forward(const Case& kase, const double* rgb, double* ycc) {
        double kr = lumaWeight(kase, 0);
        double kb = lumaWeight(kase, 2);
        double kg = 1. - kr - kb;

        switch (kase.matrix) {
            case AVCOL_SPC_RGB:
                ycc[0] = rgb[1];
                ycc[1] = rgb[2];
                ycc[2] = rgb[0];
                return;
            case AVCOL_SPC_YCGCO:
                ycc[0] = rgb[0] / 4. + rgb[1] / 2. + rgb[2] / 4.;
                ycc[1] = -rgb[0] / 4. + rgb[1] / 2. - rgb[2] / 4.;
                ycc[2] = rgb[0] / 2. - rgb[2] / 2.;
                return;
            case AVCOL_SPC_YCGCO_RE:
            case AVCOL_SPC_YCGCO_RO: {
                double co = rgb[0] - rgb[2];
                double t = rgb[2] + co / 2.;
                double cg = rgb[1] - t;

                ycc[0] = t + cg / 2.;
                ycc[1] = cg;
                ycc[2] = co;
                return;
            }
            case AVCOL_SPC_SMPTE2085:
                ycc[0] = rgb[1];
                ycc[1] = (0.986566 * rgb[2] - rgb[1]) / 2.;
                ycc[2] = (rgb[0] - 0.991902 * rgb[1]) / 2.;
                return;
            case AVCOL_SPC_BT2020_CL:
            case AVCOL_SPC_CHROMA_DERIVED_CL: {
                int tf = kase.transfer;
                double y = oetf(tf, kr * inverseOetf(tf, rgb[0]) + kg * inverseOetf(tf, rgb[1]) + kb * inverseOetf(tf, rgb[2]));
                double b = rgb[2] - y;
                double r = rgb[0] - y;

                ycc[0] = y;
                ycc[1] = b / (2. * (b <= 0. ? oetf(tf, 1. - kb) : 1. - oetf(tf, kb)));
                ycc[2] = r / (2. * (r <= 0. ? oetf(tf, 1. - kr) : 1. - oetf(tf, kr)));
                return;
            }
            case AVCOL_SPC_ICTCP: {
                bool hlg = kase.transfer == AVCOL_TRC_ARIB_STD_B67;
                const Matrix toLms = {{{1688. / 4096., 2146. / 4096., 262. / 4096.}, {683. / 4096., 2951. / 4096., 462. / 4096.}, {99. / 4096., 309. / 4096., 3688. / 4096.}}};
                const Matrix toIctcp = hlg
                    ? Matrix{{{0.5, 0.5, 0.}, {3625. / 4096., -7465. / 4096., 3840. / 4096.}, {9500. / 4096., -9212. / 4096., -288. / 4096.}}}
                    : Matrix{{{0.5, 0.5, 0.}, {6610. / 4096., -13613. / 4096., 7003. / 4096.}, {17933. / 4096., -17390. / 4096., -543. / 4096.}}};
                double light[3];
                double lms[3];

                for (int c = 0; c < 3; c++) {
                    light[c] = hlg ? hlgDecode(rgb[c]) : pqDecode(rgb[c]);
                }

                apply(toLms, light, lms);

                for (int c = 0; c < 3; c++) {
                    lms[c] = hlg ? hlgEncode(lms[c]) : pqEncode(lms[c]);
                }

                apply(toIctcp, lms, ycc);
                return;
            }
            default:
                ycc[0] = kr * rgb[0] + kg * rgb[1] + kb * rgb[2];
                ycc[1] = (rgb[2] - ycc[0]) / (2. * (1. - kb));
                ycc[2] = (rgb[0] - ycc[0]) / (2. * (1. - kr));
                return;
        }
    }

    static bool reversible(const Case& kase) {
        return kase.matrix == AVCOL_SPC_YCGCO_RE || kase.matrix == AVCOL_SPC_YCGCO_RO;
    }

    static Levels levels(const Case& kase, const VideoLayout& layout, int c) {
        Model model = modelOf(layout);
        int depth = layout.components[c][4];
        bool alpha = layout.alpha && c == layout.count - 1;
        bool chroma = model == Model::Yuv && (c == 1 || c == 2) && kase.matrix != AVCOL_SPC_RGB;
        bool full = depth < 8 || kase.range == AVCOL_RANGE_JPEG || (kase.range == AVCOL_RANGE_UNSPECIFIED && model != Model::Yuv);
        double unit = exp2(depth - 8.);

        if (layout.floating) {
            return {0., 1.};
        }

        if (alpha) {
            return {0., exp2(depth) - 1.};
        }

        if (model == Model::Yuv && reversible(kase)) {
            double scale = exp2(depth - (kase.matrix == AVCOL_SPC_YCGCO_RE ? 2 : 1)) - 1.;

            return {chroma ? exp2(depth - 1.) : 0., scale};
        }

        if (chroma) {
            return full ? Levels{exp2(depth - 1.), exp2(depth) - 1.} : Levels{128. * unit, 224. * unit};
        }

        return full ? Levels{0., exp2(depth) - 1.} : Levels{16. * unit, 219. * unit};
    }

    static double within(AVChromaLocation location, int axis) {
        const double sites[7][2] = {{0., 0.5}, {0., 0.5}, {0.5, 0.5}, {0., 0.}, {0.5, 0.}, {0., 1.}, {0.5, 1.}};

        return sites[location][axis];
    }

    static u32 halfBits(double value) {
        u32 bits = __builtin_bit_cast(u32, (float)value);
        u32 sign = (bits >> 16) & 0x8000u;
        int exponent = (int)((bits >> 23) & 255u) - 127 + 15;
        u32 mantissa = bits & 0x7fffffu;
        u32 rest = mantissa & 0x1fffu;
        u32 half = sign | (u32)exponent << 10 | mantissa >> 13;

        if (exponent <= 0) {
            return sign;
        }

        return rest > 0x1000u || (rest == 0x1000u && (half & 1u)) ? half + 1 : half;
    }

    static double halfValue(u32 half) {
        return (1. + (half & 1023u) / 1024.) * exp2((int)((half >> 10) & 31u) - 15) * (half & 0x8000u ? -1. : 1.);
    }

    static u32 encode(double value, Levels levels, const VideoLayout& layout, int c) {
        int depth = layout.components[c][4];

        if (layout.floating) {
            return depth == 16 ? halfBits(value) : __builtin_bit_cast(u32, (float)value);
        }

        double top = exp2(depth) - 1.;
        double code = round(value * levels.scale + levels.offset);

        return (u32)(code < 0. ? 0. : code > top ? top : code);
    }

    static double decode(u32 code, Levels levels, const VideoLayout& layout, int c) {
        if (layout.floating) {
            return layout.components[c][4] == 16 ? halfValue(code) : __builtin_bit_cast(float, code);
        }

        return ((double)code - levels.offset) / levels.scale;
    }

    static int mirrored(int at, int extent) {
        int reflected = at < 0 ? -at : at;
        int distance = extent - 1 - reflected;

        return extent - 1 - (distance < 0 ? -distance : distance);
    }

    static int bayerRed(const AVPixFmtDescriptor* descriptor) {
        const char* pattern = descriptor->name + 6;
        int red = 0;

        while (pattern[red] != 'r') {
            red++;
        }

        return red;
    }

    static int bayerChannel(const AVPixFmtDescriptor* descriptor, int x, int y) {
        int cell = (y & 1) * 2 + (x & 1);
        int red = bayerRed(descriptor);

        return cell == red ? 0 : cell == 3 - red ? 2 : 1;
    }

    static void writeRgb48(AVFrame* frame, int x, int y, const double* rgb) {
        u16* pixel = (u16*)(frame->data[0] + y * frame->linesize[0]) + x * 3;

        for (int c = 0; c < 3; c++) {
            double value = rgb[c] < 0. ? 0. : rgb[c] > 1. ? 1. : rgb[c];

            pixel[c] = (u16)lround(value * 65535.);
        }
    }
}

void Ignored::run() {
}

FormatCheck::FormatCheck(ObjPool* pool_, Ui* ui_)
    : pool(pool_)
    , ui(ui_)
{
    pooledGuard(*pool, [this] {
        av_buffer_pool_uninit(&slots.pool);
    });
}

RenderShader& FormatCheck::compile(const VideoShaderCode& code) {
    for (const CompiledShader& known : compiled) {
        if (known.code == &code) {
            return *known.shader;
        }
    }

    RenderShader* shader = ui->compileShader(*pool, code.code, code.size);

    compiled.pushBack(CompiledShader{&code, shader});

    return *shader;
}

AVFrame* FormatCheck::frame(const Case& kase) {
    const int align[4] = {};
    AVFrame* frame = av_frame_alloc();

    frame->format = kase.format;
    frame->width = sampleWidth;
    frame->height = sampleHeight;
    frame->colorspace = kase.matrix;
    frame->color_range = kase.range;
    frame->color_trc = kase.transfer;
    frame->color_primaries = kase.primaries;
    frame->chroma_location = kase.location;
    slots.fill(frame, sampleWidth, sampleHeight, align);
    memZero(frame->buf[0]->data, frame->buf[0]->data + frame->buf[0]->size);

    return frame;
}

void FormatCheck::write(AVFrame* frame, const Case& kase) {
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(kase.format);
    const VideoLayout& layout = *layoutNamed(descriptor->name);
    Model model = modelOf(layout);
    bool alpha = layout.alpha;
    int count = layout.count;
    AVPixFmtDescriptor written = *descriptor;
    int stepX = 1 << descriptor->log2_chroma_w;
    int stepY = 1 << descriptor->log2_chroma_h;
    int chromaWidth = AV_CEIL_RSHIFT(sampleWidth, (int)descriptor->log2_chroma_w);
    int chromaHeight = AV_CEIL_RSHIFT(sampleHeight, (int)descriptor->log2_chroma_h);
    double siteX = within(kase.location, 0) * (stepX - 1);
    double siteY = within(kase.location, 1) * (stepY - 1);
    auto source = [&](int c, double x, double y) {
        double rgba[4];
        double ycc[3];

        for (int k = 0; k < 4; k++) {
            rgba[k] = sampleValue(x, y, k);
        }

        if (alpha && c == count - 1) {
            return rgba[3];
        }

        if (model == Model::Gray) {
            return 0.2126 * rgba[0] + 0.7152 * rgba[1] + 0.0722 * rgba[2];
        }

        if (model == Model::Yuv) {
            forward(kase, rgba, ycc);

            return ycc[c];
        }

        return rgba[c];
    };
    auto stored = [&](int c, double x, double y) {
        Levels scale = levels(kase, layout, c);

        return decode(encode(source(c, x, y), scale, layout, c), scale, layout, c);
    };

    written.flags = (layout.bigEndian ? AV_PIX_FMT_FLAG_BE : 0) | (layout.bits ? AV_PIX_FMT_FLAG_BITSTREAM : 0);

    for (int c = 0; c < count; c++) {
        written.comp[c] = {layout.components[c][0], layout.components[c][1], layout.components[c][2], layout.components[c][3], layout.components[c][4]};
    }

    expected.clear();

    if (model == Model::Bayer) {
        int step = layout.components[0][1];
        bool bigEndian = layout.bigEndian;
        double top = exp2(8 * step) - 1.;
        auto value = [&](int x, int y) {
            x = mirrored(x, sampleWidth);
            y = mirrored(y, sampleHeight);

            return round(sampleValue(x, y, bayerChannel(descriptor, x, y)) * top) / top;
        };

        for (int y = 0; y < sampleHeight; y++) {
            uint8_t* row = frame->data[0] + y * frame->linesize[0];

            for (int x = 0; x < sampleWidth; x++) {
                u32 code = (u32)lround(value(x, y) * top);

                if (step == 1) {
                    row[x] = (uint8_t)code;
                } else {
                    row[2 * x + (bigEndian ? 0 : 1)] = (uint8_t)(code >> 8);
                    row[2 * x + (bigEndian ? 1 : 0)] = (uint8_t)code;
                }
            }
        }

        for (int y = 0; y < sampleHeight; y++) {
            for (int x = 0; x < sampleWidth; x++) {
                int channel = bayerChannel(descriptor, x, y);
                double here = value(x, y);
                double horizontal = (value(x - 1, y) + value(x + 1, y)) / 2.;
                double vertical = (value(x, y - 1) + value(x, y + 1)) / 2.;
                double cross = (horizontal + vertical) / 2.;
                double diagonal = (value(x - 1, y - 1) + value(x + 1, y - 1) + value(x - 1, y + 1) + value(x + 1, y + 1)) / 4.;
                bool redRow = (y & 1) == bayerRed(descriptor) / 2;
                double rgb[3] = {here, cross, diagonal};

                if (channel == 2) {
                    rgb[0] = diagonal;
                    rgb[2] = here;
                } else if (channel == 1) {
                    rgb[0] = redRow ? horizontal : vertical;
                    rgb[1] = here;
                    rgb[2] = redRow ? vertical : horizontal;
                }

                for (int c = 0; c < 3; c++) {
                    expected.pushBack(rgb[c]);
                }

                expected.pushBack(1.);
            }
        }

        return;
    }

    for (int c = 0; c < count; c++) {
        bool chroma = model == Model::Yuv && (c == 1 || c == 2);
        int width = chroma ? chromaWidth : sampleWidth;
        int height = chroma ? chromaHeight : sampleHeight;
        Levels scale = levels(kase, layout, c);
        u32 line[sampleWidth];

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                double at = chroma ? x * stepX + siteX : x;
                double row = chroma ? y * stepY + siteY : y;

                line[x] = encode(source(c, at, row), scale, layout, c);

                if (kase.format == AV_PIX_FMT_MONOWHITE) {
                    line[x] ^= 1u;
                }

                if (kase.format == AV_PIX_FMT_UYYVYY411 && c == 0) {
                    frame->data[0][y * frame->linesize[0] + x / 4 * 6 + lumaBytes[x % 4]] = (uint8_t)line[x];
                }
            }

            if (!(kase.format == AV_PIX_FMT_UYYVYY411 && c == 0)) {
                av_write_image_line2(line, frame->data, frame->linesize, &written, 0, y, c, width, 4);
            }
        }
    }

    Matrix toSignal = {};

    if (model == Model::Yuv && (descriptor->log2_chroma_w || descriptor->log2_chroma_h)) {
        Matrix fromSignal = {};

        for (int k = 0; k < 3; k++) {
            double rgb[3] = {k == 0 ? 1. : 0., k == 1 ? 1. : 0., k == 2 ? 1. : 0.};
            double ycc[3];

            forward(kase, rgb, ycc);

            for (int c = 0; c < 3; c++) {
                fromSignal.m[c][k] = ycc[c];
            }
        }

        toSignal = invert(fromSignal);
    }

    for (int y = 0; y < sampleHeight; y++) {
        for (int x = 0; x < sampleWidth; x++) {
            double rgba[4] = {sampleValue(x, y, 0), sampleValue(x, y, 1), sampleValue(x, y, 2), alpha ? stored(count - 1, x, y) : 1.};

            if (model == Model::Gray) {
                rgba[0] = rgba[1] = rgba[2] = stored(0, x, y);
            } else if (model != Model::Yuv) {
                for (int c = 0; c < 3; c++) {
                    rgba[c] = stored(c, x, y);
                }
            } else if (descriptor->log2_chroma_w || descriptor->log2_chroma_h) {
                double chromaX = fmin(fmax((x - siteX) / stepX, 0.), chromaWidth - 1.) * stepX + siteX;
                double chromaY = fmin(fmax((y - siteY) / stepY, 0.), chromaHeight - 1.) * stepY + siteY;
                double ycc[3] = {source(0, x, y), source(1, chromaX, chromaY), source(2, chromaX, chromaY)};

                apply(toSignal, ycc, rgba);
            }

            for (int c = 0; c < 4; c++) {
                expected.pushBack(rgba[c]);
            }
        }
    }
}

void FormatCheck::writePalette(AVFrame* frame) {
    u32* entries = (u32*)frame->data[1];

    for (u32 i = 0; i < 256; i++) {
        entries[i] = (255 - i) << 24 | i << 16 | ((i * 7) & 255) << 8 | ((i * 13) & 255);
    }

    expected.clear();

    for (int y = 0; y < sampleHeight; y++) {
        for (int x = 0; x < sampleWidth; x++) {
            u32 index = (u32)((x * 5 + y * 3) & 255);
            u32 entry = entries[index];

            frame->data[0][y * frame->linesize[0] + x] = (uint8_t)index;
            expected.pushBack(pow(srgbDecode(((entry >> 16) & 255) / 255.), 1. / 2.4));
            expected.pushBack(pow(srgbDecode(((entry >> 8) & 255) / 255.), 1. / 2.4));
            expected.pushBack(pow(srgbDecode((entry & 255) / 255.), 1. / 2.4));
            expected.pushBack((entry >> 24) / 255.);
        }
    }
}

void FormatCheck::linearize(const Case& kase) {
    Matrix toScene = toBt2020(kase.primaries);

    for (size_t i = 0; i < samplePixels; i++) {
        double signal[3] = {expected[i * 4], expected[i * 4 + 1], expected[i * 4 + 2]};
        double light[3];
        double scene[3];

        displayLight(kase.transfer, kase.primaries, signal, light);
        apply(toScene, light, scene);

        for (int c = 0; c < 3; c++) {
            expected.mut(i * 4 + c) = scene[c];
        }
    }
}

void FormatCheck::shade(AVFrame* frame, const char* output, Vector<double>& out) {
    FrameShader shading = describeFrame(frame, RendererOptions{}.sdrWhiteNits);
    ScopedPtr<ObjPool> owner{ObjPool::fromMemoryRaw()};
    RenderImage* image = ui->shadeImage(*owner.ptr, compile(shaderCode(shading, output)), (u32)frame->width, (u32)frame->height, frame->buf[0]->data, frame->buf[0]->size, &shading.uniform, sizeof(shading.uniform), retired);
    ImagePixels pixels;

    image->prepare();
    image->read(0, 0, frame->width, frame->height, pixels);

    const float* got = (const float*)pixels.rgbaf.data();

    out.clear();

    for (size_t i = 0; i < samplePixels * 4; i++) {
        out.pushBack(got[i]);
    }
}

void FormatCheck::compare(StringView what, StringView output, const Vector<double>& got, double tolerance, bool relative) {
    double worst = 0.;
    size_t at = 0;

    for (size_t i = 0; i < samplePixels * 4; i++) {
        double error = fabs(got[i] - expected[i]) / (relative ? fmax(1., fabs(expected[i])) : 1.);

        if (!(error <= worst) && !isnan(worst)) {
            worst = error;
            at = i;
        }
    }

    checked++;

    if (!(worst <= tolerance)) {
        failed++;
        sysE << StringView(u8"play formats: ") << what << StringView(u8" ") << output << StringView(u8" is off by ") << worst << StringView(u8" at ") << (u64)(at / 4 % sampleWidth) << StringView(u8",") << (u64)(at / 4 / sampleWidth) << StringView(u8" channel ") << (u64)(at % 4) << StringView(u8", got ") << got[at] << StringView(u8" for ") << expected[at] << StringView(u8", allowed ") << tolerance << endL;
    }
}

void FormatCheck::verify(AVFrame* frame, StringView what, double tolerance) {
    Vector<double> got;

    shade(frame, "sdr", got);

    for (size_t i = 0; i < samplePixels * 4; i++) {
        if (i % 4 != 3) {
            got.mut(i) = pow(srgbDecode(got[i]), 1. / 2.4);
        }
    }

    compare(what, StringView(u8"sdr"), got, tolerance, false);
    shade(frame, "hdr", got);

    Matrix back = invert(toBt2020(1));

    for (size_t i = 0; i < samplePixels; i++) {
        double linear[3] = {got[i * 4], got[i * 4 + 1], got[i * 4 + 2]};
        double rgb[3];

        apply(back, linear, rgb);

        for (int c = 0; c < 3; c++) {
            got.mut(i * 4 + c) = pow(fmax(rgb[c], 0.), 1. / 2.4);
        }
    }

    compare(what, StringView(u8"hdr"), got, tolerance, false);
}

void FormatCheck::verifyLinear(AVFrame* frame, StringView what, double tolerance) {
    Vector<double> got;

    shade(frame, "hdr", got);
    compare(what, StringView(u8"hdr"), got, tolerance, true);
}

void FormatCheck::check(const Case& kase, StringView what) {
    const VideoLayout* layout = layoutNamed(av_get_pix_fmt_name(kase.format));

    if (!layout) {
        checked++;
        failed++;
        sysE << StringView(u8"play formats: ") << what << StringView(u8" has no layout") << endL;

        return;
    }

    Model model = modelOf(*layout);
    AVFrame* frame = this->frame(kase);
    STD_DEFER {
        av_frame_free(&frame);
    };

    if (model == Model::Palette) {
        writePalette(frame);
        frame->color_trc = AVCOL_TRC_IEC61966_2_1;
        verify(frame, what, readback);

        return;
    }

    write(frame, kase);

    if (model == Model::Xyz || kase.transfer != AVCOL_TRC_BT709 || kase.primaries != AVCOL_PRI_BT709) {
        Case shown = kase;

        if (model == Model::Xyz && kase.transfer == AVCOL_TRC_UNSPECIFIED) {
            shown.transfer = AVCOL_TRC_SMPTE428;
        }

        linearize(shown);
        verifyLinear(frame, what, readback);

        return;
    }

    double tolerance = readback;

    if (model == Model::Yuv) {
        Matrix fromSignal = {};
        double steps[3];

        for (int k = 0; k < 3; k++) {
            double rgb[3] = {k == 0 ? 1. : 0., k == 1 ? 1. : 0., k == 2 ? 1. : 0.};
            double ycc[3];

            forward(kase, rgb, ycc);

            for (int c = 0; c < 3; c++) {
                fromSignal.m[c][k] = ycc[c];
            }

            steps[k] = layout->floating ? 0. : 0.5 / levels(kase, *layout, k).scale;
        }

        Matrix toSignal = invert(fromSignal);

        for (int c = 0; c < 3; c++) {
            double bound = readback;

            for (int k = 0; k < 3; k++) {
                bound += fabs(toSignal.m[c][k]) * steps[k];
            }

            tolerance = fmax(tolerance, bound);
        }
    }

    verify(frame, what, tolerance);
}

void FormatCheck::formats() {
    for (const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_next(nullptr); descriptor; descriptor = av_pix_fmt_desc_next(descriptor)) {
        if (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) {
            continue;
        }

        Case kase;
        const VideoLayout* layout = layoutNamed(descriptor->name);
        Model model = layout ? modelOf(*layout) : Model::Rgb;

        kase.format = av_pix_fmt_desc_get_id(descriptor);
        kase.matrix = model == Model::Yuv ? AVCOL_SPC_BT709 : AVCOL_SPC_RGB;
        kase.range = model == Model::Yuv && !StringView(descriptor->name).startsWith(StringView(u8"yuvj")) ? AVCOL_RANGE_MPEG : AVCOL_RANGE_JPEG;

        if (model == Model::Xyz) {
            kase.transfer = AVCOL_TRC_UNSPECIFIED;
            kase.primaries = AVCOL_PRI_SMPTE428;
        }

        check(kase, StringView(descriptor->name));
    }
}

void FormatCheck::matrices() {
    const AVPixelFormat formats[] = {AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_YUV444P16LE, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUYV422, AV_PIX_FMT_Y210LE};
    const AVColorRange ranges[] = {AVCOL_RANGE_UNSPECIFIED, AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG};

    for (const VideoCode& matrix : videoMatrices) {
        if (strcmp(matrix.kind, "linear")) {
            continue;
        }

        for (AVColorRange range : ranges) {
            for (AVPixelFormat format : formats) {
                Case kase;

                kase.format = format;
                kase.matrix = (AVColorSpace)matrix.code;
                kase.range = range;
                check(kase, StringView(StringBuilder() << StringView(av_get_pix_fmt_name(format)) << StringView(u8" matrix ") << (i64)matrix.code << StringView(u8" range ") << (i64)range));
            }
        }
    }
}

void FormatCheck::locations() {
    const AVPixelFormat formats[] = {
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV411P, AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_NV12,
        AV_PIX_FMT_NV21, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUYV422, AV_PIX_FMT_UYVY422, AV_PIX_FMT_Y210LE, AV_PIX_FMT_UYYVYY411,
    };

    for (AVPixelFormat format : formats) {
        for (int location = AVCHROMA_LOC_UNSPECIFIED; location <= AVCHROMA_LOC_BOTTOM; location++) {
            Case kase;

            kase.format = format;
            kase.location = (AVChromaLocation)location;
            check(kase, StringView(StringBuilder() << StringView(av_get_pix_fmt_name(format)) << StringView(u8" chroma location ") << (i64)location));
        }
    }
}

void FormatCheck::systems() {
    for (const VideoCode& matrix : videoMatrices) {
        bool cl = !strcmp(matrix.kind, "cl");

        if (!cl && strcmp(matrix.kind, "ictcp")) {
            continue;
        }

        for (const VideoCode& transfer : videoTransfers) {
            if (!cl && transfer.code != AVCOL_TRC_SMPTE2084 && transfer.code != AVCOL_TRC_ARIB_STD_B67) {
                continue;
            }

            Case kase;

            kase.format = AV_PIX_FMT_YUV444P16LE;
            kase.matrix = (AVColorSpace)matrix.code;
            kase.range = AVCOL_RANGE_MPEG;
            kase.transfer = (AVColorTransferCharacteristic)transfer.code;
            kase.primaries = AVCOL_PRI_BT2020;
            check(kase, StringView(StringBuilder() << StringView(u8"yuv444p16le matrix ") << (i64)matrix.code << StringView(u8" transfer ") << (i64)transfer.code));
        }
    }
}

void FormatCheck::transfers() {
    float white = RendererOptions{}.sdrWhiteNits;

    for (const VideoCode& entry : videoTransfers) {
        Case kase;

        kase.format = AV_PIX_FMT_RGB48LE;
        kase.matrix = AVCOL_SPC_RGB;
        kase.range = AVCOL_RANGE_JPEG;
        kase.transfer = (AVColorTransferCharacteristic)entry.code;

        AVFrame* frame = this->frame(kase);
        STD_DEFER {
            av_frame_free(&frame);
        };
        bool absolute = entry.code == AVCOL_TRC_SMPTE2084 || entry.code == AVCOL_TRC_ARIB_STD_B67;
        bool gray = entry.code == AVCOL_TRC_ARIB_STD_B67;
        Matrix toScene = toBt2020(1);

        expected.clear();

        for (int y = 0; y < sampleHeight; y++) {
            for (int x = 0; x < sampleWidth; x++) {
                double linear[3];
                double encoded[3];
                double scene[3];

                for (int c = 0; c < 3; c++) {
                    double value = gray ? sampleValue(x, 0, 0) * sampleValue(0, y, 1) : sampleValue(x, y, c);

                    linear[c] = absolute ? value * 1000. / white : value;
                    encoded[c] = encodeTransfer(entry.code, linear[c], white);
                }

                writeRgb48(frame, x, y, encoded);
                apply(toScene, linear, scene);

                for (int c = 0; c < 3; c++) {
                    expected.pushBack(scene[c]);
                }

                expected.pushBack(1.);
            }
        }

        verifyLinear(frame, StringView(StringBuilder() << StringView(u8"rgb48le transfer ") << (i64)entry.code), 0.02);
    }
}

void FormatCheck::primaries() {
    for (uint8_t code : videoPrimaries) {
        Case kase;

        kase.format = AV_PIX_FMT_RGB48LE;
        kase.matrix = AVCOL_SPC_RGB;
        kase.range = AVCOL_RANGE_JPEG;
        kase.transfer = AVCOL_TRC_LINEAR;
        kase.primaries = (AVColorPrimaries)code;

        AVFrame* frame = this->frame(kase);
        STD_DEFER {
            av_frame_free(&frame);
        };
        Matrix toScene = toBt2020(code);

        expected.clear();

        for (int y = 0; y < sampleHeight; y++) {
            for (int x = 0; x < sampleWidth; x++) {
                double linear[3] = {sampleValue(x, y, 0), sampleValue(x, y, 1), sampleValue(x, y, 2)};
                double scene[3];

                writeRgb48(frame, x, y, linear);
                apply(toScene, linear, scene);

                for (int c = 0; c < 3; c++) {
                    expected.pushBack(scene[c]);
                }

                expected.pushBack(1.);
            }
        }

        verifyLinear(frame, StringView(StringBuilder() << StringView(u8"rgb48le primaries ") << (i64)code), 0.005);
    }
}

static int checkFormats(ObjPool& pool) {
    Ui& ui = *Ui::create(pool, StringView(u8"play-formats"), {64_d, 64_d});
    FormatCheck& check = *pool.make<FormatCheck>(&pool, &ui);
    bool crashed = false;
    auto body = makeRunable([&] {
        try {
            check.formats();
            check.matrices();
            check.locations();
            check.systems();
            check.transfers();
            check.primaries();
        } catch (...) {
            crashed = true;
            sysE << StringView(u8"play formats: ") << Exception::current() << endL;
        }
    });
    int result = ui.run(body);

    if (crashed || check.failed) {
        return 1;
    }

    sysO << StringView(u8"OK: ") << (u64)check.checked << StringView(u8" video color checks") << endL;

    return result;
}
#endif

int mainPlay(ObjPool& pool, int argc, char** argv) {
    if (argc != 2) {
        sysE << StringView(u8"usage: im play <file>") << endL;

        return 2;
    }

#if defined(IM_FOR_TESTS)
    if (StringView(argv[1]) == StringView(u8"--formats")) {
        return checkFormats(pool);
    }
#endif

    Ui& ui = *Ui::create(pool, StringView(u8"play"), {windowWidth, windowHeight});
    Player& player = *pool.make<Player>(pool, ui, argv[1]);
    auto body = makeRunable([&] {
        UiEvent event;

        while (ui.next(event)) {
            if (event.kind == UiEvent::Kind::Close || !player.screen->frame()) {
                return;
            }
        }
    });
    int result = ui.run(body);

    return player.screen->failed ? 1 : result;
}
