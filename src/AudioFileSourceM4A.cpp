#include "AudioFileSourceM4A.h"
#include <Arduino.h>
#include <string.h>

namespace {

// Box types are 4-character ASCII codes packed into a uint32 in big-endian
// order so they appear correctly in source.
constexpr uint32_t fourcc(const char (&s)[5]) {
    return ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) |
           ((uint32_t)s[2] <<  8) |  (uint32_t)s[3];
}

constexpr uint32_t BOX_FTYP = fourcc("ftyp");
constexpr uint32_t BOX_MOOV = fourcc("moov");
constexpr uint32_t BOX_TRAK = fourcc("trak");
constexpr uint32_t BOX_MDIA = fourcc("mdia");
constexpr uint32_t BOX_MINF = fourcc("minf");
constexpr uint32_t BOX_STBL = fourcc("stbl");
constexpr uint32_t BOX_STSD = fourcc("stsd");
constexpr uint32_t BOX_MP4A = fourcc("mp4a");
constexpr uint32_t BOX_ESDS = fourcc("esds");
constexpr uint32_t BOX_STSZ = fourcc("stsz");
constexpr uint32_t BOX_STCO = fourcc("stco");
constexpr uint32_t BOX_CO64 = fourcc("co64");
constexpr uint32_t BOX_STSC = fourcc("stsc");
constexpr uint32_t BOX_HDLR = fourcc("hdlr");

// Read a single MP4 box header. Returns the box type and the file offset of
// the byte just past the box (so the caller knows where the box ends).
// `pos_in` is the file offset where the header starts.
struct BoxHeader { uint32_t type; uint32_t end; bool ok; };

}  // namespace

AudioFileSourceM4A::AudioFileSourceM4A(AudioFileSourceSD* underlying)
    : _src(underlying) {
    if (!_src || !_src->isOpen()) return;

    if (!seekFileTo(0)) return;

    uint32_t total = _src->getSize();
    if (!parseTopLevel(total)) {
        Serial.printf("m4a: parse failed\n");
        return;
    }

    if (_sample_count == 0 || _chunk_count == 0 || _chunk_file_off == nullptr) {
        Serial.printf("m4a: incomplete sample tables\n");
        return;
    }

    if (!buildChunkIndex()) {
        Serial.printf("m4a: chunk index build failed\n");
        return;
    }

    if (!positionAtChunk(0)) return;
    _ok = true;
}

AudioFileSourceM4A::~AudioFileSourceM4A() {
    delete   _src;
    delete[] _chunk_file_off;
    delete[] _chunk_first_sample;
    delete[] _chunk_samples;
    delete[] _chunk_adts_off;
    delete[] _stsc_runs;
}

bool AudioFileSourceM4A::isOpen() {
    return _ok && _src && _src->isOpen();
}

bool AudioFileSourceM4A::close() {
    _ok = false;
    if (_src) return _src->close();
    return true;
}

uint32_t AudioFileSourceM4A::getSize() {
    return (uint32_t)_total_adts_size;
}

uint32_t AudioFileSourceM4A::getPos() {
    return (uint32_t)_stream_pos;
}

uint32_t AudioFileSourceM4A::read(void* data, uint32_t len) {
    if (!_ok || _eof) return 0;
    uint8_t* out = static_cast<uint8_t*>(data);
    uint32_t emitted = 0;

    while (emitted < len && !_eof) {
        if (_cur_in_sample < 7) {
            uint32_t take = 7 - _cur_in_sample;
            if (take > len - emitted) take = len - emitted;
            memcpy(out + emitted, _cur_adts + _cur_in_sample, take);
            _cur_in_sample += take;
            emitted        += take;
            _stream_pos    += take;
        } else {
            uint32_t in_payload   = _cur_in_sample - 7;
            uint32_t payload_left = _cur_sample_size - in_payload;
            uint32_t take = payload_left;
            if (take > len - emitted) take = len - emitted;

            uint32_t file_off = _chunk_file_off[_cur_chunk]
                              + _cur_byte_in_chunk
                              + in_payload;
            if (!seekFileTo(file_off)) { _eof = true; break; }
            uint32_t got = _src->read(out + emitted, take);
            if (got == 0) { _eof = true; break; }

            _cur_in_sample += got;
            emitted        += got;
            _stream_pos    += got;
        }

        if (_cur_in_sample >= 7 + _cur_sample_size) {
            // Sample exhausted — advance.
            _cur_byte_in_chunk += _cur_sample_size;
            _cur_in_chunk      += 1;
            _cur_in_sample      = 0;

            if (_cur_in_chunk >= _chunk_samples[_cur_chunk]) {
                _cur_chunk         += 1;
                _cur_in_chunk       = 0;
                _cur_byte_in_chunk  = 0;
                if (_cur_chunk >= _chunk_count) { _eof = true; break; }
            }

            uint32_t global_sample = _chunk_first_sample[_cur_chunk] + _cur_in_chunk;
            _cur_sample_size = sampleSize(global_sample);
            buildAdtsHeader(_cur_sample_size);
        }
    }

    return emitted;
}

bool AudioFileSourceM4A::seek(int32_t pos, int dir) {
    if (!_ok) return false;

    int64_t target;
    switch (dir) {
        case SEEK_SET: target = (int64_t)pos; break;
        case SEEK_CUR: target = (int64_t)_stream_pos + pos; break;
        case SEEK_END: target = (int64_t)_total_adts_size + pos; break;
        default: return false;
    }
    if (target < 0) target = 0;
    if ((uint64_t)target >= _total_adts_size) target = _total_adts_size - 1;

    uint32_t k = chunkForAdtsOffset((uint64_t)target);
    if (!positionAtChunk(k)) return false;
    _stream_pos = _chunk_adts_off[k];
    _eof = false;
    return true;
}

// ---------------------------------------------------------------- box parsing

bool AudioFileSourceM4A::parseTopLevel(uint32_t end) {
    bool ftyp_seen = false;
    bool moov_seen = false;
    while (fileTell() + 8 <= end) {
        uint32_t hsize, htype;
        uint32_t hpos = fileTell();
        if (!readU32(hsize) || !readU32(htype)) return false;

        uint32_t body_start = fileTell();
        uint64_t box_size   = hsize;
        if (hsize == 1) {
            uint64_t big;
            if (!readU64(big)) return false;
            box_size = big;
            body_start = fileTell();
        } else if (hsize == 0) {
            box_size = end - hpos;
        } else if (hsize < 8) {
            return false;
        }
        uint32_t box_end = (uint32_t)(hpos + box_size);
        if (box_end > end) return false;

        if (htype == BOX_FTYP) {
            ftyp_seen = true;
            // Major brand + version follow; we don't validate the brand list,
            // since real files use a wide variety (M4A, mp42, isom, …).
        } else if (htype == BOX_MOOV) {
            if (!parseMoov(box_end)) return false;
            moov_seen = true;
        }
        // mdat and any other top-level boxes are skipped — chunk offsets are
        // absolute, so we don't need to know where mdat starts.

        if (!seekFileTo(box_end)) return false;
    }
    return ftyp_seen && moov_seen;
}

bool AudioFileSourceM4A::parseMoov(uint32_t end) {
    int trak_count = 0;
    while (fileTell() + 8 <= end) {
        uint32_t hsize, htype;
        uint32_t hpos = fileTell();
        if (!readU32(hsize) || !readU32(htype)) return false;
        if (hsize < 8 || hpos + hsize > end) return false;
        uint32_t box_end = hpos + hsize;

        if (htype == BOX_TRAK) {
            if (trak_count > 0) {
                Serial.printf("m4a: multi-track file rejected\n");
                return false;
            }
            if (!parseTrak(box_end)) return false;
            trak_count++;
        }
        if (!seekFileTo(box_end)) return false;
    }
    return trak_count == 1;
}

bool AudioFileSourceM4A::parseTrak(uint32_t end) {
    while (fileTell() + 8 <= end) {
        uint32_t hsize, htype;
        uint32_t hpos = fileTell();
        if (!readU32(hsize) || !readU32(htype)) return false;
        if (hsize < 8 || hpos + hsize > end) return false;
        uint32_t box_end = hpos + hsize;

        if (htype == BOX_MDIA) {
            if (!parseMdia(box_end)) return false;
        }
        if (!seekFileTo(box_end)) return false;
    }
    return true;
}

bool AudioFileSourceM4A::parseMdia(uint32_t end) {
    while (fileTell() + 8 <= end) {
        uint32_t hsize, htype;
        uint32_t hpos = fileTell();
        if (!readU32(hsize) || !readU32(htype)) return false;
        if (hsize < 8 || hpos + hsize > end) return false;
        uint32_t box_end = hpos + hsize;

        if (htype == BOX_HDLR) {
            // 8 bytes reserved-ish, then 4-byte handler_type. "soun" required.
            if (!skipBytes(8)) return false;
            uint32_t hdlr_type;
            if (!readU32(hdlr_type)) return false;
            if (hdlr_type != fourcc("soun")) {
                Serial.printf("m4a: non-audio track rejected\n");
                return false;
            }
        } else if (htype == BOX_MINF) {
            if (!parseMinf(box_end)) return false;
        }
        if (!seekFileTo(box_end)) return false;
    }
    return true;
}

bool AudioFileSourceM4A::parseMinf(uint32_t end) {
    while (fileTell() + 8 <= end) {
        uint32_t hsize, htype;
        uint32_t hpos = fileTell();
        if (!readU32(hsize) || !readU32(htype)) return false;
        if (hsize < 8 || hpos + hsize > end) return false;
        uint32_t box_end = hpos + hsize;

        if (htype == BOX_STBL) {
            if (!parseStbl(box_end)) return false;
        }
        if (!seekFileTo(box_end)) return false;
    }
    return true;
}

bool AudioFileSourceM4A::parseStbl(uint32_t end) {
    while (fileTell() + 8 <= end) {
        uint32_t hsize, htype;
        uint32_t hpos = fileTell();
        if (!readU32(hsize) || !readU32(htype)) return false;
        if (hsize < 8 || hpos + hsize > end) return false;
        uint32_t box_end = hpos + hsize;

        bool ok = true;
        if      (htype == BOX_STSD) ok = parseStsd(box_end);
        else if (htype == BOX_STSZ) ok = parseStsz(box_end);
        else if (htype == BOX_STCO) ok = parseStco(box_end, false);
        else if (htype == BOX_CO64) ok = parseStco(box_end, true);
        else if (htype == BOX_STSC) ok = parseStsc(box_end);
        if (!ok) return false;

        if (!seekFileTo(box_end)) return false;
    }
    return true;
}

bool AudioFileSourceM4A::parseStsd(uint32_t end) {
    if (!skipBytes(4)) return false;          // version + flags
    uint32_t entries;
    if (!readU32(entries)) return false;
    if (entries < 1) return false;

    // Only the first entry is read; we reject multi-entry stsd as it implies
    // mid-track codec changes, which we don't support.
    if (entries > 1) {
        Serial.printf("m4a: multi-entry stsd rejected\n");
        return false;
    }

    uint32_t hsize, htype;
    uint32_t hpos = fileTell();
    if (!readU32(hsize) || !readU32(htype)) return false;
    if (hsize < 8 || hpos + hsize > end) return false;
    if (htype != BOX_MP4A) {
        Serial.printf("m4a: non-AAC sample entry rejected (type %08x)\n", htype);
        return false;
    }
    return parseMp4a(hpos + hsize);
}

bool AudioFileSourceM4A::parseMp4a(uint32_t end) {
    // 28 bytes of SampleEntry + AudioSampleEntry fixed header, then nested
    // boxes (esds expected).
    if (!skipBytes(28)) return false;

    while (fileTell() + 8 <= end) {
        uint32_t hsize, htype;
        uint32_t hpos = fileTell();
        if (!readU32(hsize) || !readU32(htype)) return false;
        if (hsize < 8 || hpos + hsize > end) return false;
        uint32_t box_end = hpos + hsize;

        if (htype == BOX_ESDS) {
            if (!parseEsds(box_end)) return false;
        }
        if (!seekFileTo(box_end)) return false;
    }
    return true;
}

// Read an MPEG-4 descriptor variable-length size (1..4 bytes, 7 bits each).
static bool readDescriptorSize(AudioFileSourceM4A* /*self*/,
                               uint8_t (*readU8)(void*), void* ctx,
                               uint32_t& size) {
    size = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b;
        b = readU8(ctx);
        // We can't actually fail-check this signature; reading helper handles
        // its own errors. Caller checks size sanity afterwards.
        size = (size << 7) | (b & 0x7F);
        if (!(b & 0x80)) return true;
    }
    return false;
}

bool AudioFileSourceM4A::parseEsds(uint32_t end) {
    if (!skipBytes(4)) return false;          // version + flags

    auto readVarSize = [&](uint32_t& size) -> bool {
        size = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t b;
            if (!readU8(b)) return false;
            size = (size << 7) | (b & 0x7F);
            if (!(b & 0x80)) return true;
        }
        return false;
    };

    uint8_t tag;
    uint32_t dsize;

    // ESDescriptor
    if (!readU8(tag) || tag != 0x03) return false;
    if (!readVarSize(dsize)) return false;
    if (!skipBytes(2)) return false;           // ES_ID
    uint8_t flags;
    if (!readU8(flags)) return false;
    if (flags & 0x80) { if (!skipBytes(2)) return false; }   // streamDependence
    if (flags & 0x40) {                                       // URL
        uint8_t url_len;
        if (!readU8(url_len)) return false;
        if (!skipBytes(url_len)) return false;
    }
    if (flags & 0x20) { if (!skipBytes(2)) return false; }   // OCRstream

    // DecoderConfigDescriptor
    if (!readU8(tag) || tag != 0x04) return false;
    if (!readVarSize(dsize)) return false;
    if (!skipBytes(13)) return false;          // object_type + stream_type + buffer + bitrates

    // DecoderSpecificInfo — payload is the AudioSpecificConfig
    if (!readU8(tag) || tag != 0x05) return false;
    if (!readVarSize(dsize)) return false;
    if (dsize < 2 || fileTell() + dsize > end) return false;

    uint8_t a, b;
    if (!readU8(a) || !readU8(b)) return false;
    uint8_t aot     = (a >> 3) & 0x1F;
    uint8_t sr_idx  = ((a & 0x07) << 1) | ((b >> 7) & 0x01);
    uint8_t chan    = (b >> 3) & 0x0F;

    if (aot < 1 || aot > 4) {
        Serial.printf("m4a: AOT %u not supported (only 1/2/3/4)\n", aot);
        return false;
    }
    if (sr_idx > 12) {
        // 13..14 reserved, 15 = explicit (24-bit follows). We don't decode it.
        Serial.printf("m4a: sample-rate index %u not supported\n", sr_idx);
        return false;
    }
    if (chan < 1 || chan > 7) {
        Serial.printf("m4a: channel-config %u not supported\n", chan);
        return false;
    }

    _profile  = aot - 1;
    _sr_idx   = sr_idx;
    _chan_cfg = chan;
    return true;
}

bool AudioFileSourceM4A::parseStsz(uint32_t end) {
    if (!skipBytes(4)) return false;          // version + flags
    uint32_t default_size, count;
    if (!readU32(default_size) || !readU32(count)) return false;
    _stsz_default   = default_size;
    _sample_count   = count;
    _stsz_table_off = fileTell();             // table follows when default == 0

    if (default_size == 0) {
        if (fileTell() + (uint64_t)count * 4 > end) return false;
    }
    return true;
}

bool AudioFileSourceM4A::parseStco(uint32_t end, bool is_co64) {
    if (!skipBytes(4)) return false;
    uint32_t entries;
    if (!readU32(entries)) return false;
    uint32_t per_entry = is_co64 ? 8 : 4;
    if (fileTell() + (uint64_t)entries * per_entry > end) return false;

    delete[] _chunk_file_off;
    _chunk_file_off = new uint32_t[entries];
    if (!_chunk_file_off) return false;
    _chunk_count = entries;

    for (uint32_t i = 0; i < entries; i++) {
        if (is_co64) {
            uint64_t v;
            if (!readU64(v)) return false;
            if (v > 0xFFFFFFFFu) {
                Serial.printf("m4a: 64-bit chunk offset exceeds 4GB\n");
                return false;
            }
            _chunk_file_off[i] = (uint32_t)v;
        } else {
            uint32_t v;
            if (!readU32(v)) return false;
            _chunk_file_off[i] = v;
        }
    }
    return true;
}

bool AudioFileSourceM4A::parseStsc(uint32_t end) {
    if (!skipBytes(4)) return false;
    uint32_t entries;
    if (!readU32(entries)) return false;
    if (entries == 0) return false;
    if (fileTell() + (uint64_t)entries * 12 > end) return false;

    delete[] _stsc_runs;
    _stsc_runs = new StscRun[entries];
    if (!_stsc_runs) return false;
    _stsc_run_count = entries;

    for (uint32_t i = 0; i < entries; i++) {
        uint32_t first_chunk, samples_per_chunk, sdi;
        if (!readU32(first_chunk) ||
            !readU32(samples_per_chunk) ||
            !readU32(sdi)) return false;
        _stsc_runs[i].first_chunk       = first_chunk;
        _stsc_runs[i].samples_per_chunk = samples_per_chunk;
    }
    return true;
}

// --------------------------------------------------------- chunk index build

bool AudioFileSourceM4A::buildChunkIndex() {
    if (!_chunk_count || !_stsc_run_count) return false;

    _chunk_first_sample = new uint32_t[_chunk_count];
    _chunk_samples      = new uint32_t[_chunk_count];
    _chunk_adts_off     = new uint64_t[_chunk_count];
    if (!_chunk_first_sample || !_chunk_samples || !_chunk_adts_off) return false;

    // Expand stsc runs into per-chunk samples_per_chunk.
    uint32_t run = 0;
    uint32_t cumulative_samples = 0;
    for (uint32_t c = 1; c <= _chunk_count; c++) {     // chunk indices are 1-based in stsc
        // Advance run if the next run's first_chunk has been reached.
        while (run + 1 < _stsc_run_count &&
               c >= _stsc_runs[run + 1].first_chunk) {
            run++;
        }
        uint32_t spc = _stsc_runs[run].samples_per_chunk;
        _chunk_samples[c - 1]      = spc;
        _chunk_first_sample[c - 1] = cumulative_samples;
        cumulative_samples += spc;
    }
    if (cumulative_samples != _sample_count) {
        Serial.printf("m4a: stsc/stsz disagree (%u vs %u)\n",
                      cumulative_samples, _sample_count);
        return false;
    }

    // Compute per-chunk synthetic ADTS-byte offsets. We walk stsz once
    // from disk; the read is sequential so the SD cache copes well.
    if (!seekFileTo(_stsz_table_off)) return false;
    uint64_t adts = 0;
    uint32_t sample = 0;
    for (uint32_t c = 0; c < _chunk_count; c++) {
        _chunk_adts_off[c] = adts;
        for (uint32_t s = 0; s < _chunk_samples[c]; s++) {
            uint32_t sz;
            if (_stsz_default != 0) {
                sz = _stsz_default;
            } else {
                if (!readU32(sz)) return false;
            }
            adts += 7 + sz;
            sample++;
        }
    }
    _total_adts_size = adts;

    // Done with the transient stsc runs.
    delete[] _stsc_runs;
    _stsc_runs      = nullptr;
    _stsc_run_count = 0;
    return true;
}

// ----------------------------------------------------------------- read state

bool AudioFileSourceM4A::positionAtChunk(uint32_t k) {
    if (k >= _chunk_count) return false;
    _cur_chunk         = k;
    _cur_in_chunk      = 0;
    _cur_byte_in_chunk = 0;
    _cur_in_sample     = 0;
    _eof               = false;

    uint32_t global_sample = _chunk_first_sample[k];
    if (!refillWindow(global_sample)) return false;
    _cur_sample_size = sampleSize(global_sample);
    buildAdtsHeader(_cur_sample_size);
    return true;
}

uint32_t AudioFileSourceM4A::sampleSize(uint32_t sample_idx) {
    if (_stsz_default != 0) return _stsz_default;
    if (sample_idx >= _sample_count) return 0;
    if (!_window_valid ||
        sample_idx < _window_first ||
        sample_idx >= _window_first + WINDOW_SAMPLES) {
        if (!refillWindow(sample_idx)) return 0;
    }
    return _window[sample_idx - _window_first];
}

bool AudioFileSourceM4A::refillWindow(uint32_t starting_at) {
    if (_stsz_default != 0) {
        // No need; sampleSize() returns the constant directly.
        _window_valid = true;
        _window_first = starting_at;
        return true;
    }
    if (starting_at >= _sample_count) {
        _window_valid = false;
        return false;
    }
    uint32_t to_read = _sample_count - starting_at;
    if (to_read > WINDOW_SAMPLES) to_read = WINDOW_SAMPLES;

    if (!seekFileTo(_stsz_table_off + starting_at * 4)) {
        _window_valid = false;
        return false;
    }
    for (uint32_t i = 0; i < to_read; i++) {
        if (!readU32(_window[i])) {
            _window_valid = false;
            return false;
        }
    }
    // Pad the rest with zeros so out-of-range reads are obvious.
    for (uint32_t i = to_read; i < WINDOW_SAMPLES; i++) _window[i] = 0;
    _window_first = starting_at;
    _window_valid = true;
    return true;
}

void AudioFileSourceM4A::buildAdtsHeader(uint32_t payload) {
    uint32_t frame_len = 7 + payload;
    _cur_adts[0] = 0xFF;
    _cur_adts[1] = 0xF1;                                    // MPEG-4, layer 0, no CRC
    _cur_adts[2] = (uint8_t)((_profile << 6) |
                             (_sr_idx  << 2) |
                             ((_chan_cfg >> 2) & 0x01));
    _cur_adts[3] = (uint8_t)(((_chan_cfg & 0x03) << 6) |
                             ((frame_len >> 11) & 0x03));
    _cur_adts[4] = (uint8_t)((frame_len >> 3) & 0xFF);
    _cur_adts[5] = (uint8_t)(((frame_len & 0x07) << 5) | 0x1F);
    _cur_adts[6] = 0xFC;                                    // 11111100: buffer_fullness = 0x7FF, raw_blocks = 0
}

uint32_t AudioFileSourceM4A::chunkForAdtsOffset(uint64_t off) {
    if (_chunk_count == 0) return 0;
    uint32_t lo = 0, hi = _chunk_count - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        if (_chunk_adts_off[mid] <= off) lo = mid;
        else                              hi = mid - 1;
    }
    return lo;
}

// ------------------------------------------------------ low-level file helpers

bool AudioFileSourceM4A::readU8(uint8_t& v) {
    return _src->read(&v, 1) == 1;
}
bool AudioFileSourceM4A::readU16(uint16_t& v) {
    uint8_t b[2];
    if (_src->read(b, 2) != 2) return false;
    v = ((uint16_t)b[0] << 8) | b[1];
    return true;
}
bool AudioFileSourceM4A::readU32(uint32_t& v) {
    uint8_t b[4];
    if (_src->read(b, 4) != 4) return false;
    v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
        ((uint32_t)b[2] <<  8) |  (uint32_t)b[3];
    return true;
}
bool AudioFileSourceM4A::readU64(uint64_t& v) {
    uint8_t b[8];
    if (_src->read(b, 8) != 8) return false;
    v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return true;
}
bool AudioFileSourceM4A::readBytes(void* dst, uint32_t n) {
    return _src->read(dst, n) == n;
}
bool AudioFileSourceM4A::skipBytes(uint32_t n) {
    return _src->seek((int32_t)n, SEEK_CUR);
}
bool AudioFileSourceM4A::seekFileTo(uint32_t off) {
    return _src->seek((int32_t)off, SEEK_SET);
}
uint32_t AudioFileSourceM4A::fileTell() {
    return _src->getPos();
}
