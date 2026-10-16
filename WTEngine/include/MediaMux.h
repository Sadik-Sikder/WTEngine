// MediaMux.h
#pragma once
#include <string>
#include <vector>

// Turns MediaSource recordings (mse::recordings) into ordinary media files.
// A recording is what a page appended to one SourceBuffer in one quality:
// an init segment, then media segments - fragmented MP4 or WebM - in the
// order the page fetched them, which after a seek isn't time order, and
// with repeats. merge() puts one or more recordings (a video track and an
// audio track, say) into one file: their headers combined, the media in
// time order with the repeats left out, and an index for seeking. Nothing
// is decoded or re-encoded; the bytes are only rearranged.
namespace mux {

struct Info {
    bool ok = false;    // a recording merge() can read (one track)
    bool webm = false;  // WebM, else MP4
    bool video = false; // its track is video, else audio
    double seconds = 0; // how much media it holds, repeats counted once
    std::string codec;  // its Matroska codec id ("V_AV1", "A_AAC", ...); "" if unknown
};
Info inspect(const std::wstring& path);

// The kind of file merge() makes of recordings like `inputs`, as its file
// extension: "mp4" ("m4a" for audio alone) if they're all MP4, else "webm"
// if WebM can hold every codec, else "mkv" (Matroska holds any); "" if
// they can't be merged.
std::wstring mergedExtension(const std::vector<Info>& inputs);

// Writes `inputs` - recordings of one track each - as one file at `out`
// (of the kind mergedExtension names), the tracks in that order. False,
// leaving no file, if they can't be.
bool merge(const std::vector<std::wstring>& inputs, const std::wstring& out);

} // namespace mux
