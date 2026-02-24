#include "transcriber.h"
#include "whisper.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>

static constexpr int   SAMPLE_RATE      = 16000;
static constexpr int   PROCESS_INTERVAL = 3 * SAMPLE_RATE;  // ~3s
static constexpr int   MAX_WINDOW       = 30 * SAMPLE_RATE;  // 30s
static constexpr int   OVERLAP          = 5 * SAMPLE_RATE;   // 5s carry-forward

struct Transcriber::Impl {
    whisper_context* ctx = nullptr;

    // Sliding window buffer
    std::vector<float> window_buf;
    int                samples_since_process = 0;

    // Committed text from previous windows
    std::string committed_text;

    // Current window partial text
    std::string current_text;

    // Prompt tokens for carry-forward
    std::vector<whisper_token> prompt_tokens;

    TextCallback callback;
};

Transcriber::Transcriber() : impl_(std::make_unique<Impl>()) {}
Transcriber::~Transcriber() { shutdown(); }

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

void Transcriber::process(const float* samples, uint32_t n)
{
    if (!impl_->ctx || n == 0) return;

    // Append to sliding window
    impl_->window_buf.insert(impl_->window_buf.end(), samples, samples + n);
    impl_->samples_since_process += static_cast<int>(n);

    // Only run inference every ~3s worth of audio
    if (impl_->samples_since_process < PROCESS_INTERVAL) return;
    impl_->samples_since_process = 0;

    // Run whisper on current window
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress   = false;
    wparams.print_special    = false;
    wparams.print_realtime   = false;
    wparams.print_timestamps = false;
    wparams.single_segment   = false;
    wparams.no_context       = false;
    wparams.language         = "en";

    // Use prompt tokens from previous window for context carry-forward
    if (!impl_->prompt_tokens.empty()) {
        wparams.prompt_tokens   = impl_->prompt_tokens.data();
        wparams.prompt_n_tokens = static_cast<int>(impl_->prompt_tokens.size());
    }

    int ret = whisper_full(impl_->ctx, wparams,
                           impl_->window_buf.data(),
                           static_cast<int>(impl_->window_buf.size()));
    if (ret != 0) {
        std::fprintf(stderr, "transcriber: whisper_full failed\n");
        return;
    }

    // Collect all segments into current text
    std::string text;
    int n_segments = whisper_full_n_segments(impl_->ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char* seg = whisper_full_get_segment_text(impl_->ctx, i);
        if (seg) text += seg;
    }
    impl_->current_text = text;

    // Notify via callback
    if (impl_->callback)
        impl_->callback(full_text());

    // Window rollover: if window exceeds max, commit and start new window
    if (static_cast<int>(impl_->window_buf.size()) >= MAX_WINDOW) {
        // Commit current text
        impl_->committed_text += impl_->current_text;
        impl_->current_text.clear();

        // Extract prompt tokens from last segment for carry-forward
        impl_->prompt_tokens.clear();
        if (n_segments > 0) {
            int last_seg = n_segments - 1;
            int n_tokens = whisper_full_n_tokens(impl_->ctx, last_seg);
            for (int i = 0; i < n_tokens; ++i) {
                impl_->prompt_tokens.push_back(
                    whisper_full_get_token_id(impl_->ctx, last_seg, i));
            }
        }

        // Keep last OVERLAP samples for continuity
        int keep = std::min(OVERLAP, static_cast<int>(impl_->window_buf.size()));
        std::vector<float> tail(impl_->window_buf.end() - keep,
                                impl_->window_buf.end());
        impl_->window_buf = std::move(tail);
    }
}

std::string Transcriber::full_text() const
{
    return impl_->committed_text + impl_->current_text;
}

void Transcriber::reset()
{
    impl_->window_buf.clear();
    impl_->committed_text.clear();
    impl_->current_text.clear();
    impl_->prompt_tokens.clear();
    impl_->samples_since_process = 0;
}

void Transcriber::set_callback(TextCallback cb)
{
    impl_->callback = std::move(cb);
}
