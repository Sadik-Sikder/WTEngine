#include "MediaMux.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <share.h> // _SH_DENYNO: _wfsopen lets others read a file being written
#include <cstring>
#include <memory>
#include <utility>

namespace mux {
namespace {

using Bytes = std::vector<uint8_t>;
constexpr size_t npos = (size_t)-1;

// A file read at any offset.
class File {
public:
    explicit File(const std::wstring& path) : f_(_wfsopen(path.c_str(), L"rb", _SH_DENYNO)) {
        if (f_ && _fseeki64(f_, 0, SEEK_END) == 0) size = (uint64_t)_ftelli64(f_);
    }
    ~File() { if (f_) fclose(f_); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    bool read(uint64_t at, size_t n, uint8_t* to) {
        return f_ && at <= size && n <= size - at && _fseeki64(f_, (int64_t)at, SEEK_SET) == 0 && fread(to, 1, n, f_) == n;
    }
    bool read(uint64_t at, uint64_t n, Bytes& to) {
        if (n > size) return false;
        to.resize((size_t)n);
        return n == 0 || read(at, (size_t)n, to.data());
    }
    uint64_t size = 0;
private:
    FILE* f_;
};

// The file being written. It's deleted again unless finish() succeeds.
class Out {
public:
    explicit Out(const std::wstring& path) : path_(path), f_(_wfsopen(path.c_str(), L"wb", _SH_DENYNO)) {}
    ~Out() {
        if (f_) {
            fclose(f_);
            _wremove(path_.c_str());
        }
    }
    bool ok() const { return f_ && !failed_; }
    void write(const uint8_t* p, size_t n) {
        if (f_ && n && fwrite(p, 1, n, f_) != n) failed_ = true;
        pos += n;
    }
    void write(const Bytes& b) { write(b.data(), b.size()); }
    void copy(File& from, uint64_t at, uint64_t n) {
        Bytes chunk;
        while (n && ok()) {
            size_t k = (size_t)std::min<uint64_t>(n, 1 << 20);
            if (!from.read(at, k, chunk)) { failed_ = true; return; }
            write(chunk);
            at += k;
            n -= k;
        }
    }
    // Overwrites what's at `at` (already written), then carries on at the end.
    void patch(uint64_t at, const Bytes& b) {
        if (!f_ || _fseeki64(f_, (int64_t)at, SEEK_SET) != 0 || fwrite(b.data(), 1, b.size(), f_) != b.size() ||
            _fseeki64(f_, 0, SEEK_END) != 0)
            failed_ = true;
    }
    bool finish() {
        if (!f_) return false;
        bool good = !failed_ && fflush(f_) == 0;
        fclose(f_);
        f_ = nullptr;
        if (!good) _wremove(path_.c_str());
        return good;
    }
    uint64_t pos = 0;
private:
    std::wstring path_;
    FILE* f_;
    bool failed_ = false;
};

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
void put64(uint8_t* p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}
void add32(Bytes& b, uint32_t v) {
    uint8_t t[4];
    put32(t, v);
    b.insert(b.end(), t, t + 4);
}
void add64(Bytes& b, uint64_t v) {
    uint8_t t[8];
    put64(t, v);
    b.insert(b.end(), t, t + 8);
}
void append(Bytes& to, const Bytes& b) { to.insert(to.end(), b.begin(), b.end()); }

// One media segment of a recording - an MP4 moof and its mdat, or a WebM
// Cluster's contents - and its span, in its track's own time units.
struct Fragment {
    uint64_t offset = 0, size = 0;
    uint64_t head = 0;                    // MP4: the moof's length (its mdat follows)
    int64_t start = 0, last = 0, end = 0; // its first frame, its last, and where it ends
    bool key = true;                      // it starts with a keyframe
};

// What one recording holds: its one track's header, and its fragments in
// time order with the repeats left out.
struct Track {
    bool ok = false, webm = false, video = false;
    double timescale = 1000; // time units a second
    std::vector<Fragment> fragments;
    std::string codec; // its Matroska CodecID ("" if MP4 and not one we know)
    // MP4: the boxes the merged header is made from, and the trex defaults.
    Bytes ftyp, mvhd, trak, trex;
    uint32_t defaultDuration = 0, defaultSize = 0, defaultFlags = 0;
    // MP4, for writing it as Matroska: the codec's setup, its picture or
    // sound, and the edit list's start (presentation time = decode time +
    // composition offset - this).
    Bytes codecPrivate;
    uint32_t width = 0, height = 0, channels = 0, sampleRate = 0, preSkip = 0;
    int64_t presentationShift = 0;
    // WebM: the TrackEntry's contents.
    Bytes entry;
    uint64_t timecodeScale = 1000000; // nanoseconds a time unit
    int64_t frameDuration = 0;        // DefaultDuration, in time units

    double seconds() const {
        int64_t units = 0;
        for (const auto& f : fragments) units += f.end - f.start;
        return units / timescale;
    }
    double endSeconds() const { return fragments.empty() ? 0 : fragments.back().end / timescale; }
};

// Sorts a track's fragments into time order and drops the ones it already
// has: a fragment fetched again after a seek starts within (or at) one
// before it.
void tidy(Track& t) {
    std::stable_sort(t.fragments.begin(), t.fragments.end(),
                     [](const Fragment& a, const Fragment& b) { return a.start < b.start; });
    std::vector<Fragment> kept;
    for (const auto& f : t.fragments)
        if (kept.empty() || f.start > kept.back().last) kept.push_back(f);
    t.fragments = std::move(kept);
}

// --- MP4 ---------------------------------------------------------------

// The boxes in d[from, to): f(type, at, header, size) for each, as far as
// they're whole.
template <class F> void eachBox(const Bytes& d, size_t from, size_t to, F f) {
    while (from + 8 <= to) {
        uint64_t size = be32(&d[from]);
        size_t header = 8;
        if (size == 1) {
            if (from + 16 > to) return;
            size = be64(&d[from + 8]);
            header = 16;
        } else if (size == 0) {
            size = to - from;
        }
        if (size < header || size > to - from) return;
        f(std::string((const char*)&d[from + 4], 4), from, header, (size_t)size);
        from += (size_t)size;
    }
}

struct Box {
    size_t at = npos, header = 0, size = 0;
    explicit operator bool() const { return at != npos; }
    size_t data() const { return at + header; }
    size_t end() const { return at + size; }
};

// The first `type` box in d[from, to).
Box findBox(const Bytes& d, size_t from, size_t to, const char* type) {
    Box found;
    eachBox(d, from, to, [&](const std::string& t, size_t at, size_t header, size_t size) {
        if (!found && t == type) found = { at, header, size };
    });
    return found;
}
Box findBox(const Bytes& d, const Box& in, const char* type) { return in ? findBox(d, in.data(), in.end(), type) : Box{}; }

// The header of the box at `at` in a file: false if there isn't a whole one.
bool boxInFile(File& file, uint64_t at, std::string& type, uint64_t& size, size_t& header) {
    uint8_t h[16];
    if (at >= file.size || file.size - at < 8) return false;
    size_t n = (size_t)std::min<uint64_t>(16, file.size - at);
    if (!file.read(at, n, h)) return false;
    size = be32(h);
    header = 8;
    if (size == 1) {
        if (n < 16) return false;
        size = be64(h + 8);
        header = 16;
    } else if (size == 0) {
        size = file.size - at;
    }
    type.assign((const char*)h + 4, 4);
    return size >= header && size <= file.size - at; // else cut off: the end of what was recorded
}

// A FullBox's version byte.
uint8_t version(const Bytes& d, const Box& b) { return b.size > b.header ? d[b.data()] : 0; }

uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
void addLE(Bytes& b, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; i++) b.push_back((uint8_t)(v >> (8 * i)));
}

// The track's codec, from the first sample entry in its stsd, as Matroska
// names it, with the setup it needs (CodecPrivate) - what writing this
// track into a WebM/Matroska file takes. Left empty for a codec it doesn't
// know (or an encrypted one).
void readSampleEntry(const Bytes& d, const Box& stsd, Track& t) {
    if (!stsd || stsd.size < stsd.header + 16) return;
    Box entry;
    eachBox(d, stsd.data() + 8, stsd.end(), [&](const std::string&, size_t at, size_t h, size_t size) {
        if (!entry) entry = { at, h, size };
    });
    if (!entry) return;
    const std::string type((const char*)&d[entry.at + 4], 4);
    const size_t fixed = t.video ? 78 : 28; // the VisualSampleEntry / AudioSampleEntry fields
    if (entry.size < entry.header + fixed) return;
    const uint8_t* e = &d[entry.data()];
    if (t.video) {
        t.width = be16(e + 24);
        t.height = be16(e + 26);
    } else {
        t.channels = be16(e + 16);
        t.sampleRate = be32(e + 24) >> 16;
    }
    Box children{ entry.data() + fixed - 8, 8, entry.size - entry.header - fixed + 8 }; // a box around the child boxes
    auto content = [&](const char* name) {
        Box b = findBox(d, children, name);
        return b ? Bytes(d.begin() + b.data(), d.begin() + b.end()) : Bytes();
    };
    if (type == "avc1" || type == "avc3") {
        t.codec = "V_MPEG4/ISO/AVC";
        t.codecPrivate = content("avcC");
    } else if (type == "hvc1" || type == "hev1") {
        t.codec = "V_MPEGH/ISO/HEVC";
        t.codecPrivate = content("hvcC");
    } else if (type == "av01") {
        t.codec = "V_AV1";
        t.codecPrivate = content("av1C");
    } else if (type == "vp09") {
        t.codec = "V_VP9";
    } else if (type == "vp08") {
        t.codec = "V_VP8";
    } else if (type == "mp4a") {
        // esds: an ES_Descriptor holding a DecoderConfigDescriptor holding
        // the DecoderSpecificInfo - AAC's AudioSpecificConfig.
        Bytes esds = content("esds");
        size_t p = 4, end = esds.size();
        auto descriptor = [&](uint8_t want, size_t& length) {
            if (p >= end || esds[p++] != want) return false;
            length = 0;
            for (int i = 0; i < 4 && p < end; i++) {
                uint8_t b = esds[p++];
                length = length << 7 | (b & 0x7F);
                if (!(b & 0x80)) break;
            }
            return length <= end - p;
        };
        size_t length;
        if (!descriptor(3, length) || p + 3 > end) return;
        uint8_t flags = esds[p + 2];
        p += 3;
        if (flags & 0x80) p += 2;
        if ((flags & 0x40) && p < end) p += 1 + esds[p];
        if (flags & 0x20) p += 2;
        if (!descriptor(4, length) || (p += 13) > end || !descriptor(5, length)) return;
        t.codec = "A_AAC";
        t.codecPrivate.assign(esds.begin() + p, esds.begin() + p + length);
    } else if (type == "Opus") {
        // dOps, rewritten as the OpusHead Matroska wants (little-endian).
        Bytes dops = content("dOps");
        if (dops.size() < 11) return;
        t.codec = "A_OPUS";
        t.preSkip = be16(&dops[2]);
        Bytes& head = t.codecPrivate;
        head = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, dops[1] };
        addLE(head, t.preSkip, 2);
        addLE(head, be32(&dops[4]), 4);
        addLE(head, be16(&dops[8]), 2);
        head.insert(head.end(), dops.begin() + 10, dops.end()); // the channel mapping
    }
}

bool readMoov(const Bytes& moov, size_t header, Track& t) {
    Box all{ 0, header, moov.size() };
    Box mvhd = findBox(moov, all, "mvhd");
    int traks = 0;
    Box trak;
    eachBox(moov, all.data(), all.end(), [&](const std::string& type, size_t at, size_t h, size_t size) {
        if (type == "trak" && traks++ == 0) trak = { at, h, size };
    });
    if (!mvhd || traks != 1) return false; // a stream of several tracks is left as it is
    Box tkhd = findBox(moov, trak, "tkhd");
    Box mdia = findBox(moov, trak, "mdia");
    Box mdhd = findBox(moov, mdia, "mdhd");
    Box hdlr = findBox(moov, mdia, "hdlr");
    if (!tkhd || !mdhd || !hdlr || hdlr.size < hdlr.header + 12) return false;
    size_t ts = mdhd.data() + (version(moov, mdhd) == 1 ? 20 : 12);
    size_t id = tkhd.data() + (version(moov, tkhd) == 1 ? 20 : 12);
    if (ts + 4 > mdhd.end() || id + 4 > tkhd.end()) return false;
    uint32_t timescale = be32(&moov[ts]);
    uint32_t trackId = be32(&moov[id]);
    if (!timescale) return false;
    t.timescale = timescale;
    t.video = std::memcmp(&moov[hdlr.data() + 8], "vide", 4) == 0;
    t.mvhd.assign(moov.begin() + mvhd.at, moov.begin() + mvhd.end());
    t.trak.assign(moov.begin() + trak.at, moov.begin() + trak.end());
    // This track's fragment defaults (the trex box), or a plain one.
    t.trex.clear();
    eachBox(moov, all.data(), all.end(), [&](const std::string& type, size_t at, size_t h, size_t size) {
        if (type != "mvex") return;
        eachBox(moov, at + h, at + size, [&](const std::string& type, size_t at, size_t h, size_t size) {
            if (type == "trex" && size >= h + 24 && be32(&moov[at + h + 4]) == trackId) {
                t.trex.assign(moov.begin() + at, moov.begin() + at + size);
                t.defaultDuration = be32(&moov[at + h + 12]);
                t.defaultSize = be32(&moov[at + h + 16]);
                t.defaultFlags = be32(&moov[at + h + 20]);
            }
        });
    });
    if (t.trex.empty()) {
        add32(t.trex, 32);
        t.trex.insert(t.trex.end(), { 't', 'r', 'e', 'x', 0, 0, 0, 0 });
        add32(t.trex, trackId);
        add32(t.trex, 1);
        t.trex.resize(32, 0);
    }
    readSampleEntry(moov, findBox(moov, findBox(moov, findBox(moov, mdia, "minf"), "stbl"), "stsd"), t);
    // The edit list's first edit that shows media: where presentation starts.
    Box elst = findBox(moov, findBox(moov, trak, "edts"), "elst");
    if (elst && elst.size >= elst.header + 8) {
        const bool v1 = version(moov, elst) == 1;
        const size_t entry = v1 ? 20 : 12;
        uint32_t count = be32(&moov[elst.data() + 4]);
        for (size_t i = 0, p = elst.data() + 8; i < count && p + entry <= elst.end(); i++, p += entry) {
            int64_t mediaTime = v1 ? (int64_t)be64(&moov[p + 8]) : (int64_t)(int32_t)be32(&moov[p + 4]);
            if (mediaTime >= 0) { t.presentationShift = mediaTime; break; }
        }
    }
    return true;
}

// One frame (or block of sound) of an MP4 fragment.
struct Sample {
    uint64_t offset = 0;  // from the moof's start
    uint32_t size = 0;
    int64_t time = 0;     // decode time
    int64_t duration = 0;
    int64_t composition = 0; // presentation time - decode time
    bool key = true;
};

// A moof's samples, from its trun boxes and the defaults in its tfhd (and
// the trex). `start` is its tfdt, or `running` (where the one before it
// ended) if it has none.
bool readSamples(const Bytes& moof, size_t header, const Track& t, int64_t running, std::vector<Sample>& out, int64_t& start) {
    Box all{ 0, header, moof.size() };
    int trafs = 0;
    Box traf;
    eachBox(moof, all.data(), all.end(), [&](const std::string& type, size_t at, size_t h, size_t size) {
        if (type == "traf" && trafs++ == 0) traf = { at, h, size };
    });
    Box tfhd = findBox(moof, traf, "tfhd");
    if (trafs != 1 || !tfhd || tfhd.size < tfhd.header + 8) return false;
    const uint32_t flags = be32(&moof[tfhd.data()]) & 0xFFFFFF;
    size_t p = tfhd.data() + 8;
    if (flags & 0x1) p += 8; // base_data_offset: data offsets still count from the moof, as segments' do
    if (flags & 0x2) p += 4; // sample_description_index
    uint32_t duration = t.defaultDuration, sampleSize = t.defaultSize, sampleFlags = t.defaultFlags;
    if (flags & 0x8) { if (p + 4 > tfhd.end()) return false; duration = be32(&moof[p]); p += 4; }
    if (flags & 0x10) { if (p + 4 > tfhd.end()) return false; sampleSize = be32(&moof[p]); p += 4; }
    if (flags & 0x20) { if (p + 4 > tfhd.end()) return false; sampleFlags = be32(&moof[p]); }

    start = running;
    if (Box tfdt = findBox(moof, traf, "tfdt")) {
        bool v1 = version(moof, tfdt) == 1;
        if (tfdt.data() + (v1 ? 12 : 8) <= tfdt.end())
            start = v1 ? (int64_t)be64(&moof[tfdt.data() + 4]) : be32(&moof[tfdt.data() + 4]);
    }
    int64_t time = start;
    uint64_t data = moof.size() + 8; // with no data offset given: the start of the mdat after it
    bool good = true;
    eachBox(moof, traf.data(), traf.end(), [&](const std::string& type, size_t at, size_t h, size_t size) {
        if (type != "trun" || !good) return;
        size_t q = at + h, e = at + size;
        if (q + 8 > e) { good = false; return; }
        const bool signedOffsets = moof[q] >= 1;
        uint32_t tf = be32(&moof[q]) & 0xFFFFFF, count = be32(&moof[q + 4]);
        q += 8;
        if (tf & 0x1) {
            if (q + 4 > e) { good = false; return; }
            data = (uint64_t)(int64_t)(int32_t)be32(&moof[q]);
            q += 4;
        }
        uint32_t firstFlags = sampleFlags;
        bool hasFirst = (tf & 0x4) != 0;
        if (hasFirst) { if (q + 4 > e) { good = false; return; } firstFlags = be32(&moof[q]); q += 4; }
        size_t per = 4 * (((tf & 0x100) != 0) + ((tf & 0x200) != 0) + ((tf & 0x400) != 0) + ((tf & 0x800) != 0));
        if (per && (e - std::min(q, e)) / per < count) { good = false; return; }
        if (!per && count > 1000000) { good = false; return; }
        for (uint32_t i = 0; i < count; i++) {
            size_t s = q + i * per;
            Sample x;
            uint32_t sf = i == 0 ? firstFlags : sampleFlags;
            x.duration = duration;
            x.size = sampleSize;
            if (tf & 0x100) { x.duration = be32(&moof[s]); s += 4; }
            if (tf & 0x200) { x.size = be32(&moof[s]); s += 4; }
            if (tf & 0x400) { if (!(i == 0 && hasFirst)) sf = be32(&moof[s]); s += 4; }
            if (tf & 0x800) x.composition = signedOffsets ? (int64_t)(int32_t)be32(&moof[s]) : (int64_t)be32(&moof[s]);
            x.key = !(sf & 0x10000); // sample_is_non_sync_sample
            x.offset = data;
            x.time = time;
            data += x.size;
            time += x.duration;
            out.push_back(x);
        }
    });
    return good;
}

// A moof's span, from its samples.
bool readMoof(const Bytes& moof, size_t header, const Track& t, Fragment& f, int64_t& running) {
    std::vector<Sample> samples;
    int64_t start;
    if (!readSamples(moof, header, t, running, samples, start)) return false;
    int64_t end = start;
    for (const auto& s : samples) end += s.duration;
    f.start = start;
    f.end = end;
    f.last = end > start ? end - 1 : start;
    f.key = samples.empty() || samples[0].key;
    running = end;
    return true;
}

bool readMp4(File& file, Track& t) {
    int64_t running = 0;
    bool haveMoov = false;
    uint64_t at = 0, size;
    size_t header;
    std::string type;
    while (boxInFile(file, at, type, size, header)) {
        if (type == "ftyp") {
            if (!file.read(at, size, t.ftyp)) return false;
        } else if (type == "moov") {
            Bytes moov;
            if (!file.read(at, size, moov) || !readMoov(moov, header, t)) return false;
            haveMoov = true;
        } else if (type == "moof" && haveMoov) {
            // Its mdat follows. A moof without one (its segment was cut
            // off) is skipped; at the end of the file, it's the end.
            std::string next;
            uint64_t mdat;
            size_t mh;
            if (!boxInFile(file, at + size, next, mdat, mh)) break;
            if (next != "mdat") {
                at += size;
                continue;
            }
            Bytes moof;
            Fragment f;
            if (file.read(at, size, moof) && readMoof(moof, header, t, f, running)) {
                f.offset = at;
                f.head = size;
                f.size = size + mdat;
                t.fragments.push_back(f);
            }
            at += size + mdat;
            continue;
        }
        at += size;
    }
    return haveMoov;
}

Bytes box(const char* type, const Bytes& content) {
    Bytes b;
    add32(b, (uint32_t)(content.size() + 8));
    b.insert(b.end(), type, type + 4);
    append(b, content);
    return b;
}

// The tracks' fragments in the order they're written: by time, the tracks
// interleaved, so a player reading from the start has both as it goes.
std::vector<std::pair<size_t, size_t>> interleave(const std::vector<Track>& tracks) {
    std::vector<std::pair<size_t, size_t>> order;
    for (size_t i = 0; i < tracks.size(); i++)
        for (size_t j = 0; j < tracks[i].fragments.size(); j++) order.push_back({ i, j });
    auto at = [&](const std::pair<size_t, size_t>& o) {
        const Track& t = tracks[o.first];
        return t.fragments[o.second].start / t.timescale;
    };
    std::stable_sort(order.begin(), order.end(), [&](const auto& a, const auto& b) { return at(a) < at(b); });
    return order;
}

// A fragmented MP4: ftyp, a moov with every track (track ids 1, 2, ...),
// the fragments, and an mfra indexing the keyframe ones for seeking.
bool writeMp4(std::vector<Track>& tracks, std::vector<std::unique_ptr<File>>& files, Out& out) {
    Bytes ftyp = tracks[0].ftyp;
    if (ftyp.empty()) ftyp = box("ftyp", { 'i', 's', 'o', '6', 0, 0, 0, 0, 'i', 's', 'o', '6', 'm', 'p', '4', '1' });
    out.write(ftyp);

    Bytes mvhd = tracks[0].mvhd;
    Box mv{ 0, 8, mvhd.size() };
    size_t tsAt = mv.data() + (version(mvhd, mv) == 1 ? 20 : 12);
    if (mvhd.size() < 100 || tsAt + 4 > mvhd.size()) return false;
    uint32_t movieScale = be32(&mvhd[tsAt]);
    put32(&mvhd[mvhd.size() - 4], (uint32_t)tracks.size() + 1); // next_track_ID
    double end = 0;
    for (const auto& t : tracks) end = std::max(end, t.endSeconds());

    Bytes moov = mvhd, mvex;
    Bytes mehd = { 1, 0, 0, 0 }; // the whole length, for players that show it
    add64(mehd, (uint64_t)(end * movieScale));
    append(mvex, box("mehd", mehd));
    for (size_t i = 0; i < tracks.size(); i++) {
        const uint32_t id = (uint32_t)i + 1;
        Bytes trak = tracks[i].trak;
        Box tkhd = findBox(trak, Box{ 0, 8, trak.size() }, "tkhd");
        if (!tkhd) return false;
        put32(&trak[tkhd.data() + (version(trak, tkhd) == 1 ? 20 : 12)], id);
        append(moov, trak);
        Bytes trex = tracks[i].trex;
        put32(&trex[12], id);
        append(mvex, trex);
    }
    append(moov, box("mvex", mvex));
    out.write(box("moov", moov));

    std::vector<Bytes> index(tracks.size()); // each track's tfra entries
    std::vector<uint32_t> entries(tracks.size(), 0);
    uint32_t sequence = 0;
    for (const auto& [i, j] : interleave(tracks)) {
        const Fragment& f = tracks[i].fragments[j];
        Bytes moof;
        if (!files[i]->read(f.offset, f.head, moof)) return false;
        const uint64_t at = out.pos;
        size_t header = moof.size() >= 16 && be32(moof.data()) == 1 ? 16 : 8;
        eachBox(moof, header, moof.size(), [&](const std::string& type, size_t b, size_t h, size_t size) {
            if (type == "mfhd" && size >= h + 8) put32(&moof[b + h + 4], ++sequence);
            if (type != "traf") return;
            Box tfhd = findBox(moof, b + h, b + size, "tfhd");
            if (!tfhd || tfhd.size < tfhd.header + 8) return;
            put32(&moof[tfhd.data() + 4], (uint32_t)i + 1);
            if ((be32(&moof[tfhd.data()]) & 0x1) && tfhd.size >= tfhd.header + 16) { // an offset from the file's start
                size_t p = tfhd.data() + 8;
                put64(&moof[p], at + (be64(&moof[p]) - f.offset));
            }
        });
        out.write(moof);
        out.copy(*files[i], f.offset + f.head, f.size - f.head);
        if (f.key || !tracks[i].video) {
            add64(index[i], (uint64_t)f.start);
            add64(index[i], at);
            index[i].insert(index[i].end(), { 1, 1, 1 }); // traf, trun, sample numbers
            entries[i]++;
        }
        if (!out.ok()) return false;
    }

    Bytes mfra;
    for (size_t i = 0; i < tracks.size(); i++) {
        Bytes tfra = { 1, 0, 0, 0 };
        add32(tfra, (uint32_t)i + 1);
        add32(tfra, 0); // one-byte traf/trun/sample numbers
        add32(tfra, entries[i]);
        append(tfra, index[i]);
        append(mfra, box("tfra", tfra));
    }
    Bytes mfro = { 0, 0, 0, 0 };
    add32(mfro, (uint32_t)(mfra.size() + 8 + 16));
    append(mfra, box("mfro", mfro));
    out.write(box("mfra", mfra));
    return out.ok();
}

// --- WebM ----------------------------------------------------------------

constexpr uint64_t kUnknown = ~0ull;
constexpr uint32_t kEbml = 0x1A45DFA3, kSegment = 0x18538067, kSeekHead = 0x114D9B74, kInfo = 0x1549A966,
                   kTracks = 0x1654AE6B, kCluster = 0x1F43B675, kCues = 0x1C53BB6B;

// An EBML number at p (`avail` bytes there): an element id, its length
// marker kept, or a data size, the marker dropped (all ones: unknown).
// Its length, or 0 if it isn't one.
size_t vint(const uint8_t* p, size_t avail, uint64_t& v, bool id) {
    if (!avail || !p[0]) return 0;
    size_t len = 1;
    while (!(p[0] & (0x80 >> (len - 1)))) len++;
    if (len > (id ? 4u : 8u) || len > avail) return 0;
    v = id ? p[0] : p[0] & (0xFF >> len);
    bool ones = v == (uint64_t)(0xFF >> len);
    for (size_t i = 1; i < len; i++) {
        v = v << 8 | p[i];
        ones = ones && p[i] == 0xFF;
    }
    if (!id && ones) v = kUnknown;
    return len;
}

// An element's header at p: its id and data size; its length, or 0.
size_t elementHeader(const uint8_t* p, size_t avail, uint32_t& id, uint64_t& size) {
    uint64_t i;
    size_t a = vint(p, avail, i, true);
    size_t b = a ? vint(p + a, avail - a, size, false) : 0;
    id = (uint32_t)i;
    return b ? a + b : 0;
}

uint64_t ebmlUint(const Bytes& d, size_t at, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && i < 8; i++) v = v << 8 | d[at + i];
    return v;
}

// The elements in d[from, to): f(id, at (the header), data, size), as far as
// they're whole and their size is given.
template <class F> void eachElement(const Bytes& d, size_t from, size_t to, F f) {
    while (from < to) {
        uint32_t id;
        uint64_t size;
        size_t h = elementHeader(&d[from], to - from, id, size);
        if (!h || size == kUnknown || size > to - from - h) return;
        f(id, from, from + h, (size_t)size);
        from += h + (size_t)size;
    }
}

bool topLevel(uint32_t id) {
    switch (id) {
    case kCluster: case kCues: case kSeekHead: case kInfo: case kTracks: case kEbml: case kSegment:
    case 0x1254C367: case 0x1043A770: case 0x1941A469: // Tags, Chapters, Attachments
        return true;
    }
    return false;
}

// A Cluster's span, from its Timecode and its blocks' times.
bool readCluster(const Bytes& body, const Track& t, Fragment& f) {
    int64_t timecode = 0, first = 0, last = 0, lastDuration = 0;
    int blocks = 0;
    auto block = [&](size_t p, size_t n, bool key, int64_t duration) {
        uint64_t track;
        size_t l = vint(&body[p], n, track, false);
        if (!l || l + 3 > n) return;
        int64_t time = timecode + (int16_t)(body[p + l] << 8 | body[p + l + 1]);
        if (!blocks++) {
            first = last = time;
            f.key = key;
        }
        first = std::min(first, time);
        if (time >= last) {
            last = time;
            lastDuration = duration;
        }
    };
    eachElement(body, 0, body.size(), [&](uint32_t id, size_t, size_t p, size_t n) {
        if (id == 0xE7) timecode = (int64_t)ebmlUint(body, p, n);
        else if (id == 0xA3 && n > 0) {
            uint64_t track;
            size_t l = vint(&body[p], n, track, false);
            block(p, n, l && l + 2 < n && (body[p + l + 2] & 0x80), 0); // SimpleBlock: the keyframe flag
        } else if (id == 0xA0) { // BlockGroup: a keyframe if it refers to no other
            size_t at = 0, length = 0;
            bool referred = false;
            int64_t duration = 0;
            eachElement(body, p, p + n, [&](uint32_t id, size_t, size_t q, size_t m) {
                if (id == 0xA1) { at = q; length = m; }
                if (id == 0xFB) referred = true;
                if (id == 0x9B) duration = (int64_t)ebmlUint(body, q, m);
            });
            if (length) block(at, length, !referred, duration);
        }
    });
    if (!blocks) return false;
    int64_t frame = lastDuration ? lastDuration
                  : t.frameDuration ? t.frameDuration
                  : blocks > 1 ? std::max<int64_t>(1, (last - first) / (blocks - 1)) : 1;
    f.start = first;
    f.last = last;
    f.end = last + frame;
    return true;
}

bool readWebm(File& file, Track& t) {
    uint8_t h[12];
    auto headerAt = [&](uint64_t at, uint32_t& id, uint64_t& size) -> size_t {
        size_t n = at < file.size ? (size_t)std::min<uint64_t>(12, file.size - at) : 0;
        return n && file.read(at, n, h) ? elementHeader(h, n, id, size) : 0;
    };
    uint32_t id;
    uint64_t size;
    size_t hl = headerAt(0, id, size);
    if (!hl || id != kEbml || size == kUnknown || size > file.size - hl) return false;
    uint64_t at = hl + size;
    hl = headerAt(at, id, size);
    if (!hl || id != kSegment) return false;
    at += hl;
    const uint64_t end = size == kUnknown ? file.size : std::min(file.size, at + size);
    bool haveTracks = false;
    while (at < end) {
        hl = headerAt(at, id, size);
        if (!hl) break;
        const uint64_t data = at + hl;
        if (id == kCluster) {
            uint64_t stop = data;
            bool cut = false;
            if (size == kUnknown) { // it ends where the next top-level element starts
                while (stop < end) {
                    uint32_t child;
                    uint64_t length;
                    size_t ch = headerAt(stop, child, length);
                    if (ch && topLevel(child)) break;
                    if (!ch || length == kUnknown || length > file.size - stop - ch) { cut = true; break; }
                    stop += ch + length;
                }
            } else {
                if (size > file.size - data) break; // cut off: the end of what was recorded
                stop = data + size;
            }
            Bytes body;
            Fragment f;
            if (haveTracks && file.read(data, stop - data, body) && readCluster(body, t, f)) {
                f.offset = data;
                f.size = stop - data;
                t.fragments.push_back(f);
            }
            if (cut) break;
            at = stop;
            continue;
        }
        if (size == kUnknown || size > file.size - data) break;
        if (id == kInfo) {
            Bytes info;
            if (!file.read(data, size, info)) return false;
            eachElement(info, 0, info.size(), [&](uint32_t id, size_t, size_t p, size_t n) {
                if (id == 0x2AD7B1) t.timecodeScale = std::max<uint64_t>(1, ebmlUint(info, p, n));
            });
        } else if (id == kTracks) {
            Bytes tracks;
            if (!file.read(data, size, tracks)) return false;
            int entries = 0;
            uint64_t defaultDuration = 0;
            eachElement(tracks, 0, tracks.size(), [&](uint32_t id, size_t, size_t p, size_t n) {
                if (id != 0xAE || entries++) return;
                t.entry.assign(tracks.begin() + p, tracks.begin() + p + n);
                eachElement(tracks, p, p + n, [&](uint32_t id, size_t, size_t q, size_t m) {
                    if (id == 0x83) t.video = ebmlUint(tracks, q, m) == 1;
                    if (id == 0x23E383) defaultDuration = ebmlUint(tracks, q, m);
                    if (id == 0x86) t.codec.assign(tracks.begin() + q, tracks.begin() + q + m);
                });
            });
            if (entries != 1) return false; // a stream of several tracks is left as it is
            haveTracks = true;
            t.frameDuration = (int64_t)(defaultDuration / t.timecodeScale);
        }
        at = data + size;
    }
    t.timescale = 1e9 / (double)t.timecodeScale;
    return haveTracks;
}

// An element, its size written as eight bytes (so it can be patched).
Bytes element(uint32_t id, const Bytes& content) {
    Bytes b;
    for (int shift = id > 0xFFFFFF ? 24 : id > 0xFFFF ? 16 : id > 0xFF ? 8 : 0; shift >= 0; shift -= 8)
        b.push_back((uint8_t)(id >> shift));
    b.push_back(0x01);
    for (int shift = 48; shift >= 0; shift -= 8) b.push_back((uint8_t)(content.size() >> shift));
    append(b, content);
    return b;
}
Bytes uintElement(uint32_t id, uint64_t v) {
    Bytes b;
    add64(b, v);
    return element(id, b);
}
Bytes floatElement(uint32_t id, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    return uintElement(id, bits);
}
Bytes stringElement(uint32_t id, const char* s) { return element(id, Bytes(s, s + std::strlen(s))); }

// Gives the blocks in body[from, to) track number `number`, written in
// the same number of bytes as before.
void renumber(Bytes& body, size_t from, size_t to, uint64_t number) {
    eachElement(body, from, to, [&](uint32_t id, size_t, size_t p, size_t n) {
        if (id == 0xA3 || id == 0xA1) {
            uint64_t old;
            size_t l = vint(&body[p], n, old, false);
            if (!l) return;
            std::memset(&body[p], 0, l);
            body[p] = (uint8_t)(0x80 >> (l - 1));
            for (size_t i = l, v = (size_t)number; i-- > 0 && v; v >>= 8) body[p + i] |= (uint8_t)v;
        } else if (id == 0xA0) {
            renumber(body, p, p + n, number);
        }
    });
}

// Codecs WebM allows; Matroska takes any.
bool webmCodec(const std::string& codec) {
    return codec == "V_VP8" || codec == "V_VP9" || codec == "V_AV1" || codec == "A_OPUS" || codec == "A_VORBIS";
}

// A WebM or Matroska file: the EBML header, then a Segment with a
// SeekHead, Info (with the duration), Tracks (numbered 1, 2, ...), the
// Clusters, and Cues for the keyframe ones, for seeking. A WebM track's
// Clusters are copied; an MP4 track's samples are written as SimpleBlocks,
// in Clusters of their own.
bool writeMatroska(std::vector<Track>& tracks, std::vector<std::unique_ptr<File>>& files, Out& out, bool webm) {
    // Time units: the WebM tracks' (all the same), else milliseconds.
    uint64_t scale = 1000000;
    for (const auto& t : tracks)
        if (t.webm) scale = t.timecodeScale;
    const double unitsPerSecond = 1e9 / (double)scale;

    Bytes ebml = uintElement(0x4286, 1); // EBMLVersion
    append(ebml, uintElement(0x42F7, 1)); // EBMLReadVersion
    append(ebml, uintElement(0x42F2, 4)); // EBMLMaxIDLength
    append(ebml, uintElement(0x42F3, 8)); // EBMLMaxSizeLength
    append(ebml, stringElement(0x4282, webm ? "webm" : "matroska"));
    append(ebml, uintElement(0x4287, 4)); // DocTypeVersion
    append(ebml, uintElement(0x4285, 2)); // DocTypeReadVersion
    out.write(element(kEbml, ebml));
    const uint64_t segmentAt = out.pos;
    out.write(element(kSegment, {}));
    const uint64_t segmentData = out.pos;
    auto seekHead = [](uint64_t info, uint64_t trackList, uint64_t cues) {
        Bytes seeks;
        for (auto [id, pos] : { std::pair<uint32_t, uint64_t>{ kInfo, info }, { kTracks, trackList }, { kCues, cues } }) {
            Bytes seek = element(0x53AB, { (uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id });
            append(seek, uintElement(0x53AC, pos));
            append(seeks, element(0x4DBB, seek));
        }
        return element(kSeekHead, seeks);
    };
    out.write(seekHead(0, 0, 0));

    double end = 0;
    for (const auto& t : tracks) end = std::max(end, t.endSeconds());
    const uint64_t infoAt = out.pos - segmentData;
    Bytes info = uintElement(0x2AD7B1, scale);
    append(info, floatElement(0x4489, end * unitsPerSecond));
    append(info, stringElement(0x4D80, "WTEngine"));
    append(info, stringElement(0x5741, "WTEngine"));
    out.write(element(kInfo, info));

    const uint64_t tracksAt = out.pos - segmentData;
    Bytes list;
    for (size_t i = 0; i < tracks.size(); i++) {
        const Track& t = tracks[i];
        Bytes e = uintElement(0xD7, i + 1); // TrackNumber
        append(e, uintElement(0x73C5, i + 1)); // TrackUID
        if (t.webm) {
            eachElement(t.entry, 0, t.entry.size(), [&](uint32_t id, size_t at, size_t p, size_t n) {
                if (id != 0xD7 && id != 0x73C5) e.insert(e.end(), t.entry.begin() + at, t.entry.begin() + p + n);
            });
        } else {
            append(e, uintElement(0x83, t.video ? 1 : 2)); // TrackType
            append(e, stringElement(0x86, t.codec.c_str()));
            if (!t.codecPrivate.empty()) append(e, element(0x63A2, t.codecPrivate));
            if (t.video) {
                Bytes video = uintElement(0xB0, t.width);
                append(video, uintElement(0xBA, t.height));
                append(e, element(0xE0, video));
            } else {
                Bytes audio = floatElement(0xB5, t.sampleRate);
                append(audio, uintElement(0x9F, t.channels));
                append(e, element(0xE1, audio));
            }
            if (t.codec == "A_OPUS") {
                append(e, uintElement(0x56AA, (uint64_t)t.preSkip * 1000000000 / 48000)); // CodecDelay
                append(e, uintElement(0x56BB, 80000000));                                // SeekPreRoll
            }
        }
        append(list, element(0xAE, e));
    }
    out.write(element(kTracks, list));

    // Cues on the video's keyframe clusters (or the first track's, for audio).
    size_t cued = 0;
    for (size_t i = 0; i < tracks.size(); i++)
        if (tracks[i].video) { cued = i; break; }
    Bytes cues;
    auto cluster = [&](size_t track, const Bytes& body, int64_t time, bool key) {
        if (track == cued && (key || !tracks[track].video)) {
            Bytes position = uintElement(0xF7, track + 1);
            append(position, uintElement(0xF1, out.pos - segmentData));
            Bytes point = uintElement(0xB3, (uint64_t)std::max<int64_t>(0, time));
            append(point, element(0xB7, position));
            append(cues, element(0xBB, point));
        }
        out.write(element(kCluster, body));
    };
    for (const auto& [i, j] : interleave(tracks)) {
        const Track& t = tracks[i];
        const Fragment& f = t.fragments[j];
        Bytes data;
        if (!files[i]->read(f.offset, f.size, data)) return false;
        if (t.webm) {
            renumber(data, 0, data.size(), i + 1);
            cluster(i, data, f.start, f.key);
        } else {
            // An MP4 fragment: its samples as SimpleBlocks, at their
            // presentation times. A block's time is relative to its
            // Cluster's, in 16 bits; a new Cluster starts when that's out of reach.
            std::vector<Sample> samples;
            int64_t start;
            Bytes moof(data.begin(), data.begin() + (size_t)f.head);
            size_t header = moof.size() >= 16 && be32(moof.data()) == 1 ? 16 : 8;
            if (!readSamples(moof, header, t, f.start, samples, start)) continue;
            Bytes body;
            int64_t clusterTime = 0;
            bool clusterKey = false;
            for (const auto& s : samples) {
                if (s.offset + s.size > data.size()) break;
                const int64_t pts = s.time + s.composition - t.presentationShift;
                const int64_t time = std::max<int64_t>(0, (int64_t)std::llround(pts / t.timescale * unitsPerSecond));
                if (body.empty() || time - clusterTime > 32767 || time - clusterTime < -32768) {
                    if (!body.empty()) cluster(i, body, clusterTime, clusterKey);
                    clusterTime = time;
                    clusterKey = s.key;
                    body = uintElement(0xE7, (uint64_t)time); // the Cluster's Timecode
                }
                const int16_t relative = (int16_t)(time - clusterTime);
                Bytes block = { (uint8_t)(0x80 | (i + 1)), (uint8_t)((uint16_t)relative >> 8), (uint8_t)relative,
                                (uint8_t)(s.key ? 0x80 : 0) };
                block.insert(block.end(), data.begin() + (size_t)s.offset, data.begin() + (size_t)(s.offset + s.size));
                append(body, element(0xA3, block));
            }
            if (!body.empty()) cluster(i, body, clusterTime, clusterKey);
        }
        if (!out.ok()) return false;
    }
    const uint64_t cuesAt = out.pos - segmentData;
    out.write(element(kCues, cues));

    // Now the sizes and positions are known.
    Bytes segment = element(kSegment, {});
    const uint64_t length = out.pos - segmentData;
    for (int k = 0; k < 7; k++) segment[segment.size() - 1 - k] = (uint8_t)(length >> (8 * k));
    out.patch(segmentAt, segment);
    out.patch(segmentData, seekHead(infoAt, tracksAt, cuesAt));
    return out.ok();
}

Track read(File& file) {
    Track t;
    uint8_t magic[4] = {};
    t.webm = file.read(0, 4, magic) && be32(magic) == kEbml;
    if (t.webm ? readWebm(file, t) : readMp4(file, t)) {
        tidy(t);
        t.ok = !t.fragments.empty();
    }
    return t;
}

Info infoOf(const Track& t) {
    Info info;
    info.ok = t.ok;
    info.webm = t.webm;
    info.video = t.video;
    info.seconds = t.ok ? t.seconds() : 0;
    info.codec = t.codec;
    return info;
}

} // namespace

Info inspect(const std::wstring& path) {
    File file(path);
    return infoOf(read(file));
}

std::wstring mergedExtension(const std::vector<Info>& inputs) {
    if (inputs.empty()) return L"";
    bool mp4 = true, webm = true, video = false;
    for (const auto& i : inputs) {
        if (!i.ok) return L"";
        mp4 = mp4 && !i.webm;
        webm = webm && webmCodec(i.codec);
        video = video || i.video;
    }
    if (mp4) return video ? L"mp4" : L"m4a";
    for (const auto& i : inputs)
        if (i.codec.empty()) return L""; // an MP4 codec that can't be written as Matroska
    return webm ? L"webm" : L"mkv";
}

bool merge(const std::vector<std::wstring>& inputs, const std::wstring& out) {
    if (inputs.empty()) return false;
    std::vector<std::unique_ptr<File>> files;
    std::vector<Track> tracks;
    std::vector<Info> infos;
    uint64_t scale = 0;
    for (const auto& path : inputs) {
        files.push_back(std::make_unique<File>(path));
        tracks.push_back(read(*files.back()));
        const Track& t = tracks.back();
        if (!t.ok) return false;
        if (t.webm) { // block times can't be rescaled
            if (scale && t.timecodeScale != scale) return false;
            scale = t.timecodeScale;
        }
        infos.push_back(infoOf(t));
    }
    const std::wstring kind = mergedExtension(infos);
    if (kind.empty()) return false;
    Out file(out);
    if (!file.ok()) return false;
    bool written = kind == L"mp4" || kind == L"m4a" ? writeMp4(tracks, files, file) : writeMatroska(tracks, files, file, kind == L"webm");
    return written && file.finish();
}

} // namespace mux
