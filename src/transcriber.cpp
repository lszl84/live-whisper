#include "transcriber.h"
#include "whisper.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

static constexpr int SAMPLE_RATE      = 16000;
static constexpr int PROCESS_INTERVAL = 2 * SAMPLE_RATE;  // transcribe every ~2s
static constexpr int OVERLAP_SAMPLES  = SAMPLE_RATE / 2;  // 0.5s audio overlap

struct Transcriber::Impl {
    whisper_context* ctx = nullptr;

    // Audio buffer for the current (not yet committed) chunk
    std::vector<float> chunk_buf;
    int                samples_since_process = 0;

    // Final committed text (append-only, never re-transcribed)
    std::string committed_text;

    // Partial text from the current chunk (may change until committed)
    std::string pending_text;

    // Prompt tokens from last committed chunk for context carry-forward
    std::vector<whisper_token> prompt_tokens;

    // Total samples received (for recording time display)
    uint64_t total_samples = 0;

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

// Run whisper and return concatenated segment text.
static std::string run_inference(whisper_context* ctx,
                                 const std::vector<float>& audio,
                                 const std::vector<whisper_token>& prompt_tokens)
{
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress   = false;
    wparams.print_special    = false;
    wparams.print_realtime   = false;
    wparams.print_timestamps = false;
    wparams.single_segment   = false;
    wparams.no_context       = false;
    wparams.language         = "en";

    if (!prompt_tokens.empty()) {
        wparams.prompt_tokens   = prompt_tokens.data();
        wparams.prompt_n_tokens = static_cast<int>(prompt_tokens.size());
    }

    int ret = whisper_full(ctx, wparams,
                           audio.data(), static_cast<int>(audio.size()));
    if (ret != 0) return {};

    std::string text;
    int n_seg = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_seg; ++i) {
        const char* s = whisper_full_get_segment_text(ctx, i);
        if (s) text += s;
    }
    return text;
}

// Extract prompt tokens from the last segment for carry-forward.
static std::vector<whisper_token> extract_prompt_tokens(whisper_context* ctx)
{
    std::vector<whisper_token> tokens;
    int n_seg = whisper_full_n_segments(ctx);
    if (n_seg > 0) {
        int last = n_seg - 1;
        int n_tok = whisper_full_n_tokens(ctx, last);
        for (int i = 0; i < n_tok; ++i)
            tokens.push_back(whisper_full_get_token_id(ctx, last, i));
    }
    return tokens;
}

void Transcriber::process(const float* samples, uint32_t n)
{
    if (!impl_->ctx || n == 0) return;

    impl_->chunk_buf.insert(impl_->chunk_buf.end(), samples, samples + n);
    impl_->samples_since_process += static_cast<int>(n);
    impl_->total_samples += n;

    if (impl_->samples_since_process < PROCESS_INTERVAL) return;
    impl_->samples_since_process = 0;

    // Run inference on the current chunk
    std::string text = run_inference(impl_->ctx, impl_->chunk_buf,
                                     impl_->prompt_tokens);

    // Commit: this chunk's text is final
    impl_->prompt_tokens = extract_prompt_tokens(impl_->ctx);
    impl_->committed_text += text;
    impl_->pending_text.clear();

    // Keep a small audio overlap for continuity, discard the rest
    int keep = std::min(OVERLAP_SAMPLES, static_cast<int>(impl_->chunk_buf.size()));
    std::vector<float> tail(impl_->chunk_buf.end() - keep,
                            impl_->chunk_buf.end());
    impl_->chunk_buf = std::move(tail);

    if (impl_->callback)
        impl_->callback(full_text());
}

std::string Transcriber::full_text() const
{
    return impl_->committed_text + impl_->pending_text;
}

float Transcriber::recording_seconds() const
{
    return static_cast<float>(impl_->total_samples) / SAMPLE_RATE;
}

void Transcriber::reset()
{
    impl_->chunk_buf.clear();
    impl_->committed_text.clear();
    impl_->pending_text.clear();
    impl_->prompt_tokens.clear();
    impl_->samples_since_process = 0;
    impl_->total_samples = 0;
}

void Transcriber::set_callback(TextCallback cb)
{
    impl_->callback = std::move(cb);
}
