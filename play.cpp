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

#if defined(__APPLE__)
    #include <video_msl.h>
#else
    #include <video_spv.h>
#endif

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#if defined(IM_FOR_TESTS)
    #include <libavutil/opt.h>
    #include <libswscale/swscale.h>
#endif
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
        u32 plane[4];
        u32 step[4];
        u32 offset[4];
        u32 shift[4];
        u32 depth[4];
        u32 planeOffset[4];
        u32 lineSize[4];
        u32 size[4];
        u32 chroma[4];
        u32 color[4];
        float white[4];
    };

    struct FrameShader {
        const char* fetch = nullptr;
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

    static FrameShader describeFrame(const AVFrame* frame, float sdrWhiteNits) {
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
        const AVBufferRef* buffer = frame->buf[0];

        if (!descriptor || (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) || !buffer || frame->buf[1] || buffer->size % 4) {
            raiseError(StringView(StringBuilder() << StringView(u8"video frames of format ") << StringView(av_get_pix_fmt_name((AVPixelFormat)frame->format)) << StringView(u8" cannot be shown")));
        }

        FrameShader out;
        FrameUniform& u = out.uniform;
        bool rgb = descriptor->flags & AV_PIX_FMT_FLAG_RGB;
        bool alpha = descriptor->flags & AV_PIX_FMT_FLAG_ALPHA;
        int colors = descriptor->nb_components - (alpha ? 1 : 0);

        out.fetch = descriptor->flags & AV_PIX_FMT_FLAG_PAL ? "palette" : descriptor->flags & AV_PIX_FMT_FLAG_BITSTREAM ? "bitstream" : descriptor->flags & AV_PIX_FMT_FLAG_FLOAT ? "float" : "integer";

        for (int c = 0; c < descriptor->nb_components; c++) {
            const AVComponentDescriptor& comp = descriptor->comp[c];
            bool chroma = (c == 1 || c == 2) && !rgb && colors >= 3;
            u64 width = (u64)(chroma ? AV_CEIL_RSHIFT(frame->width, (int)descriptor->log2_chroma_w) : frame->width);
            u64 height = (u64)(chroma ? AV_CEIL_RSHIFT(frame->height, (int)descriptor->log2_chroma_h) : frame->height);
            u64 bits = comp.shift + comp.depth;
            u64 row = descriptor->flags & AV_PIX_FMT_FLAG_BITSTREAM ? ((width - 1) * comp.step + comp.offset) / 8 + 1 : (width - 1) * comp.step + comp.offset + (bits > 16 || (descriptor->flags & AV_PIX_FMT_FLAG_FLOAT) ? 4 : bits > 8 ? 2 : 1 + ((descriptor->flags & AV_PIX_FMT_FLAG_BE) ? 1 : 0));
            const uint8_t* data = frame->data[comp.plane];

            if (!data || frame->linesize[comp.plane] <= 0 || data < buffer->data || (u64)(data - buffer->data) + (height - 1) * (u64)frame->linesize[comp.plane] + row > buffer->size) {
                raiseError(StringView(u8"video frame planes exceed their buffer"));
            }

            u.plane[c] = (u32)comp.plane;
            u.step[c] = (u32)comp.step;
            u.offset[c] = (u32)comp.offset;
            u.shift[c] = (u32)comp.shift;
            u.depth[c] = (u32)comp.depth;
        }

        for (int p = 0; p < 4; p++) {
            u.planeOffset[p] = frame->data[p] ? (u32)(frame->data[p] - buffer->data) : 0;
            u.lineSize[p] = frame->data[p] ? (u32)frame->linesize[p] : 0;
        }

        u.size[0] = (u32)frame->width;
        u.size[1] = (u32)frame->height;
        u.size[2] = (u32)descriptor->nb_components;
        u.size[3] = ((descriptor->flags & AV_PIX_FMT_FLAG_BE) ? 1u : 0u) | (rgb ? 2u : 0u) | (alpha ? 4u : 0u);
        u.chroma[0] = descriptor->log2_chroma_w;
        u.chroma[1] = descriptor->log2_chroma_h;
        u.chroma[2] = (u32)frame->chroma_location;
        u.color[0] = (u32)frame->colorspace;
        u.color[1] = frame->color_range == AVCOL_RANGE_UNSPECIFIED && StringView(descriptor->name).startsWith(StringView(u8"yuvj")) ? (u32)AVCOL_RANGE_JPEG : (u32)frame->color_range;
        u.color[2] = (u32)frame->color_trc;
        u.color[3] = (u32)frame->color_primaries;
        u.white[0] = sdrWhiteNits;

        if (!listed(videoMatrices, sizeof(videoMatrices), frame->colorspace)) {
            unsupported("matrix", frame->colorspace);
        }

        if (!listed(videoRanges, sizeof(videoRanges), (int)u.color[1])) {
            unsupported("range", (int)u.color[1]);
        }

        if (!listed(videoPrimaries, sizeof(videoPrimaries), frame->color_primaries)) {
            unsupported("primaries", frame->color_primaries);
        }

        if (!listed(videoLocations, sizeof(videoLocations), frame->chroma_location)) {
            unsupported("chroma location", frame->chroma_location);
        }

        for (const VideoTransferCode& entry : videoTransfers) {
            if (entry.code == frame->color_trc) {
                out.transfer = entry.kind;
            }
        }

        if (!out.transfer) {
            unsupported("transfer", frame->color_trc);
        }

        return out;
    }

    static const VideoShaderCode& shaderCode(const FrameShader& frame, const char* output) {
        for (const VideoShaderCode& code : videoShaders) {
            if (!strcmp(code.fetch, frame.fetch) && !strcmp(code.transfer, frame.transfer) && !strcmp(code.output, output)) {
                return code;
            }
        }

        raiseError(StringView(u8"no video shader for this frame"));
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
    player->ui->trace(StringView(StringBuilder() << StringView(u8"compiled video shader ") << StringView(code.fetch) << StringView(u8" ") << StringView(code.transfer) << StringView(u8" ") << StringView(code.output)));

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

    constexpr const char* formatNames[] = {
        "abgr",
        "argb",
        "ayuv64le",
        "bgr0",
        "bgr24",
        "bgr48le",
        "bgr565le",
        "bgra",
        "gbrap",
        "gbrap10le",
        "gbrap12le",
        "gbrap14le",
        "gbrap16le",
        "gbrapf32le",
        "gbrp",
        "gbrp10le",
        "gbrp12le",
        "gbrp14le",
        "gbrp16le",
        "gbrp9le",
        "gbrpf32le",
        "gray10le",
        "gray12le",
        "gray14le",
        "gray16be",
        "gray16le",
        "gray",
        "gray9le",
        "grayf32le",
        "monob",
        "nv12",
        "nv16",
        "nv24",
        "p010le",
        "p012le",
        "p016le",
        "p210le",
        "p212le",
        "p216le",
        "p410le",
        "p412le",
        "p416le",
        "pal8",
        "rgb0",
        "rgb24",
        "rgb48be",
        "rgb48le",
        "rgb565le",
        "rgba",
        "rgba64be",
        "rgba64le",
        "uyvy422",
        "x2bgr10le",
        "x2rgb10le",
        "xv36le",
        "y210le",
        "y212le",
        "ya16be",
        "ya8",
        "yuv410p",
        "yuv411p",
        "yuv420p",
        "yuv420p10le",
        "yuv420p12le",
        "yuv420p14le",
        "yuv420p16le",
        "yuv420p9le",
        "yuv422p",
        "yuv422p10le",
        "yuv422p12le",
        "yuv422p14le",
        "yuv422p16le",
        "yuv422p9le",
        "yuv440p",
        "yuv440p10le",
        "yuv440p12le",
        "yuv444p",
        "yuv444p10le",
        "yuv444p12le",
        "yuv444p14le",
        "yuv444p16le",
        "yuv444p9le",
        "yuva420p",
        "yuva420p10le",
        "yuva420p16le",
        "yuva420p9le",
        "yuva422p",
        "yuva422p10le",
        "yuva422p12le",
        "yuva422p16le",
        "yuva422p9le",
        "yuva444p",
        "yuva444p10le",
        "yuva444p12le",
        "yuva444p16le",
        "yuva444p9le",
        "yuvj411p",
        "yuvj420p",
        "yuvj422p",
        "yuvj440p",
        "yuvj444p",
        "yuyv422",
    };

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

    struct Matrix {
        double m[3][3];
    };

    struct Ignored final: public Runable {
        void run() override;
    };

    struct Case {
        AVPixelFormat format = AV_PIX_FMT_NONE;
        AVColorSpace matrix = AVCOL_SPC_BT709;
        AVColorRange range = AVCOL_RANGE_MPEG;
        AVColorTransferCharacteristic transfer = AVCOL_TRC_BT709;
        AVColorPrimaries primaries = AVCOL_PRI_BT709;
        AVChromaLocation location = AVCHROMA_LOC_LEFT;
    };

    struct FormatCheck {
        ObjPool* pool;
        Ui* ui;
        AVFrame* source = nullptr;
        Slots slots;
        Vector<CompiledShader> compiled;
        Ignored retired;
        Vector<double> expected;
        int checked = 0;
        int failed = 0;

        FormatCheck(ObjPool* pool, Ui* ui);
        RenderShader& compile(const VideoShaderCode& code);
        AVFrame* frame(const Case& kase);
        void scale(AVFrame* frame, const Case& kase);
        void shade(AVFrame* frame, const char* output, Vector<double>& out);
        void compare(StringView what, StringView output, const Vector<double>& got, double tolerance, bool relative);
        void verify(AVFrame* frame, StringView what, double tolerance);
        void verifyLinear(AVFrame* frame, StringView what, double tolerance);
        void expectSource(bool alpha);
        void formats();
        void matrices();
        void locations();
        void transfers();
        void primaries();
    };

    static double sampleValue(int x, int y, int c) {
        double u = (double)x / (sampleWidth - 1);
        double v = (double)y / (sampleHeight - 1);
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

    static int componentDepth(AVPixelFormat format) {
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
        int depth = 16;

        for (int c = 0; c < descriptor->nb_components; c++) {
            depth = descriptor->comp[c].depth < depth ? descriptor->comp[c].depth : depth;
        }

        return depth;
    }

    static double tolerance(AVPixelFormat format) {
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
        bool exact = descriptor->flags & (AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_FLOAT);
        bool subsampled = descriptor->log2_chroma_w || descriptor->log2_chroma_h;

        return exact ? 0.003 : 4. / (exp2(componentDepth(format)) - 1.) + 0.006 + (subsampled ? 0.01 : 0.);
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
    , source(av_frame_alloc())
{
    pooledGuard(*pool, [this] {
        av_buffer_pool_uninit(&slots.pool);
        av_frame_free(&source);
    });

    source->format = AV_PIX_FMT_RGBA64LE;
    source->width = sampleWidth;
    source->height = sampleHeight;

    if (av_frame_get_buffer(source, 0) < 0) {
        fail(StringView(u8"cannot allocate the test picture"));
    }

    for (int y = 0; y < sampleHeight; y++) {
        u16* row = (u16*)(source->data[0] + y * source->linesize[0]);

        for (int x = 0; x < sampleWidth; x++) {
            for (int c = 0; c < 4; c++) {
                row[x * 4 + c] = (u16)lround(sampleValue(x, y, c) * 65535.);
            }
        }
    }
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

    return frame;
}

void FormatCheck::scale(AVFrame* frame, const Case& kase) {
    static constexpr double positions[7][2] = {{0., 0.5}, {0., 0.5}, {0.5, 0.5}, {0., 0.}, {0.5, 0.}, {0., 1.}, {0.5, 1.}};
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(kase.format);
    SwsContext* sws = sws_alloc_context();
    STD_DEFER {
        sws_freeContext(sws);
    };

    av_opt_set_int(sws, "srcw", sampleWidth, 0);
    av_opt_set_int(sws, "srch", sampleHeight, 0);
    av_opt_set_int(sws, "src_format", AV_PIX_FMT_RGBA64LE, 0);
    av_opt_set_int(sws, "dstw", sampleWidth, 0);
    av_opt_set_int(sws, "dsth", sampleHeight, 0);
    av_opt_set_int(sws, "dst_format", kase.format, 0);
    av_opt_set_int(sws, "sws_flags", SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP | SWS_BITEXACT, 0);
    av_opt_set(sws, "sws_dither", "none", 0);
    av_opt_set_int(sws, "dst_h_chr_pos", lround(positions[kase.location][0] * ((1 << descriptor->log2_chroma_w) - 1) * 256.), 0);
    av_opt_set_int(sws, "dst_v_chr_pos", lround(positions[kase.location][1] * ((1 << descriptor->log2_chroma_h) - 1) * 256.), 0);

    if (sws_init_context(sws, nullptr, nullptr) < 0) {
        fail(StringView(StringBuilder() << StringView(u8"swscale cannot produce ") << StringView(descriptor->name)));
    }

    sws_setColorspaceDetails(sws, sws_getCoefficients(SWS_CS_ITU709), 1, sws_getCoefficients(kase.matrix), kase.range == AVCOL_RANGE_JPEG, 0, 1 << 16, 1 << 16);
    sws_scale(sws, source->data, source->linesize, 0, sampleHeight, frame->data, frame->linesize);
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

        if (!(error <= worst)) {
            worst = error;
            at = i;
        }
    }

    checked++;

    if (!(worst <= tolerance)) {
        failed++;
        sysE << StringView(u8"play formats: ") << what << StringView(u8" ") << output << StringView(u8" is off by ") << worst << StringView(u8" at ") << (u64)(at / 4 % sampleWidth) << StringView(u8",") << (u64)(at / 4 / sampleWidth) << StringView(u8" channel ") << (u64)(at % 4) << StringView(u8", allowed ") << tolerance << endL;
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

void FormatCheck::expectSource(bool alpha) {
    expected.clear();

    for (int y = 0; y < sampleHeight; y++) {
        for (int x = 0; x < sampleWidth; x++) {
            for (int c = 0; c < 3; c++) {
                expected.pushBack(sampleValue(x, y, c));
            }

            expected.pushBack(alpha ? sampleValue(x, y, 3) : 1.);
        }
    }
}

void FormatCheck::formats() {
    for (const char* name : formatNames) {
        Case kase;

        kase.format = av_get_pix_fmt(name);

        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(kase.format);
        bool rgb = descriptor->flags & AV_PIX_FMT_FLAG_RGB;
        bool alpha = descriptor->flags & AV_PIX_FMT_FLAG_ALPHA;
        bool palette = descriptor->flags & AV_PIX_FMT_FLAG_PAL;
        bool bitstream = descriptor->flags & AV_PIX_FMT_FLAG_BITSTREAM;
        bool gray = descriptor->nb_components - (alpha ? 1 : 0) == 1 && !palette;

        kase.matrix = rgb ? AVCOL_SPC_RGB : AVCOL_SPC_BT709;
        kase.range = rgb || palette || StringView(name).startsWith(StringView(u8"yuvj")) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;

        AVFrame* frame = this->frame(kase);
        STD_DEFER {
            av_frame_free(&frame);
        };

        if (palette) {
            u32* entries = (u32*)frame->data[1];

            for (u32 i = 0; i < 256; i++) {
                entries[i] = (255 - i) << 24 | i << 16 | ((i * 7) & 255) << 8 | ((i * 13) & 255);
            }

            for (int y = 0; y < sampleHeight; y++) {
                for (int x = 0; x < sampleWidth; x++) {
                    frame->data[0][y * frame->linesize[0] + x] = (uint8_t)((x * 5 + y * 3) & 255);
                }
            }
        } else {
            scale(frame, kase);
        }

        expectSource(alpha);

        for (int y = 0; y < sampleHeight; y++) {
            for (int x = 0; x < sampleWidth; x++) {
                size_t at = ((size_t)y * sampleWidth + x) * 4;
                double value[4] = {expected[at], expected[at + 1], expected[at + 2], expected[at + 3]};

                if (palette) {
                    u32 entry = ((const u32*)frame->data[1])[frame->data[0][y * frame->linesize[0] + x]];

                    value[0] = pow(srgbDecode(((entry >> 16) & 255) / 255.), 1. / 2.4);
                    value[1] = pow(srgbDecode(((entry >> 8) & 255) / 255.), 1. / 2.4);
                    value[2] = pow(srgbDecode((entry & 255) / 255.), 1. / 2.4);
                    value[3] = (entry >> 24) / 255.;
                } else if (bitstream) {
                    value[0] = value[1] = value[2] = (frame->data[0][y * frame->linesize[0] + x / 8] >> (7 - x % 8)) & 1;
                } else if (gray) {
                    value[0] = value[1] = value[2] = 0.2126 * value[0] + 0.7152 * value[1] + 0.0722 * value[2];
                }

                for (int c = 0; c < 4; c++) {
                    expected.mut(at + c) = value[c];
                }
            }
        }

        if (palette) {
            frame->color_trc = AVCOL_TRC_IEC61966_2_1;
        }

        verify(frame, StringView(name), tolerance(kase.format));
    }
}

void FormatCheck::matrices() {
    const char* const names[] = {"yuv444p", "yuv444p10le", "yuv444p12le", "yuv444p16le"};
    const AVColorSpace matrices[] = {AVCOL_SPC_BT709, AVCOL_SPC_FCC, AVCOL_SPC_BT470BG, AVCOL_SPC_SMPTE170M, AVCOL_SPC_SMPTE240M, AVCOL_SPC_BT2020_NCL};
    const AVColorRange ranges[] = {AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG};
    const AVColorSpace direct[] = {AVCOL_SPC_RGB, AVCOL_SPC_YCGCO};

    for (const char* name : names) {
        for (AVColorSpace matrix : matrices) {
            for (AVColorRange range : ranges) {
                Case kase;

                kase.format = av_get_pix_fmt(name);
                kase.matrix = matrix;
                kase.range = range;

                AVFrame* frame = this->frame(kase);
                STD_DEFER {
                    av_frame_free(&frame);
                };

                scale(frame, kase);
                expectSource(false);
                verify(frame, StringView(StringBuilder() << StringView(name) << StringView(u8" matrix ") << (i64)matrix << StringView(u8" range ") << (i64)range), tolerance(kase.format));
            }
        }
    }

    for (AVColorSpace matrix : direct) {
        Case kase;

        kase.format = AV_PIX_FMT_YUV444P16LE;
        kase.matrix = matrix;
        kase.range = AVCOL_RANGE_JPEG;

        AVFrame* frame = this->frame(kase);
        STD_DEFER {
            av_frame_free(&frame);
        };

        for (int y = 0; y < sampleHeight; y++) {
            for (int x = 0; x < sampleWidth; x++) {
                double r = sampleValue(x, y, 0);
                double g = sampleValue(x, y, 1);
                double b = sampleValue(x, y, 2);
                double planes[3] = {g, b, r};

                if (matrix == AVCOL_SPC_YCGCO) {
                    planes[0] = 0.25 * r + 0.5 * g + 0.25 * b;
                    planes[1] = -0.25 * r + 0.5 * g - 0.25 * b + 32768. / 65535.;
                    planes[2] = 0.5 * r - 0.5 * b + 32768. / 65535.;
                }

                for (int p = 0; p < 3; p++) {
                    ((u16*)(frame->data[p] + y * frame->linesize[p]))[x] = (u16)lround(planes[p] * 65535.);
                }
            }
        }

        expectSource(false);
        verify(frame, StringView(StringBuilder() << StringView(u8"yuv444p16le matrix ") << (i64)matrix), tolerance(kase.format));
    }
}

void FormatCheck::locations() {
    const char* const names[] = {"yuv420p", "yuv422p", "yuv440p", "yuv411p", "yuv410p", "yuv420p10le", "nv12", "p010le"};

    for (const char* name : names) {
        for (int location = AVCHROMA_LOC_UNSPECIFIED; location <= AVCHROMA_LOC_BOTTOM; location++) {
            Case kase;

            kase.format = av_get_pix_fmt(name);
            kase.location = (AVChromaLocation)location;

            AVFrame* frame = this->frame(kase);
            STD_DEFER {
                av_frame_free(&frame);
            };

            scale(frame, kase);
            expectSource(false);
            verify(frame, StringView(StringBuilder() << StringView(name) << StringView(u8" chroma location ") << (i64)location), tolerance(kase.format));
        }
    }
}

void FormatCheck::transfers() {
    float white = RendererOptions{}.sdrWhiteNits;

    for (const VideoTransferCode& entry : videoTransfers) {
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
