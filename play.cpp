#include "play.h"

#include "ui.h"
#include "error.h"
#include "pooled.h"

#include <std/ios/sys.h>
#include <std/sys/crt.h>
#include <std/lib/list.h>
#include <std/sys/throw.h>
#include <std/dbg/verify.h>
#include <std/lib/vector.h>
#include <std/thr/thread.h>
#include <std/str/builder.h>
#include <std/thr/channel.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <stdlib.h>
#include <string.h>
#include <AL/alext.h>
#include <plt/fiber.h>
#include <plt/poller.h>
#include <plt/platform.h>
#include <plt/loop_wake.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

using namespace stl;

namespace {
    constexpr size_t imageCount = 10;
    constexpr size_t packetCount = 16;
    // At most 32 packet slots, 10 image slots, one control per worker and
    // one audio clock/event token can be in transit. Data cannot consume
    // the space needed by control messages or return messages.
    constexpr size_t channelCapacity = 128;
    constexpr int audioRate = 48000;
    constexpr int audioBufferCount = 6;
    constexpr int audioBufferSamples = 1024;

    enum class Kind {
        Seek,
        Playback,
        Packet,
        Stream,
        File,
        Image,
        Resize,
        Clock,
        AudioPulse,
        End,
        Failure
    };

    struct Message: public IntrusiveNode {
        ObjPool* owner = nullptr;
        virtual Kind messageKind() const = 0;
    };

    template <typename T>
    static T* cast(Message* message) {
        return message && message->messageKind() == T::kind ? static_cast<T*>(message) : nullptr;
    }

    template <typename T>
    static T* makeMessage() {
        ObjPool* pool = ObjPool::fromMemoryRaw();
        T* message = pool->make<T>();
        message->owner = pool;
        return message;
    }

    static void discard(Message* message) {
        delete message->owner;
    }

    static void send(Channel* channel, Message* message) {
        STD_VERIFY(channel->tryEnqueue(message));
    }

    static void ffcheck(int status) {
        if (status < 0) {
            char text[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(status, text, sizeof(text));
            fail(StringView(text));
        }
    }

    struct Mailbox {
        Channel* channel = nullptr;
        plt::LoopWake* wake = nullptr;
        void send(Message* message) const;
    };

    struct Seek final: public Message {
        static constexpr Kind kind = Kind::Seek;
        int worker = 0;
        u64 generation = 0;
        double position = 0;
        bool playing = false;
        Kind messageKind() const override;
    };

    struct Playback final: public Message {
        static constexpr Kind kind = Kind::Playback;
        int worker = 0;
        u64 generation = 0;
        bool playing = false;
        Kind messageKind() const override;
    };

    struct Packet final: public Message {
        static constexpr Kind kind = Kind::Packet;
        AVPacket* packet = nullptr;
        int stream = 0;
        u64 generation = 0;
        bool eof = false;
        Packet();
        ~Packet() noexcept;
        Kind messageKind() const override;
    };

    struct Stream final: public Message {
        static constexpr Kind kind = Kind::Stream;
        AVCodecParameters* parameters = nullptr;
        AVRational timeBase{};
        AVRational frameRate{};
        double origin = 0;
        Stream();
        ~Stream() noexcept;
        Kind messageKind() const override;
    };

    struct File final: public Message {
        static constexpr Kind kind = Kind::File;
        double duration = 0;
        u32 width = 0;
        u32 height = 0;
        bool audio = false;
        bool video = false;
        Kind messageKind() const override;
    };

    struct VideoImage final: public Message {
        static constexpr Kind kind = Kind::Image;
        RenderImage* image = nullptr;
        void* data = nullptr;
        size_t size = 0;
        size_t stride = 0;
        ObjPool* pixels = nullptr;
        u32 width = 0;
        u32 height = 0;
        double pts = 0;
        double duration = 0;
        double aspect = 1;
        u64 generation = 0;
        // These two flags belong to the UI, throughout the GPU use. The
        // producer receives the image only after both have been cleared.
        bool held = false;
        bool busy = false;
        Runable* retired = nullptr;
        Kind messageKind() const override;
    };

    struct Resize final: public Message {
        static constexpr Kind kind = Kind::Resize;
        VideoImage* image = nullptr;
        u32 width = 0;
        u32 height = 0;
        Kind messageKind() const override;
    };

    struct Clock final: public Message {
        static constexpr Kind kind = Kind::Clock;
        u64 generation = 0;
        u64 at = 0;
        double position = 0;
        double limit = 0;
        bool running = false;
        bool ended = false;
        Kind messageKind() const override;
    };

    struct AudioPulse final: public Message {
        static constexpr Kind kind = Kind::AudioPulse;
        Kind messageKind() const override;
    };

    struct End final: public Message {
        static constexpr Kind kind = Kind::End;
        u64 generation = 0;
        double position = 0;
        Kind messageKind() const override;
    };

    struct Failure final: public Message {
        static constexpr Kind kind = Kind::Failure;
        Buffer text;
        Kind messageKind() const override;
    };

    struct Worker: public Runable {
        Channel* input = nullptr;
        Mailbox output;
        u64 generation = 1;
        bool playing = false;
        void run() override;
        virtual void start(ObjPool& pool) = 0;
        virtual void accept(Message* message) = 0;
        virtual bool step() = 0;
        virtual void seek(double position) = 0;
        virtual void playback() = 0;
        bool control(Message* message);
    };

    struct Demux final: public Worker {
        const char* path = nullptr;
        Channel* streams[2]{};
        AVFormatContext* format = nullptr;
        AVPacket* pending = nullptr;
        IntrusiveList available[2];
        int indexes[2] = {-1, -1};
        bool ended[2]{};
        bool eof = false;
        bool pendingReady = false;
        double origin = 0;
        void start(ObjPool& pool) override;
        void accept(Message* message) override;
        bool step() override;
        void seek(double position) override;
        void playback() override;
    };

    struct Decoder: public Worker {
        Channel* demux = nullptr;
        AVCodecContext* codec = nullptr;
        AVFrame* frame = nullptr;
        AVRational timeBase{};
        double origin = 0;
        double target = 0;
        IntrusiveList packets;
        bool draining = false;
        bool exhausted = false;
        void start(ObjPool& pool) override;
        void open(Stream& stream);
        void reset(double position);
        void returnPacket(Packet* packet);
        // One decoder call per step, returning to the mailbox between calls.
        bool feed();
    };

    struct Video final: public Decoder {
        IntrusiveList images;
        SwsContext* scaler = nullptr;
        bool frameReady = false;
        bool preview = true;
        double frameDuration = 1.0 / 25;
        double nextPts = 0;
        double endPts = 0;
        void accept(Message* message) override;
        bool step() override;
        void seek(double position) override;
        void playback() override;
    };

    struct AudioEvent {
        Channel* input = nullptr;
        Channel* token = nullptr;
    };

    struct Audio final: public Decoder {
        ALCdevice* device = nullptr;
        ALCcontext* context = nullptr;
        ALuint source = 0;
        ALuint buffers[audioBufferCount]{};
        Vector<ALuint> available;

        struct Queued {
            ALuint buffer;
            double pts;
            int samples;
        };

        Vector<Queued> queued;
        AudioEvent event;
        Clock* clock = nullptr;
        bool clockDirty = false;
        LPALGETSOURCEDVSOFT getLatency = nullptr;
        SwrContext* resampler = nullptr;
        AVChannelLayout inputLayout{};
        AVSampleFormat inputFormat = AV_SAMPLE_FMT_NONE;
        int inputRate = 0;
        Buffer pcm;
        int pcmSamples = 0;
        int pcmAt = 0;
        double pcmPts = 0;
        double nextPts = 0;
        double played = 0;
        double latency = 0;
        bool resamplerEnded = false;
        void start(ObjPool& pool) override;
        void accept(Message* message) override;
        bool step() override;
        void seek(double position) override;
        void playback() override;
        void openOutput(ObjPool& pool);
        void service();
        void publishClock();
        void convert();
        void queueSamples();
        ObjPool* pool = nullptr;
    };

    struct Player;

    struct WakePlayer final: public plt::TimerCallback {
        Player* player;
        explicit WakePlayer(Player* player);
        void ready() override;
    };

    struct RetiredImage final: public Runable {
        Player* player;
        VideoImage* image;
        RetiredImage(Player* player, VideoImage* image);
        void run() override;
    };

    struct Player final: public Runable {
        ObjPool* pool = nullptr;
        Ui* ui = nullptr;
        Channel* inbox = nullptr;
        Channel* workers[3]{};
        plt::LoopWake* wake = nullptr;
        plt::Fiber* controller = nullptr;
        Vector<VideoImage*> ready;
        VideoImage* shown = nullptr;

        struct Sent {
            u64 generation = 1;
            bool playing = false;
            bool pending = false;
        };

        Sent sent[3];
        Buffer error;
        const char* path = nullptr;
        double duration = 0;
        double seekPosition = 0;
        double clockPosition = 0;
        double clockLimit = 0;
        u64 clockAt = 0;
        u64 generation = 1;
        bool playing = true;
        bool loaded = false;
        bool audio = false;
        bool video = false;
        bool haveClock = false;
        bool clockRunning = false;
        bool audioEnded = false;
        bool videoEnded = false;
        bool preview = true;
        bool quit = false;
        double videoEnd = 0;
        u64 nextUi = 0;
        void init(ObjPool& owner, const char* filename);
        void run() override;
        void accept(Message* message);
        void controls();
        void seek(double position, bool resume);
        void toggle();
        double position(u64 now) const;
        bool ended(double position) const;
        void resize(VideoImage& image, u32 width, u32 height);
        void release(VideoImage* image);
        void retired(VideoImage* image);
        void draw();
        void show(VideoImage* image);
        void trace(const char* event, double position);
    };
}

Kind Seek::messageKind() const {
    return kind;
}

Kind Playback::messageKind() const {
    return kind;
}

Kind Packet::messageKind() const {
    return kind;
}

Kind Stream::messageKind() const {
    return kind;
}

Kind File::messageKind() const {
    return kind;
}

Kind VideoImage::messageKind() const {
    return kind;
}

Kind Resize::messageKind() const {
    return kind;
}

Kind Clock::messageKind() const {
    return kind;
}

Kind AudioPulse::messageKind() const {
    return kind;
}

Kind End::messageKind() const {
    return kind;
}

Kind Failure::messageKind() const {
    return kind;
}

Packet::Packet()
    : packet(av_packet_alloc())
{
    STD_VERIFY(packet);
}

Packet::~Packet() noexcept {
    av_packet_free(&packet);
}

Stream::Stream()
    : parameters(avcodec_parameters_alloc())
{
    STD_VERIFY(parameters);
}

Stream::~Stream() noexcept {
    avcodec_parameters_free(&parameters);
}

void Mailbox::send(Message* message) const {
    ::send(channel, message);
    wake->signal();
}

void Worker::run() {
    ObjPool::Ref pool = ObjPool::fromMemory();
    try {
        start(*pool);
        for (;;) {
            void* value = nullptr;
            if (input->tryDequeue(&value)) {
                accept((Message*)value);
            } else if (!step()) {
                if (!input->dequeue(&value)) {
                    return;
                }
                accept((Message*)value);
            }
        }
    } catch (...) {
        Failure* failure = makeMessage<Failure>();
        failure->text = Buffer(Exception::current());
        output.send(failure);
    }
    // The UI owns process exit; no worker tears down another worker's state.
    for (;;) {
        void* value = nullptr;
        if (!input->dequeue(&value)) {
            return;
        }
        discard((Message*)value);
    }
}

bool Worker::control(Message* message) {
    if (Seek* command = cast<Seek>(message)) {
        generation = command->generation;
        playing = command->playing;
        seek(command->position);
        output.send(command);
        return true;
    }
    if (Playback* command = cast<Playback>(message)) {
        STD_VERIFY(command->generation == generation);
        playing = command->playing;
        playback();
        output.send(command);
        return true;
    }
    return false;
}

void Demux::start(ObjPool& pool) {
    ffcheck(avformat_open_input(&format, path, nullptr, nullptr));
    ffcheck(avformat_find_stream_info(format, nullptr));
    indexes[0] = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    indexes[1] = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (indexes[0] < 0 && indexes[1] < 0) {
        fail(StringView(u8"file has no video or audio stream"));
    }
    origin = format->start_time == AV_NOPTS_VALUE ? 0 : (double)format->start_time / AV_TIME_BASE;
    File* file = makeMessage<File>();
    file->duration = format->duration == AV_NOPTS_VALUE ? 0 : (double)format->duration / AV_TIME_BASE;
    file->audio = indexes[1] >= 0;
    file->video = indexes[0] >= 0;
    for (int i = 0; i < 2; i++) {
        if (indexes[i] < 0) {
            ended[i] = true;
            continue;
        }
        AVStream* source = format->streams[indexes[i]];
        Stream* stream = makeMessage<Stream>();
        ffcheck(avcodec_parameters_copy(stream->parameters, source->codecpar));
        stream->timeBase = source->time_base;
        stream->frameRate = av_guess_frame_rate(format, source, nullptr);
        stream->origin = origin;
        send(streams[i], stream);
        for (size_t j = 0; j < packetCount; j++) {
            Packet* packet = pool.make<Packet>();
            packet->stream = i;
            available[i].pushBack(packet);
        }
    }
    if (indexes[0] >= 0) {
        file->width = format->streams[indexes[0]]->codecpar->width;
        file->height = format->streams[indexes[0]]->codecpar->height;
    }
    pending = av_packet_alloc();
    STD_VERIFY(pending);
    output.send(file);
}

void Demux::accept(Message* message) {
    if (control(message)) {
        return;
    }
    Packet* packet = cast<Packet>(message);
    STD_VERIFY(packet);
    available[packet->stream].pushBack(packet);
}

bool Demux::step() {
    if (eof) {
        for (int i = 0; i < 2; i++) {
            if (!ended[i] && !available[i].empty()) {
                Packet* packet = (Packet*)available[i].popFront();
                packet->generation = generation;
                packet->eof = true;
                ended[i] = true;
                send(streams[i], packet);
                return true;
            }
        }
        return false;
    }
    if (pendingReady) {
        int stream = pending->stream_index == indexes[0] ? 0 : 1;
        if (available[stream].empty()) {
            return false;
        }
        Packet* packet = (Packet*)available[stream].popFront();
        packet->generation = generation;
        packet->eof = false;
        av_packet_move_ref(packet->packet, pending);
        pendingReady = false;
        send(streams[stream], packet);
        return true;
    }
    // One packet may be held while its receiving decoder has no credit.
    int status = av_read_frame(format, pending);
    if (status == AVERROR_EOF) {
        eof = true;
    } else {
        ffcheck(status);
        if (pending->stream_index != indexes[0] && pending->stream_index != indexes[1]) {
            av_packet_unref(pending);
        } else {
            pendingReady = true;
        }
    }
    return true;
}

void Demux::seek(double position) {
    av_packet_unref(pending);
    pendingReady = false;
    i64 timestamp = (i64)((position + origin) * AV_TIME_BASE);
    ffcheck(avformat_seek_file(format, -1, INT64_MIN, timestamp, timestamp, 0));
    eof = false;
    for (int i = 0; i < 2; i++) {
        ended[i] = indexes[i] < 0;
    }
}

void Demux::playback() {
}

void Decoder::start(ObjPool&) {
    frame = av_frame_alloc();
    STD_VERIFY(frame);
}

void Decoder::open(Stream& stream) {
    const AVCodec* implementation = avcodec_find_decoder(stream.parameters->codec_id);
    if (!implementation) {
        fail(StringView(u8"unsupported media codec"));
    }
    codec = avcodec_alloc_context3(implementation);
    STD_VERIFY(codec);
    ffcheck(avcodec_parameters_to_context(codec, stream.parameters));
    codec->pkt_timebase = stream.timeBase;
    ffcheck(avcodec_open2(codec, implementation, nullptr));
    timeBase = stream.timeBase;
    origin = stream.origin;
}

void Decoder::returnPacket(Packet* packet) {
    av_packet_unref(packet->packet);
    send(demux, packet);
}

void Decoder::reset(double position) {
    target = position;
    draining = false;
    exhausted = false;
    if (codec) {
        avcodec_flush_buffers(codec);
    }
    av_frame_unref(frame);
    while (!packets.empty()) {
        returnPacket((Packet*)packets.popFront());
    }
}

bool Decoder::feed() {
    if (packets.empty()) {
        return false;
    }
    Packet* packet = (Packet*)packets.front();
    int status = avcodec_send_packet(codec, packet->eof ? nullptr : packet->packet);
    if (status == AVERROR(EAGAIN)) {
        return true;
    }
    if (status != AVERROR_EOF) {
        ffcheck(status);
    }
    draining = packet->eof;
    packets.popFront();
    returnPacket(packet);
    return true;
}

void Video::accept(Message* message) {
    if (control(message)) {
        return;
    }
    if (Stream* stream = cast<Stream>(message)) {
        open(*stream);
        double rate = av_q2d(stream->frameRate);
        if (rate > 0 && rate <= 1000) {
            frameDuration = 1 / rate;
        }
        discard(stream);
    } else if (Packet* packet = cast<Packet>(message)) {
        if (packet->generation != generation) {
            returnPacket(packet);
        } else {
            packets.pushBack(packet);
        }
    } else {
        VideoImage* image = cast<VideoImage>(message);
        STD_VERIFY(image);
        images.pushBack(image);
    }
}

bool Video::step() {
    if (!codec || exhausted || (!playing && !preview) || images.empty()) {
        return false;
    }
    if (!frameReady) {
        int status = avcodec_receive_frame(codec, frame);
        if (status == AVERROR(EAGAIN)) {
            return feed();
        }
        if (status == AVERROR_EOF) {
            exhausted = true;
            End* end = makeMessage<End>();
            end->generation = generation;
            end->position = endPts;
            output.send(end);
            return true;
        }
        ffcheck(status);
        frameReady = true;
        return true;
    }
    double pts = frame->best_effort_timestamp == AV_NOPTS_VALUE ? nextPts : frame->best_effort_timestamp * av_q2d(timeBase) - origin;
    double length = frame->duration > 0 ? frame->duration * av_q2d(timeBase) : frameDuration;
    nextPts = pts + length;
    endPts = nextPts;
    if (pts + length <= target) {
        av_frame_unref(frame);
        frameReady = false;
        return true;
    }
    VideoImage* image = (VideoImage*)images.popFront();
    if (image->width != (u32)frame->width || image->height != (u32)frame->height) {
        Resize* resize = makeMessage<Resize>();
        resize->image = image;
        resize->width = frame->width;
        resize->height = frame->height;
        output.send(resize);
        return true;
    }
    scaler = sws_getCachedContext(scaler, frame->width, frame->height, (AVPixelFormat)frame->format, frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scaler) {
        fail(StringView(u8"cannot create video pixel converter"));
    }
    int colorspace = frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : frame->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_ITU601;
    const int* coefficients = sws_getCoefficients(colorspace);
    ffcheck(sws_setColorspaceDetails(scaler, coefficients, frame->color_range == AVCOL_RANGE_JPEG, coefficients, 1, 0, 1 << 16, 1 << 16));
    uint8_t* destination[] = {(uint8_t*)image->data, nullptr, nullptr, nullptr};
    int strides[] = {(int)image->stride, 0, 0, 0};
    ffcheck(sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, destination, strides));
    image->image->prepare();
    image->pts = pts;
    image->duration = length;
    image->generation = generation;
    image->aspect = av_q2d(frame->sample_aspect_ratio);
    if (!(image->aspect > 0)) {
        image->aspect = 1;
    }
    av_frame_unref(frame);
    frameReady = false;
    preview = false;
    output.send(image);
    return true;
}

void Video::seek(double position) {
    reset(position);
    frameReady = false;
    preview = true;
    nextPts = position;
    endPts = position;
}

void Video::playback() {
}

namespace {
    static void alcheck() {
        ALenum error = alGetError();
        if (error != AL_NO_ERROR) {
            fail(StringView(StringBuilder() << StringView(u8"OpenAL error ") << (i64)error));
        }
    }

    static void AL_APIENTRY audioEvent(ALenum, ALuint, ALuint, ALsizei, const ALchar*, void* context) noexcept {
        AudioEvent& event = *(AudioEvent*)context;
        void* token = nullptr;
        if (event.token->tryDequeue(&token)) {
            // One pulse in flight, with a slot reserved in the audio mailbox.
            if (!event.input->tryEnqueue(token)) {
                abort();
            }
        }
    }
}

void Audio::start(ObjPool& owner) {
    Decoder::start(owner);
    pool = &owner;
    clock = owner.make<Clock>();
    event.input = input;
    event.token = Channel::create(&owner, 1);
    event.token->enqueue(owner.make<AudioPulse>());
}

void Audio::openOutput(ObjPool&) {
    device = alcOpenDevice(nullptr);
    if (!device) {
        fail(StringView(u8"cannot open audio output"));
    }
    context = alcCreateContext(device, nullptr);
    if (!context || !alcMakeContextCurrent(context)) {
        fail(StringView(u8"cannot create OpenAL context"));
    }
    if (!alIsExtensionPresent("AL_SOFT_events") || !alIsExtensionPresent("AL_SOFT_source_latency")) {
        fail(StringView(u8"audio output requires OpenAL Soft events and source latency"));
    }
    auto events = (LPALEVENTCONTROLSOFT)alGetProcAddress("alEventControlSOFT");
    auto callback = (LPALEVENTCALLBACKSOFT)alGetProcAddress("alEventCallbackSOFT");
    getLatency = (LPALGETSOURCEDVSOFT)alGetProcAddress("alGetSourcedvSOFT");
    const ALenum types[] = {AL_EVENT_TYPE_BUFFER_COMPLETED_SOFT, AL_EVENT_TYPE_SOURCE_STATE_CHANGED_SOFT, AL_EVENT_TYPE_DISCONNECTED_SOFT};
    callback(audioEvent, &event);
    events(3, types, AL_TRUE);
    alGenSources(1, &source);
    alGenBuffers(audioBufferCount, buffers);
    alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(source, AL_ROLLOFF_FACTOR, 0);
    for (ALuint buffer : buffers) {
        available.pushBack(buffer);
    }
    alcheck();
}

void Audio::accept(Message* message) {
    if (control(message)) {
        return;
    }
    if (Stream* stream = cast<Stream>(message)) {
        open(*stream);
        openOutput(*pool);
        discard(stream);
    } else if (Packet* packet = cast<Packet>(message)) {
        if (packet->generation != generation) {
            returnPacket(packet);
        } else {
            packets.pushBack(packet);
        }
    } else if (AudioPulse* pulse = cast<AudioPulse>(message)) {
        // Return the token before querying OpenAL. A completion after this
        // point queues another pulse; earlier ones are covered by service().
        send(event.token, pulse);
        service();
    } else {
        Clock* returned = cast<Clock>(message);
        STD_VERIFY(returned && !clock);
        clock = returned;
    }
}

void Audio::service() {
    if (!source) {
        return;
    }
    ALCint connected = ALC_TRUE;
    if (alcIsExtensionPresent(device, "ALC_EXT_disconnect")) {
        alcGetIntegerv(device, ALC_CONNECTED, 1, &connected);
    }
    if (!connected) {
        fail(StringView(u8"audio output disconnected"));
    }
    ALint processed = 0;
    alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0) {
        ALuint buffer;
        alSourceUnqueueBuffers(source, 1, &buffer);
        STD_VERIFY(!queued.empty() && queued[0].buffer == buffer);
        played = queued[0].pts + (double)queued[0].samples / audioRate;
        for (size_t i = 1; i < queued.length(); i++) {
            queued.mut(i - 1) = queued[i];
        }
        queued.popBack();
        available.pushBack(buffer);
    }
    alcheck();
    clockDirty = true;
}

void Audio::publishClock() {
    if (!clock || !clockDirty || !source) {
        return;
    }
    ALdouble offset[2]{};
    getLatency(source, AL_SEC_OFFSET_LATENCY_SOFT, offset);
    ALint state;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    alcheck();
    latency = offset[1];
    clock->generation = generation;
    clock->at = monotonicNowUs();
    clock->running = playing && state == AL_PLAYING;
    clock->ended = exhausted && resamplerEnded && pcmAt == pcmSamples && queued.empty();
    if (queued.empty()) {
        clock->position = played;
        clock->limit = played;
    } else {
        clock->position = queued[0].pts + offset[0] - (state == AL_PLAYING ? latency : 0);
        clock->limit = queued.back().pts + (double)queued.back().samples / audioRate;
    }
    if (clock->position < target) {
        clock->position = target;
    }
    output.send(clock);
    clock = nullptr;
    clockDirty = false;
}

void Audio::convert() {
    if (!resampler || frame->sample_rate != inputRate || frame->format != inputFormat || av_channel_layout_compare(&frame->ch_layout, &inputLayout)) {
        swr_free(&resampler);
        av_channel_layout_uninit(&inputLayout);
        ffcheck(av_channel_layout_copy(&inputLayout, &frame->ch_layout));
        inputRate = frame->sample_rate;
        inputFormat = (AVSampleFormat)frame->format;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        ffcheck(swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, audioRate, &inputLayout, inputFormat, inputRate, 0, nullptr));
        ffcheck(swr_init(resampler));
    }
    i64 delay = swr_get_delay(resampler, inputRate);
    i64 capacity = av_rescale_rnd(delay + frame->nb_samples, audioRate, inputRate, AV_ROUND_UP);
    if (capacity < 0 || capacity > (1 << 20)) {
        fail(StringView(u8"invalid decoded audio size"));
    }
    pcm.grow((size_t)capacity * 4);
    uint8_t* destination = (uint8_t*)pcm.mutData();
    pcmSamples = swr_convert(resampler, &destination, (int)capacity, (const uint8_t**)frame->extended_data, frame->nb_samples);
    ffcheck(pcmSamples);
    pcmPts = frame->best_effort_timestamp == AV_NOPTS_VALUE ? nextPts : frame->best_effort_timestamp * av_q2d(timeBase) - origin - (double)delay / inputRate;
    nextPts = pcmPts + (double)pcmSamples / audioRate;
    pcmAt = pcmPts < target ? (int)fmin((double)pcmSamples, ceil((target - pcmPts) * audioRate)) : 0;
    av_frame_unref(frame);
}

void Audio::queueSamples() {
    int count = pcmSamples - pcmAt;
    if (count > audioBufferSamples) {
        count = audioBufferSamples;
    }
    ALuint buffer = available.popBack();
    alBufferData(buffer, AL_FORMAT_STEREO16, (u8*)pcm.data() + (size_t)pcmAt * 4, count * 4, audioRate);
    alSourceQueueBuffers(source, 1, &buffer);
    queued.pushBack({buffer, pcmPts + (double)pcmAt / audioRate, count});
    pcmAt += count;
    alcheck();
    ALint state;
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    if (playing && state != AL_PLAYING) {
        alSourcePlay(source);
        alcheck();
    }
    clockDirty = true;
}

bool Audio::step() {
    publishClock();
    if (!codec || available.empty()) {
        return false;
    }
    if (pcmAt < pcmSamples) {
        queueSamples();
        return true;
    }
    if (exhausted) {
        if (resamplerEnded) {
            return false;
        }
        pcm.grow(audioBufferSamples * 4);
        uint8_t* destination = (uint8_t*)pcm.mutData();
        pcmSamples = resampler ? swr_convert(resampler, &destination, audioBufferSamples, nullptr, 0) : 0;
        ffcheck(pcmSamples);
        pcmAt = 0;
        pcmPts = nextPts;
        nextPts += (double)pcmSamples / audioRate;
        resamplerEnded = pcmSamples == 0;
        clockDirty = true;
        return true;
    }
    int status = avcodec_receive_frame(codec, frame);
    if (status == AVERROR(EAGAIN)) {
        return feed();
    }
    if (status == AVERROR_EOF) {
        exhausted = true;
        return true;
    }
    ffcheck(status);
    convert();
    return true;
}

void Audio::seek(double position) {
    reset(position);
    pcmSamples = 0;
    pcmAt = 0;
    pcmPts = position;
    nextPts = position;
    played = position;
    resamplerEnded = false;
    if (source) {
        alSourceStop(source);
        alSourcei(source, AL_BUFFER, 0);
        alcheck();
        queued.clear();
        available.clear();
        for (ALuint buffer : buffers) {
            available.pushBack(buffer);
        }
        if (resampler) {
            swr_close(resampler);
            ffcheck(swr_init(resampler));
        }
        clockDirty = true;
    }
}

void Audio::playback() {
    if (source) {
        if (playing) {
            if (!queued.empty()) {
                alSourcePlay(source);
            }
        } else {
            alSourcePause(source);
        }
        alcheck();
        service();
    }
}

WakePlayer::WakePlayer(Player* value)
    : player(value)
{
}

void WakePlayer::ready() {
    player->controller->wake();
}

RetiredImage::RetiredImage(Player* owner, VideoImage* value)
    : player(owner)
    , image(value)
{
}

void RetiredImage::run() {
    player->retired(image);
}

void Player::trace(const char* event, double value) {
    ui->trace(StringView(StringBuilder() << StringView(event) << StringView(u8" generation=") << generation << StringView(u8" position_ms=") << (i64)(value * 1000)));
}

void Player::init(ObjPool& owner, const char* filename) {
    pool = &owner;
    path = filename;
    ui = Ui::create(owner, StringView(u8"play"), {800_d, 500_d});
    inbox = Channel::create(&owner, channelCapacity);
    wake = ui->platform()->createLoopWake(owner, *owner.make<WakePlayer>(this));
    for (Channel*& channel : workers) {
        channel = Channel::create(&owner, channelCapacity);
    }
    Demux* demux = owner.make<Demux>();
    Video* video = owner.make<Video>();
    Audio* audioWorker = owner.make<Audio>();
    Worker* threads[] = {demux, video, audioWorker};
    demux->path = path;
    demux->streams[0] = workers[1];
    demux->streams[1] = workers[2];
    video->demux = workers[0];
    audioWorker->demux = workers[0];
    // create() runs until the controller first parks, before any wake can arrive.
    controller = ui->platform()->scheduler()->create(owner, *this, 256 * 1024);
    for (int i = 0; i < 3; i++) {
        threads[i]->input = workers[i];
        threads[i]->output = {inbox, wake};
        Thread::create(&owner, *threads[i]);
    }
}

void Player::resize(VideoImage& image, u32 width, u32 height) {
    checkImageSize(width, height, ui->maxTextureSide());
    STD_VERIFY(!image.busy && !image.held);
    delete image.pixels;
    image.pixels = ObjPool::fromMemoryRaw();
    image.width = width;
    image.height = height;
    image.stride = ((size_t)width * 4 + 255) & ~(size_t)255;
    image.size = (image.stride * height + 65535) & ~(size_t)65535;
    image.data = image.pixels->allocateOverAligned(image.size, 65536);
    image.image = ui->bindImage(*image.pixels, width, height, image.data, image.size, image.stride, *image.retired);
}

void Player::release(VideoImage* image) {
    image->held = false;
    if (!image->busy) {
        send(workers[1], image);
    }
}

void Player::retired(VideoImage* image) {
    STD_VERIFY(image->busy);
    image->busy = false;
    if (!image->held) {
        send(workers[1], image);
    }
}

void Player::controls() {
    if (!loaded || !error.empty()) {
        return;
    }
    // Each worker has at most one control in flight. UI changes while it
    // runs collapse into the latest requested position and playback state.
    // Reset the decoders before allowing demux to issue the new generation.
    const int order[] = {1, 2, 0};
    for (int i : order) {
        Sent& state = sent[i];
        if (state.pending) {
            continue;
        }
        if (i == 0 && (sent[1].generation != generation || sent[2].generation != generation)) {
            continue;
        }
        if (state.generation != generation) {
            Seek* command = makeMessage<Seek>();
            command->worker = i;
            command->generation = generation;
            command->position = seekPosition;
            command->playing = playing;
            state.pending = true;
            state.generation = generation;
            state.playing = playing;
            send(workers[i], command);
        } else if (state.playing != playing) {
            Playback* command = makeMessage<Playback>();
            command->worker = i;
            command->generation = generation;
            command->playing = playing;
            state.pending = true;
            state.playing = playing;
            send(workers[i], command);
        }
    }
}

void Player::accept(Message* message) {
    if (VideoImage* image = cast<VideoImage>(message)) {
        if (image->generation != generation) {
            release(image);
        } else {
            image->held = true;
            ready.pushBack(image);
        }
        return;
    }
    if (Clock* clock = cast<Clock>(message)) {
        if (clock->generation == generation) {
            clockPosition = clock->position;
            clockAt = clock->at;
            clockLimit = clock->limit;
            clockRunning = clock->running;
            audioEnded = clock->ended;
            haveClock = true;
        }
        send(workers[2], clock);
        return;
    }
    if (File* file = cast<File>(message)) {
        duration = file->duration;
        audio = file->audio;
        video = file->video;
        loaded = true;
        if (video) {
            for (size_t i = 0; i < imageCount; i++) {
                VideoImage* image = pool->make<VideoImage>();
                image->retired = pool->make<RetiredImage>(this, image);
                if (file->width && file->height) {
                    resize(*image, file->width, file->height);
                }
                send(workers[1], image);
            }
        } else {
            videoEnded = true;
        }
        trace("opened", duration);
        ui->requestFrame();
    } else if (Resize* request = cast<Resize>(message)) {
        resize(*request->image, request->width, request->height);
        send(workers[1], request->image);
    } else if (Seek* command = cast<Seek>(message)) {
        sent[command->worker].pending = false;
    } else if (Playback* command = cast<Playback>(message)) {
        sent[command->worker].pending = false;
    } else if (End* end = cast<End>(message)) {
        if (end->generation == generation) {
            videoEnded = true;
            videoEnd = end->position;
        }
    } else if (Failure* failure = cast<Failure>(message)) {
        error = failure->text;
        playing = false;
        ui->trace(StringView(error));
        ui->requestFrame();
    } else {
        STD_VERIFY(false);
    }
    discard(message);
}

void Player::show(VideoImage* image) {
    VideoImage* previous = shown;
    shown = image;
    if (previous) {
        release(previous);
    }
    if (!audio && !haveClock) {
        clockPosition = fmax(seekPosition, image->pts);
        clockAt = monotonicNowUs();
        clockRunning = playing;
        haveClock = true;
    }
    preview = false;
    trace("show", image->pts);
    ui->requestFrame();
}

double Player::position(u64 now) const {
    if (!haveClock) {
        return seekPosition;
    }
    double value = clockPosition;
    if (playing && (clockRunning || audioEnded)) {
        value += (double)(now - clockAt) / 1000000;
    }
    if (audio && !audioEnded && value > clockLimit) {
        value = clockLimit;
    }
    if (duration > 0 && value > duration) {
        value = duration;
    }
    return fmax(0, value);
}

bool Player::ended(double at) const {
    double end = fmax(videoEnd, clockLimit);
    return loaded && videoEnded && (!audio || audioEnded) && ready.empty() && at >= (duration > 0 ? fmin(duration, end) : end);
}

void Player::run() {
    try {
        for (;;) {
            void* item = nullptr;
            while (inbox->tryDequeue(&item)) {
                accept((Message*)item);
            }
            controls();
            if (quit) {
                return;
            }
            u64 now = monotonicNowUs();
            double at = position(now);
            size_t count = 0;
            if (!ready.empty() && preview) {
                count = 1;
            }
            if (playing && (!audio || haveClock)) {
                while (count < ready.length() && ready[count]->pts <= at) {
                    count++;
                }
            }
            if (count) {
                for (size_t i = 0; i + 1 < count; i++) {
                    release(ready[i]);
                }
                show(ready[count - 1]);
                for (size_t i = count; i < ready.length(); i++) {
                    ready.mut(i - count) = ready[i];
                }
                for (size_t i = 0; i < count; i++) {
                    ready.popBack();
                }
            }
            if (ended(at) && playing) {
                clockPosition = at;
                clockAt = now;
                playing = false;
                clockRunning = false;
                trace("ended", at);
                ui->requestFrame();
                controls();
            }
            u64 wait = 0;
            if (playing && haveClock && (clockRunning || audioEnded) && error.empty()) {
                if (now >= nextUi) {
                    nextUi = now + 100000;
                    ui->requestFrame();
                }
                wait = nextUi > now ? nextUi - now : 1;
                if (!ready.empty()) {
                    double until = ready[0]->pts - position(now);
                    if (until > 0) {
                        u64 frameWait = (u64)ceil(until * 1000000);
                        if (frameWait < wait) {
                            wait = frameWait;
                        }
                    }
                }
            }
            plt::Fiber* self = ui->platform()->scheduler()->current();
            if (wait) {
                self->parkFor(wait);
            } else {
                self->park();
            }
        }
    } catch (...) {
        error = Buffer(Exception::current());
        playing = false;
        ui->trace(StringView(error));
        ui->requestFrame();
    }
}

void Player::seek(double value, bool resume) {
    if (!loaded || !error.empty()) {
        return;
    }
    seekPosition = fmax(0, duration > 0 ? fmin(value, fmax(0, duration - 0.001)) : value);
    generation++;
    playing = resume;
    haveClock = false;
    clockRunning = false;
    audioEnded = false;
    videoEnded = !video;
    videoEnd = 0;
    clockLimit = seekPosition;
    preview = true;
    for (VideoImage* image : ready) {
        release(image);
    }
    ready.clear();
    trace("seek", seekPosition);
    controller->wake();
    ui->requestFrame();
}

void Player::toggle() {
    u64 now = monotonicNowUs();
    double at = position(now);
    if (!playing && ended(at)) {
        seek(0, true);
        return;
    }
    playing = !playing;
    if (!audio) {
        clockPosition = at;
        clockAt = now;
        clockRunning = playing;
    }
    trace(playing ? "play" : "pause", at);
    controller->wake();
    ui->requestFrame();
}

void Player::draw() {
    if (!error.empty()) {
        if (ui->drawErrorPanel(StringView(error))) {
            quit = true;
        }
        return;
    }
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("##play", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    float controlsHeight = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetStyle().WindowPadding.y;
    ImVec2 available = ImGui::GetContentRegionAvail();
    ImVec2 size(available.x, fmaxf(1, available.y - controlsHeight));
    ImVec2 lo = ImGui::GetCursorScreenPos();
    if (shown) {
        double aspect = (double)shown->width * shown->aspect / shown->height;
        float width = fminf(size.x, (float)(size.y * aspect));
        float height = (float)(width / aspect);
        ImVec2 begin(lo.x + (size.x - width) / 2, lo.y + (size.y - height) / 2);
        shown->busy = true;
        shown->image->draw(*ImGui::GetWindowDrawList(), begin, ImVec2(begin.x + width, begin.y + height));
    }
    ImGui::Dummy(size);
    if (ImGui::Button(playing ? "Pause" : "Play") || ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        toggle();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop") || ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
        seek(0, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("-10 s") || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        seek(position(monotonicNowUs()) - 10, playing);
    }
    ImGui::SameLine();
    if (ImGui::Button("+10 s") || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        seek(position(monotonicNowUs()) + 10, playing);
    }
    ImGui::SameLine();
    double at = position(monotonicNowUs());
    ImGui::Text("%02d:%02d / %02d:%02d", (int)at / 60, (int)at % 60, (int)duration / 60, (int)duration % 60);
    float slider = (float)at;
    ImGui::SetNextItemWidth(-1);
    if (duration > 0 && ImGui::SliderFloat("##position", &slider, 0, (float)duration, "")) {
        seek(slider, playing);
    }
    ImGui::End();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        quit = true;
    }
}

int mainPlay(ObjPool& pool, int argc, char** argv) {
    if (argc != 2) {
        sysE << StringView(u8"usage: im play <file>") << endL;
        return 2;
    }
    Player* player = pool.make<Player>();
    player->init(pool, argv[1]);
    auto body = makeRunable([&] {
        UiEvent event;
        while (player->ui->next(event)) {
            if (event.kind == UiEvent::Kind::Close) {
                break;
            }
            player->draw();
            if (player->quit) {
                break;
            }
        }
        player->quit = true;
        player->controller->wake();
    });
    return player->ui->run(body);
}
