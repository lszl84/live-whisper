#include "transcriber.h"
#include "whisper.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

static constexpr int SAMPLE_RATE         = 16000;
static constexpr int INITIAL_INTERVAL_MS = 300;                  // first partial fires quickly
static constexpr int STREAM_INTERVAL_MS  = 400;                  // subsequent partials
static constexpr int MIN_SAMPLES         = SAMPLE_RATE / 4;      // need >= 0.25s of audio
static constexpr int COMMIT_SAMPLES      = SAMPLE_RATE * 25;     // commit chunk every 25s

static int inference_thread_count() {
    unsigned n = std::thread::hardware_concurrency();
    return static_cast<int>(std::max(4u, std::min(n, 16u)));
}

struct Transcriber::Impl {
    whisper_context* ctx = nullptr;

    // Audio buffer — appended by process(), consumed by streaming_loop()
    std::vector<float> audio_buf;
    std::mutex         audio_mutex;

    // Total samples received (for recording time display)
    std::atomic<uint64_t> total_samples{0};

    // Background streaming thread
    std::thread             thread;
    std::mutex              stop_mutex;
    std::condition_variable stop_cv;
    std::atomic<bool>       running{false};
    std::atomic<bool>       abort_inference{false};

    // Accumulated committed text (only touched by streaming thread)
    std::string confirmed_text;

    TextCallback callback;

    void streaming_loop();
    std::string run_whisper(const std::vector<float>& audio);
};

// ---------------------------------------------------------------------------
// Run whisper inference, returning concatenated segment text.
// ---------------------------------------------------------------------------
std::string Transcriber::Impl::run_whisper(const std::vector<float>& audio) {
    if (!ctx || audio.empty()) return {};

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress   = false;
    params.print_special    = false;
    params.print_realtime   = false;
    params.print_timestamps = false;
    params.single_segment   = true;
    params.no_context       = true;
    params.language         = "en";
    params.n_threads        = inference_thread_count();

    params.abort_callback = [](void* data) -> bool {
        return static_cast<Impl*>(data)->abort_inference.load();
    };
    params.abort_callback_user_data = this;

    if (abort_inference.load()) return {};

    int ret = whisper_full(ctx, params, audio.data(), static_cast<int>(audio.size()));
    if (ret != 0) return {};

    std::string text;
    int n_seg = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_seg; ++i) {
        const char* s = whisper_full_get_segment_text(ctx, i);
        if (s) text += s;
    }

    // Strip hallucinated noise labels like [BLANK_AUDIO], (wind blowing), etc.
    std::string clean;
    clean.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '[' || text[i] == '(') {
            char close = (text[i] == '[') ? ']' : ')';
            size_t end = text.find(close, i + 1);
            if (end != std::string::npos) { i = end + 1; continue; }
        }
        clean += text[i++];
    }
    return clean;
}

// ---------------------------------------------------------------------------
// Background streaming loop (adapted from whisper-agent).
// Re-transcribes the full growing audio buffer each pass so that repetition
// loops self-correct with more context.
// ---------------------------------------------------------------------------
void Transcriber::Impl::streaming_loop() {
    bool first_iter = true;
    std::string last_partial;

    while (running.load()) {
        int interval = first_iter ? INITIAL_INTERVAL_MS : STREAM_INTERVAL_MS;
        first_iter = false;

        {
            std::unique_lock<std::mutex> lk(stop_mutex);
            stop_cv.wait_for(lk, std::chrono::milliseconds(interval),
                             [this] { return !running.load(); });
        }
        if (!running.load()) break;

        // Snapshot audio buffer. If it exceeds the commit threshold and we
        // have partial text, save that text as confirmed and clear the buffer.
        std::vector<float> audio;
        {
            std::lock_guard<std::mutex> lk(audio_mutex);

            if (audio_buf.size() > static_cast<size_t>(COMMIT_SAMPLES)
                && !last_partial.empty())
            {
                if (confirmed_text.empty())
                    confirmed_text = last_partial;
                else
                    confirmed_text += " " + last_partial;
                audio_buf.clear();
                last_partial.clear();
            }

            audio = audio_buf;
        }
        if (static_cast<int>(audio.size()) < MIN_SAMPLES) continue;

        abort_inference = false;
        if (!running.load()) break;
        std::string text = run_whisper(audio);
        if (abort_inference.load()) break;

        last_partial = text;

        // Build full display text: confirmed chunks + current partial
        std::string display;
        if (confirmed_text.empty())
            display = text;
        else if (text.empty())
            display = confirmed_text;
        else
            display = confirmed_text + " " + text;

        if (callback)
            callback(display);
    }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

Transcriber::Transcriber() : impl_(std::make_unique<Impl>()) {}
Transcriber::~Transcriber() { stop(); shutdown(); }

bool Transcriber::init(const std::string& model_path)
{
    whisper_context_params cparams = whisper_context_default_params();
    impl_->ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!impl_->ctx) {
        std::fprintf(stderr, "transcriber: failed to load model: %s\n", model_path.c_str());
        return false;
    }
    return true;
}

void Transcriber::shutdown()
{
    if (impl_->ctx) {
        whisper_free(impl_->ctx);
        impl_->ctx = nullptr;
    }
}

void Transcriber::start()
{
    if (impl_->running.load()) return;

    impl_->confirmed_text.clear();
    impl_->abort_inference = false;
    impl_->running = true;
    impl_->thread = std::thread([this] { impl_->streaming_loop(); });
}

void Transcriber::stop()
{
    if (!impl_->running.load()) return;

    // Suppress whisper's "failed to encode" message from the aborted inference
    whisper_log_set([](ggml_log_level, const char*, void*) {}, nullptr);

    impl_->running = false;
    impl_->abort_inference = true;
    impl_->stop_cv.notify_all();

    if (impl_->thread.joinable())
        impl_->thread.join();
}

void Transcriber::process(const float* samples, uint32_t n)
{
    if (!impl_->ctx || n == 0) return;

    {
        std::lock_guard<std::mutex> lk(impl_->audio_mutex);
        impl_->audio_buf.insert(impl_->audio_buf.end(), samples, samples + n);
    }
    impl_->total_samples += n;
}

std::string Transcriber::full_text() const
{
    return impl_->confirmed_text;
}

float Transcriber::recording_seconds() const
{
    return static_cast<float>(impl_->total_samples.load()) / SAMPLE_RATE;
}

void Transcriber::reset()
{
    {
        std::lock_guard<std::mutex> lk(impl_->audio_mutex);
        impl_->audio_buf.clear();
    }
    impl_->confirmed_text.clear();
    impl_->total_samples = 0;
}

void Transcriber::set_callback(TextCallback cb)
{
    impl_->callback = std::move(cb);
}
