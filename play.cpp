#include "play.h"

#include "ui.h"
#include "error.h"
#include "pooled.h"

#include <std/ios/sys.h>
#include <std/sys/crt.h>
#include <std/lib/list.h>
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
#include <plt/platform.h>
#include <plt/loop_wake.h>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
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
    constexpr size_t surfaceCount = 10;
    constexpr size_t rowAlignment = 256;
    constexpr size_t slotAlignment = 64u << 10;
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
        Resize,
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
    struct Surfaces;

    struct VideoImage final: public Runable {
        Player* player;
        Surfaces* set;
        uint8_t* data;
        size_t stride;
        u32 width;
        u32 height;
        RenderImage* render;
        u32 draws = 0;

        VideoImage(Player* player, Surfaces* set, ObjPool& pool, uint8_t* data, size_t size, size_t stride, u32 width, u32 height);
        void run() override;
    };

    struct Surfaces: public IntrusiveNode {
        ObjPool* pool;
        u32 width;
        u32 height;
        size_t home = 0;
        VideoImage* images[surfaceCount] = {};

        Surfaces(Player* player, ObjPool* pool, u32 width, u32 height);
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

    struct Resize final: public Typed<Kind::Resize> {
        u32 width;
        u32 height;

        Resize(u32 width, u32 height);
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

        Stream(Player* player, AVMediaType type);
        void seek(double position);
        int receive();
        double seconds(i64 timestamp) const;
        double timeBase() const;
    };

    struct Video final: public Runable {
        Player* player;
        Stream stream;
        SwsContext* scaler = nullptr;
        Vector<VideoImage*> idle;
        u64 generation = 1;
        double target = 0.;
        double pts = 0.;
        double next = 0.;
        u32 wantWidth = 0;
        u32 wantHeight = 0;
        u32 askedWidth = 0;
        u32 askedHeight = 0;
        bool decoded = false;
        bool ended = false;

        explicit Video(Player* player);
        void run() override;
        void apply(const Control& control);
        void give(VideoImage* image);
        void refuse();
        bool step();
        bool deliver();
        void convert(VideoImage& image);
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
        Surfaces* surfaces = nullptr;
        IntrusiveList sets;
        Vector<Surfaces*> spent;
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
        void makeSurfaces(u32 width, u32 height);
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

Resize::Resize(u32 width_, u32 height_)
    : width(width_)
    , height(height_)
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

VideoImage::VideoImage(Player* player_, Surfaces* set_, ObjPool& pool, uint8_t* data_, size_t size, size_t stride_, u32 width_, u32 height_)
    : player(player_)
    , set(set_)
    , data(data_)
    , stride(stride_)
    , width(width_)
    , height(height_)
    , render(player_->ui->bindImage(pool, width_, height_, data_, size, stride_, *this))
{
}

void VideoImage::run() {
    player->screen->retired(this);
}

Surfaces::Surfaces(Player* player, ObjPool* pool_, u32 width_, u32 height_)
    : pool(pool_)
    , width(width_)
    , height(height_)
{
    checkImageSize(width, height, player->ui->maxTextureSide());

    size_t stride = ((size_t)width * 4 + rowAlignment - 1) / rowAlignment * rowAlignment;
    size_t slot = (stride * height + slotAlignment - 1) / slotAlignment * slotAlignment;
    size_t bytes = slot * surfaceCount;
    void* base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (base == MAP_FAILED) {
        Errno().raise(StringBuilder() << StringView(u8"cannot map video buffers"));
    }

    pooledGuard(*pool, [base, bytes] {
        munmap(base, bytes);
    });
#if defined(__linux__)
    madvise(base, bytes, MADV_HUGEPAGE);
#endif

    for (size_t i = 0; i < surfaceCount; i++) {
        images[i] = pool->make<VideoImage>(player, this, *pool, (uint8_t*)base + i * slot, slot, stride, width, height);
    }

    player->ui->trace(StringView(StringBuilder() << StringView(u8"mapped ") << (i64)bytes << StringView(u8" bytes for ") << (i64)width << StringView(u8"x") << (i64)height));
}

Stream::Stream(Player* player, AVMediaType type) {
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
    , stream(player_, AVMEDIA_TYPE_VIDEO)
{
    pooledGuard(*player->pool, [this] {
        sws_freeContext(scaler);
    });
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
                give(surface->image);
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
    decoded = false;
    ended = false;
}

void Video::give(VideoImage* image) {
    if (wantWidth && (image->width != wantWidth || image->height != wantHeight)) {
        player->post(new Surface(image));

        return;
    }

    idle.pushBack(image);
}

void Video::refuse() {
    Vector<VideoImage*> kept;

    for (VideoImage* image : idle) {
        if (image->width == wantWidth && image->height == wantHeight) {
            kept.pushBack(image);
        } else {
            player->post(new Surface(image));
        }
    }

    idle.xchg(kept);
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
        av_frame_unref(frame);

        return true;
    }

    decoded = true;
    deliver();

    return true;
}

bool Video::deliver() {
    AVFrame* frame = stream.frame;
    u32 width = (u32)frame->width;
    u32 height = (u32)frame->height;

    if (width != wantWidth || height != wantHeight) {
        wantWidth = width;
        wantHeight = height;
        refuse();
    }

    if (idle.empty()) {
        if (width != askedWidth || height != askedHeight) {
            askedWidth = width;
            askedHeight = height;
            player->post(new Resize(width, height));
        }

        return false;
    }

    VideoImage* image = idle.popBack();
    AVRational sar = frame->sample_aspect_ratio;

    convert(*image);
    image->render->prepare();
    player->post(new Frame(image, generation, pts, (double)width / height * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.)));
    av_frame_unref(frame);
    decoded = false;

    return true;
}

void Video::convert(VideoImage& image) {
    AVFrame* frame = stream.frame;
    AVPixelFormat input = (AVPixelFormat)frame->format;

    scaler = sws_getCachedContext(scaler, frame->width, frame->height, input, frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!scaler) {
        raiseError(StringView(StringBuilder() << StringView(u8"cannot convert video frames of format ") << StringView(av_get_pix_fmt_name(input))));
    }

    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(input);

    if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_RGB)) {
        int space = SWS_CS_DEFAULT;

        if (frame->colorspace == AVCOL_SPC_BT709) {
            space = SWS_CS_ITU709;
        } else if (frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL) {
            space = SWS_CS_BT2020;
        }

        sws_setColorspaceDetails(scaler, sws_getCoefficients(space), frame->color_range == AVCOL_RANGE_JPEG, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    }

    uint8_t* planes[4] = {image.data, nullptr, nullptr, nullptr};
    int strides[4] = {(int)image.stride, 0, 0, 0};

    sws_scale(scaler, (const uint8_t* const*)frame->data, frame->linesize, 0, frame->height, planes, strides);
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

    while (!sets.empty()) {
        delete static_cast<Surfaces*>(sets.popFront())->pool;
    }
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
        } else if (Resize* resize = cast<Resize>(message.ptr)) {
            makeSurfaces(resize->width, resize->height);
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
    for (Surfaces* set : spent) {
        player->ui->trace(StringView(StringBuilder() << StringView(u8"unmapped ") << (i64)set->width << StringView(u8"x") << (i64)set->height));
        set->unlink();
        delete set->pool;
    }

    spent.clear();

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

    Surfaces* set = image->set;

    if (set == surfaces) {
        player->videoInbox->enqueue(new Surface(image));

        return;
    }

    if (++set->home == surfaceCount) {
        spent.pushBack(set);
        player->wake->signal();
    }
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

void Screen::makeSurfaces(u32 width, u32 height) {
    ScopedPtr<ObjPool> owner{ObjPool::fromMemoryRaw()};
    Surfaces* set = owner->make<Surfaces>(player, owner.ptr, width, height);

    owner.drop();
    sets.pushBack(set);
    surfaces = set;

    for (VideoImage* image : set->images) {
        release(image);
    }
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

int mainPlay(ObjPool& pool, int argc, char** argv) {
    if (argc != 2) {
        sysE << StringView(u8"usage: im play <file>") << endL;

        return 2;
    }

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
