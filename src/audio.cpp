#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio.h"

#include <cstdio>
#include <cstring>

static constexpr uint32_t SAMPLE_RATE    = 16000;
static constexpr uint32_t RING_BUF_SECS  = 60;
static constexpr uint32_t RING_BUF_FRAMES = SAMPLE_RATE * RING_BUF_SECS;

struct AudioCapture::Impl {
    ma_device   device{};
    ma_pcm_rb   ring_buf{};
    bool        device_inited = false;
    bool        rb_inited     = false;
};

static void capture_callback(ma_device* device, void* /*output*/,
                              const void* input, ma_uint32 frame_count)
{
    auto* rb = static_cast<ma_pcm_rb*>(device->pUserData);

    void* buf_write;
    ma_uint32 frames_to_write = frame_count;
    if (ma_pcm_rb_acquire_write(rb, &frames_to_write, &buf_write) == MA_SUCCESS) {
        std::memcpy(buf_write, input, frames_to_write * sizeof(float));
        ma_pcm_rb_commit_write(rb, frames_to_write);
    }
}

AudioCapture::AudioCapture() : impl_(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() { shutdown(); }

bool AudioCapture::init()
{
    // Init ring buffer
    if (ma_pcm_rb_init(ma_format_f32, 1, RING_BUF_FRAMES, nullptr,
                       nullptr, &impl_->ring_buf) != MA_SUCCESS) {
        std::fprintf(stderr, "audio: failed to init ring buffer\n");
        return false;
    }
    impl_->rb_inited = true;

    // Configure capture device
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format   = ma_format_f32;
    config.capture.channels = 1;
    config.sampleRate       = SAMPLE_RATE;
    config.dataCallback     = capture_callback;
    config.pUserData        = &impl_->ring_buf;

    if (ma_device_init(nullptr, &config, &impl_->device) != MA_SUCCESS) {
        std::fprintf(stderr, "audio: failed to init capture device\n");
        return false;
    }
    impl_->device_inited = true;

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        std::fprintf(stderr, "audio: failed to start capture device\n");
        return false;
    }

    return true;
}

void AudioCapture::shutdown()
{
    if (impl_->device_inited) {
        ma_device_uninit(&impl_->device);
        impl_->device_inited = false;
    }
    if (impl_->rb_inited) {
        ma_pcm_rb_uninit(&impl_->ring_buf);
        impl_->rb_inited = false;
    }
}

uint32_t AudioCapture::read(float* buf, uint32_t max_frames)
{
    void* buf_read;
    ma_uint32 frames = max_frames;
    if (ma_pcm_rb_acquire_read(&impl_->ring_buf, &frames, &buf_read) != MA_SUCCESS)
        return 0;

    std::memcpy(buf, buf_read, frames * sizeof(float));
    ma_pcm_rb_commit_read(&impl_->ring_buf, frames);
    return frames;
}

uint32_t AudioCapture::available() const
{
    return ma_pcm_rb_available_read(&impl_->ring_buf);
}
