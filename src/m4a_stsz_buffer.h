#pragma once

#include <SD.h>
#include "AudioTools/AudioCodecs/ContainerM4A.h"

// Stream the M4A stsz (sample-size) table from the open file instead of
// buffering all 22k+ entries (~45 KB for an 8-min track) in RAM. The
// audio-tools demuxer's default storage is a SingleBuffer<uint16_t> that
// gets resize()'d to sample_count; we replace it with this adapter so the
// table stays on the SD card. Each read() seeks into the stsz region,
// pulls a small lookahead chunk, and restores the file position so the
// audio frame reader is undisturbed.
class M4AStszStreamingBuffer
    : public audio_tools::BaseBuffer<audio_tools::stsz_sample_size_t> {
public:
    M4AStszStreamingBuffer(File& file, audio_tools::ContainerM4A& container)
        : p_file(&file), p_container(&container) {}

    bool read(audio_tools::stsz_sample_size_t& out) override {
        if (!initialised) {
            sample_count   = p_container->getDemuxer().getSampleCount();
            stsz_file_off  = p_container->getDemuxer().getStszFileOffset();
            initialised    = true;
        }
        if (sample_index >= sample_count) return false;

        if (chunk_pos >= chunk_len) {
            uint32_t target = stsz_file_off + STSZ_HEADER_BYTES + sample_index * 4;
            uint32_t saved  = p_file->position();
            if (!p_file->seek(target)) return false;
            chunk_len = p_file->read(chunk, sizeof(chunk));
            p_file->seek(saved);
            chunk_pos = 0;
            if (chunk_len < 4) return false;
        }

        uint32_t v = ((uint32_t)chunk[chunk_pos]     << 24)
                   | ((uint32_t)chunk[chunk_pos + 1] << 16)
                   | ((uint32_t)chunk[chunk_pos + 2] << 8)
                   |  (uint32_t)chunk[chunk_pos + 3];
        chunk_pos    += 4;
        sample_index += 1;
        out = (audio_tools::stsz_sample_size_t)v;
        return true;
    }

    bool write(audio_tools::stsz_sample_size_t) override { return true; }
    bool peek(audio_tools::stsz_sample_size_t&) override { return false; }
    bool resize(int) override { return true; }
    void reset() override { sample_index = 0; chunk_pos = 0; chunk_len = 0; }
    int available() override {
        return initialised ? (int)(sample_count - sample_index) : 0;
    }
    int availableForWrite() override { return INT32_MAX; }
    audio_tools::stsz_sample_size_t* address() override { return nullptr; }
    size_t size() override { return sample_count; }

private:
    static constexpr uint32_t STSZ_HEADER_BYTES = 20;
    static constexpr size_t   CHUNK_BYTES = 256;

    File*                          p_file = nullptr;
    audio_tools::ContainerM4A*     p_container = nullptr;
    bool                           initialised = false;
    uint32_t                       sample_count = 0;
    uint32_t                       stsz_file_off = 0;
    uint32_t                       sample_index = 0;
    uint8_t                        chunk[CHUNK_BYTES] = {0};
    size_t                         chunk_pos = 0;
    size_t                         chunk_len = 0;
};
