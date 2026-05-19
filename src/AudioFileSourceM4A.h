// Wraps an AudioFileSourceSD on an M4A (AAC-in-MP4) file and presents it
// as a stream of ADTS-framed AAC bytes — letting ESP8266Audio's existing
// AudioGeneratorAAC decode it without changes. See changes/archive/
// 2026-04-?-m4a-support for the design rationale.
#pragma once

#include <AudioFileSource.h>
#include <AudioFileSourceSD.h>

class AudioFileSourceM4A : public AudioFileSource {
public:
    // Takes ownership of the underlying source. The source must already be
    // open. After construction, isOpen() reflects whether the file parsed as
    // a single-track DRM-free AAC M4A.
    explicit AudioFileSourceM4A(AudioFileSourceSD* underlying);
    ~AudioFileSourceM4A() override;

    uint32_t read(void* data, uint32_t len) override;
    bool seek(int32_t pos, int dir) override;
    bool close() override;
    bool isOpen() override;
    uint32_t getSize() override;
    uint32_t getPos() override;

private:
    static constexpr int WINDOW_SAMPLES = 256;     // 1 KB sliding window over stsz
    static constexpr int MAX_BOX_DEPTH  = 16;      // sanity bound on container nesting

    AudioFileSourceSD* _src;
    bool _ok = false;

    // Decoder config decoded from esds → AudioSpecificConfig.
    uint8_t _profile;     // ADTS profile (= AOT - 1)
    uint8_t _sr_idx;      // sample-frequency index 0..15
    uint8_t _chan_cfg;    // channel-configuration 1..7

    // Sample-size table (stsz). When _stsz_default != 0 every sample has the
    // same size; otherwise the size lives in the on-disk table at _stsz_table.
    uint32_t _sample_count    = 0;
    uint32_t _stsz_default    = 0;
    uint32_t _stsz_table_off  = 0;     // file offset of the first table entry

    // Sliding window over stsz entries — 256 entries (1 KB) refilled on demand.
    uint32_t _window[WINDOW_SAMPLES];
    uint32_t _window_first    = 0;     // global sample index of _window[0]
    bool     _window_valid    = false;

    // Chunk tables. Allocated to _chunk_count entries each.
    uint32_t  _chunk_count           = 0;
    uint32_t* _chunk_file_off        = nullptr;   // stco/co64: file byte offset of chunk N
    uint32_t* _chunk_first_sample    = nullptr;   // first sample-index in chunk N
    uint32_t* _chunk_samples         = nullptr;   // number of samples in chunk N
    uint64_t* _chunk_adts_off        = nullptr;   // cumulative ADTS-stream bytes before chunk N

    uint64_t _total_adts_size = 0;     // sum of (7 + size) over all samples

    // Read state — describes the next byte the wrapper will emit.
    uint32_t _cur_chunk         = 0;
    uint32_t _cur_in_chunk      = 0;   // sample index within _cur_chunk
    uint32_t _cur_byte_in_chunk = 0;   // file-byte cursor within the chunk
    uint32_t _cur_sample_size   = 0;
    uint8_t  _cur_adts[7];
    uint32_t _cur_in_sample     = 0;   // 0..6 = ADTS header; 7..(6+size) = payload
    uint64_t _stream_pos        = 0;   // synthetic byte offset emitted so far
    bool     _eof               = false;

    // Box parsing helpers. Each parses one container box's children up to
    // file offset `end`, returning false on a fatal protocol violation.
    bool parseTopLevel(uint32_t end);
    bool parseMoov(uint32_t end);
    bool parseTrak(uint32_t end);
    bool parseMdia(uint32_t end);
    bool parseMinf(uint32_t end);
    bool parseStbl(uint32_t end);
    bool parseStsd(uint32_t end);
    bool parseMp4a(uint32_t end);
    bool parseEsds(uint32_t end);
    bool parseStsz(uint32_t end);
    bool parseStco(uint32_t end, bool is_co64);
    bool parseStsc(uint32_t end);

    // Low-level file helpers. All advance the underlying source's read cursor.
    bool readU8 (uint8_t&  v);
    bool readU16(uint16_t& v);
    bool readU32(uint32_t& v);
    bool readU64(uint64_t& v);
    bool readBytes(void* dst, uint32_t n);
    bool skipBytes(uint32_t n);
    bool seekFileTo(uint32_t off);
    uint32_t fileTell();

    // sample-to-chunk runs from stsc, kept transiently during parse.
    struct StscRun { uint32_t first_chunk; uint32_t samples_per_chunk; };
    StscRun* _stsc_runs    = nullptr;
    uint32_t _stsc_run_count = 0;

    // Once stsz, stco, and stsc have all been parsed, derive the per-chunk
    // first-sample, sample-count, and ADTS-byte-offset tables. Frees the
    // transient stsc runs on completion.
    bool buildChunkIndex();

    // Build the synthetic 7-byte ADTS header for a sample of `payload` bytes.
    void buildAdtsHeader(uint32_t payload);

    // Sample-size lookup with on-demand window refill.
    uint32_t sampleSize(uint32_t sample_idx);
    bool refillWindow(uint32_t starting_at);

    // Position the read state to the start of chunk `k`. Refills the window
    // and prepares the first ADTS header. Used by the constructor and seek().
    bool positionAtChunk(uint32_t k);

    // Locate which chunk owns the given synthetic byte offset (binary search
    // over _chunk_adts_off).
    uint32_t chunkForAdtsOffset(uint64_t off);
};
