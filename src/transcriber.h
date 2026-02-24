#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct Transcriber {
    using TextCallback = std::function<void(const std::string& text)>;

    Transcriber();
    ~Transcriber();

    Transcriber(const Transcriber&) = delete;
    Transcriber& operator=(const Transcriber&) = delete;

    bool init(const std::string& model_path);
    void shutdown();

    // Feed audio samples and run inference when enough has accumulated (~3s).
    void process(const float* samples, uint32_t n);

    // Get the full transcribed text so far.
    std::string full_text() const;

    // Reset all state (clear buffers and text).
    void reset();

    // Set callback for live text updates.
    void set_callback(TextCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
