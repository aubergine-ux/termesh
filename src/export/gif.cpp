#include "export/gif.hpp"

#include <algorithm>
#include <cstring>

namespace termesh {

// ------------------------------------------------------------- median cut ---
//
// Start with one box containing every colour used. Repeatedly take the box
// with the most pixels spread over the widest range, and split it at the
// median along its longest axis, so both halves hold about the same number
// of pixels. When there are as many boxes as palette entries, each box's
// average colour becomes one entry. Busy regions of colour space end up
// with many small boxes (fine distinctions); sparse regions with few.
//
// To keep it fast the colours are first reduced to 5 bits per channel, giving
// a 32x32x32 histogram.

namespace {

constexpr int kBins = 32 * 32 * 32;

struct Bin {
    std::uint16_t index;  // position in the 15-bit histogram
    std::uint32_t count;
};

struct Box {
    int begin, end;  // range in the bin array
    std::uint64_t pixels = 0;
    int longestAxis = 0, extent = 0;
};

int channel(std::uint16_t index, int axis) { return (index >> (10 - 5 * axis)) & 31; }

void measure(const std::vector<Bin>& bins, Box& box) {
    int lo[3] = {31, 31, 31}, hi[3] = {0, 0, 0};
    box.pixels = 0;
    for (int i = box.begin; i < box.end; ++i) {
        for (int a = 0; a < 3; ++a) {
            const int v = channel(bins[i].index, a);
            lo[a] = std::min(lo[a], v);
            hi[a] = std::max(hi[a], v);
        }
        box.pixels += bins[i].count;
    }
    box.longestAxis = 0;
    box.extent = 0;
    // Green is weighted slightly: the eye is most sensitive to it.
    const int weight[3] = {3, 4, 2};
    for (int a = 0; a < 3; ++a) {
        const int extent = (hi[a] - lo[a]) * weight[a];
        if (extent > box.extent) {
            box.extent = extent;
            box.longestAxis = a;
        }
    }
}

}  // namespace

GifPalette buildGifPalette(const std::vector<Image>& frames) {
    std::vector<std::uint32_t> histogram(kBins, 0);
    for (const Image& frame : frames)
        for (const Color8& c : frame.pixels)
            ++histogram[(static_cast<std::size_t>(c.r >> 3) << 10) | (static_cast<std::size_t>(c.g >> 3) << 5) |
                        (c.b >> 3)];

    std::vector<Bin> bins;
    for (int i = 0; i < kBins; ++i)
        if (histogram[i]) bins.push_back({static_cast<std::uint16_t>(i), histogram[i]});

    std::vector<Box> boxes;
    if (!bins.empty()) {
        Box all{0, static_cast<int>(bins.size())};
        measure(bins, all);
        boxes.push_back(all);
    }

    while (static_cast<int>(boxes.size()) < kGifColors) {
        // Pick the box most worth splitting.
        int best = -1;
        std::uint64_t bestScore = 0;
        for (int i = 0; i < static_cast<int>(boxes.size()); ++i) {
            const Box& b = boxes[i];
            if (b.end - b.begin < 2 || b.extent == 0) continue;
            const std::uint64_t score = b.pixels * static_cast<std::uint64_t>(b.extent);
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        if (best < 0) break;  // every box is a single colour

        Box box = boxes[best];
        const int axis = box.longestAxis;
        std::sort(bins.begin() + box.begin, bins.begin() + box.end,
                  [axis](const Bin& a, const Bin& b) { return channel(a.index, axis) < channel(b.index, axis); });

        // Find the bin where the running pixel count passes half.
        std::uint64_t running = 0;
        int split = box.begin + 1;
        for (int i = box.begin; i < box.end - 1; ++i) {
            running += bins[i].count;
            split = i + 1;
            if (running * 2 >= box.pixels) break;
        }

        Box left{box.begin, split}, right{split, box.end};
        measure(bins, left);
        measure(bins, right);
        boxes[best] = left;
        boxes.push_back(right);
    }

    GifPalette palette;
    palette.count = static_cast<int>(boxes.size());
    for (int i = 0; i < palette.count; ++i) {
        std::uint64_t r = 0, g = 0, b = 0, n = 0;
        for (int j = boxes[i].begin; j < boxes[i].end; ++j) {
            // Bin centres, scaled back up to 8 bits.
            r += static_cast<std::uint64_t>(channel(bins[j].index, 0) * 8 + 4) * bins[j].count;
            g += static_cast<std::uint64_t>(channel(bins[j].index, 1) * 8 + 4) * bins[j].count;
            b += static_cast<std::uint64_t>(channel(bins[j].index, 2) * 8 + 4) * bins[j].count;
            n += bins[j].count;
        }
        if (n == 0) n = 1;
        palette.colors[i] = {static_cast<std::uint8_t>(r / n), static_cast<std::uint8_t>(g / n),
                             static_cast<std::uint8_t>(b / n), 255};
    }

    // Precompute the nearest entry for every 15-bit colour.
    palette.lookup.resize(kBins);
    for (int i = 0; i < kBins; ++i) {
        const int r = channel(static_cast<std::uint16_t>(i), 0) * 8 + 4;
        const int g = channel(static_cast<std::uint16_t>(i), 1) * 8 + 4;
        const int b = channel(static_cast<std::uint16_t>(i), 2) * 8 + 4;
        int best = 0, bestDist = 1 << 30;
        for (int p = 0; p < palette.count; ++p) {
            const Color8 c = palette.colors[p];
            const int dr = r - c.r, dg = g - c.g, db = b - c.b;
            const int dist = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
            if (dist < bestDist) {
                bestDist = dist;
                best = p;
            }
        }
        palette.lookup[i] = static_cast<std::uint8_t>(best);
    }
    return palette;
}

// ------------------------------------------------------------- file format ---

namespace {

void put16(std::vector<std::uint8_t>& out, int v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

}  // namespace

bool GifWriter::open(const std::string& path, int width, int height, const GifPalette& palette, int delayCs) {
    close();
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return false;
    width_ = width;
    height_ = height;
    delayCs_ = std::max(delayCs, 2);  // browsers treat anything faster as "slow"
    palette_ = &palette;
    failed_ = false;
    previous_.clear();

    std::vector<std::uint8_t> header;
    header.insert(header.end(), {'G', 'I', 'F', '8', '9', 'a'});
    put16(header, width);
    put16(header, height);
    header.push_back(0xF7);  // global colour table, 8 bits per channel, 256 entries
    header.push_back(0);     // background colour index
    header.push_back(0);     // pixel aspect ratio: square
    for (int i = 0; i < 256; ++i) {
        const Color8 c = i < palette.count ? palette.colors[i] : Color8{0, 0, 0, 255};
        header.insert(header.end(), {c.r, c.g, c.b});
    }
    // The Netscape application extension: loop forever.
    header.insert(header.end(), {0x21, 0xFF, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 3, 1, 0, 0, 0});
    failed_ = std::fwrite(header.data(), 1, header.size(), file_) != header.size();
    return !failed_;
}

bool GifWriter::addFrame(const Image& frame) {
    if (!file_ || frame.width != width_ || frame.height != height_) return false;
    const std::size_t count = frame.pixels.size();

    current_.resize(count);
    for (std::size_t i = 0; i < count; ++i) current_[i] = palette_->nearest(frame.pixels[i]);

    // Pixels that have not changed since the previous frame are written as
    // "transparent", letting the old pixel show through. Long runs of the
    // same value compress to almost nothing, so a static background is free.
    encoded_ = current_;
    if (previous_.size() == count)
        for (std::size_t i = 0; i < count; ++i)
            if (current_[i] == previous_[i]) encoded_[i] = kGifTransparent;

    std::vector<std::uint8_t> block;
    // Graphic control extension: frame delay, transparency, and "leave the
    // frame in place" disposal so the next frame can draw over it.
    block.insert(block.end(), {0x21, 0xF9, 4, 0x05});
    put16(block, delayCs_);
    block.push_back(kGifTransparent);
    block.push_back(0);
    // Image descriptor: full-size frame at the origin, no local palette.
    block.push_back(0x2C);
    put16(block, 0);
    put16(block, 0);
    put16(block, width_);
    put16(block, height_);
    block.push_back(0);
    if (std::fwrite(block.data(), 1, block.size(), file_) != block.size()) failed_ = true;

    writeLzw(encoded_);
    previous_.swap(current_);
    return !failed_;
}

// GIF's LZW variant.
//
// The encoder keeps a dictionary of pixel strings it has seen. It reads
// pixels, extending the current string for as long as the result is in the
// dictionary; when it is not, it emits the code for the string so far, adds
// the extended string as a new entry, and starts over from the last pixel.
// The decoder rebuilds the same dictionary from the codes alone.
//
// Codes start one bit wider than the pixel values and grow as the dictionary
// fills. At 4096 entries a "clear" code resets everything. The bit stream is
// packed least-significant-bit first into sub-blocks of at most 255 bytes.
void GifWriter::writeLzw(const std::vector<std::uint8_t>& indices) {
    constexpr int kMinCodeSize = 8;
    constexpr int kClear = 1 << kMinCodeSize, kEnd = kClear + 1;
    constexpr int kHashSize = 1 << 13, kEmpty = -1;

    // Dictionary: (prefix code, next pixel) -> code, as an open-addressing
    // hash table. The key packs both into one integer.
    static thread_local std::vector<std::int32_t> keys(kHashSize), values(kHashSize);
    auto reset = [&] { std::fill(keys.begin(), keys.end(), kEmpty); };

    std::vector<std::uint8_t> out;
    out.reserve(indices.size() / 2 + 1024);
    out.push_back(kMinCodeSize);

    std::uint8_t sub[255];
    int subLength = 0;
    std::uint32_t bitBuffer = 0;
    int bitCount = 0;
    int codeSize = kMinCodeSize + 1, nextCode = kEnd + 1;

    auto emit = [&](int code) {
        bitBuffer |= static_cast<std::uint32_t>(code) << bitCount;
        bitCount += codeSize;
        while (bitCount >= 8) {
            sub[subLength++] = static_cast<std::uint8_t>(bitBuffer & 0xFF);
            bitBuffer >>= 8;
            bitCount -= 8;
            if (subLength == 255) {
                out.push_back(255);
                out.insert(out.end(), sub, sub + 255);
                subLength = 0;
            }
        }
    };

    reset();
    emit(kClear);

    int prefix = indices.empty() ? kEnd : indices[0];
    for (std::size_t i = 1; i < indices.size(); ++i) {
        const int pixel = indices[i];
        const std::int32_t key = (prefix << 8) | pixel;
        int slot = static_cast<int>((static_cast<std::uint32_t>(key) * 2654435761u) >> 19) & (kHashSize - 1);
        while (keys[slot] != kEmpty && keys[slot] != key) slot = (slot + 1) & (kHashSize - 1);

        if (keys[slot] == key) {
            prefix = values[slot];  // the longer string is known: keep going
            continue;
        }
        emit(prefix);
        if (nextCode < 4096) {
            keys[slot] = key;
            values[slot] = nextCode;
            // The decoder's dictionary lags one entry behind ours, so the
            // width grows when the code just assigned no longer fits.
            if (nextCode == (1 << codeSize)) ++codeSize;
            ++nextCode;
        } else {
            emit(kClear);
            reset();
            codeSize = kMinCodeSize + 1;
            nextCode = kEnd + 1;
        }
        prefix = pixel;
    }
    if (!indices.empty()) emit(prefix);
    emit(kEnd);
    if (bitCount > 0) sub[subLength++] = static_cast<std::uint8_t>(bitBuffer & 0xFF);
    if (subLength > 0) {
        out.push_back(static_cast<std::uint8_t>(subLength));
        out.insert(out.end(), sub, sub + subLength);
    }
    out.push_back(0);  // block terminator

    if (std::fwrite(out.data(), 1, out.size(), file_) != out.size()) failed_ = true;
}

bool GifWriter::close() {
    if (!file_) return !failed_;
    const std::uint8_t trailer = 0x3B;
    if (std::fwrite(&trailer, 1, 1, file_) != 1) failed_ = true;
    if (std::fclose(file_) != 0) failed_ = true;
    file_ = nullptr;
    return !failed_;
}

}  // namespace termesh
