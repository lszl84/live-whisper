#pragma once

#include <cstdint>
#include <memory>

struct AudioCapture {
    AudioCapture();
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    bool init();
    void shutdown();

    // Read available samples into buf. Returns number of frames actually read.
    uint32_t read(float* buf, uint32_t max_frames);

    // Number of frames available for reading.
    uint32_t available() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
