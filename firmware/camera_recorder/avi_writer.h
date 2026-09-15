#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace recorder {

constexpr uint32_t kHeaderBytes = 224;
constexpr uint32_t kMoviOrigin = 220;
constexpr uint32_t kDefaultFileLimit = 512U * 1024U * 1024U;

inline void put16(uint8_t *p, uint16_t value) {
  p[0] = value;
  p[1] = value >> 8;
}

inline void put32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = value >> (i * 8);
}

inline uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
         uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

struct IndexEntry {
  uint32_t offset;
  uint32_t bytes;
};

struct VideoStats {
  uint32_t frames = 0;
  uint32_t dataBytes = 0;
  uint32_t maxFrameBytes = 0;
  uint32_t durationMs = 0;
};

inline void makeHeader(uint8_t *header, uint16_t width, uint16_t height,
                       unsigned targetFps, const VideoStats &stats, bool indexed) {
  std::memset(header, 0, kHeaderBytes);
  const uint32_t indexBytes = indexed ? 8 + stats.frames * 16 : 0;
  const uint32_t duration = stats.durationMs ? stats.durationMs : 1;
  std::memcpy(header, "RIFF", 4);
  put32(header + 4, kHeaderBytes + stats.dataBytes + indexBytes - 8);
  std::memcpy(header + 8, "AVI LIST", 8);
  put32(header + 16, 192);
  std::memcpy(header + 20, "hdrlavih", 8);
  put32(header + 28, 56);
  put32(header + 32, stats.frames
                        ? uint32_t((uint64_t(duration) * 1000 + stats.frames / 2) /
                                   stats.frames)
                        : 1000000 / targetFps);
  put32(header + 36, stats.frames
                        ? uint32_t(uint64_t(stats.dataBytes) * 1000 / duration)
                        : 0);
  put32(header + 44, indexed ? 0x10 : 0);  // AVIF_HASINDEX
  put32(header + 48, stats.frames);
  put32(header + 56, 1);
  put32(header + 60, stats.maxFrameBytes);
  put32(header + 64, width);
  put32(header + 68, height);
  std::memcpy(header + 88, "LIST", 4);
  put32(header + 92, 116);
  std::memcpy(header + 96, "strlstrh", 8);
  put32(header + 104, 56);
  std::memcpy(header + 108, "vidsMJPG", 8);
  put32(header + 128, stats.frames ? duration : 1);
  put32(header + 132, stats.frames ? stats.frames * 1000 : targetFps);
  put32(header + 140, stats.frames);
  put32(header + 144, stats.maxFrameBytes);
  put32(header + 148, 0xffffffffU);
  put16(header + 160, width);
  put16(header + 162, height);
  std::memcpy(header + 164, "strf", 4);
  put32(header + 168, 40);
  put32(header + 172, 40);
  put32(header + 176, width);
  put32(header + 180, height);
  put16(header + 184, 1);
  put16(header + 186, 24);
  std::memcpy(header + 188, "MJPG", 4);
  put32(header + 192, uint32_t(width) * height * 3);
  std::memcpy(header + 212, "LIST", 4);
  put32(header + 216, 4 + stats.dataBytes);
  std::memcpy(header + 220, "movi", 4);
}

class Sink {
 public:
  virtual ~Sink() = default;
  virtual size_t write(const void *data, size_t bytes) = 0;
  virtual bool seek(uint32_t position) = 0;
  virtual bool sync() = 0;
};

enum class AppendResult { Stored, Rotate, InvalidJpeg, IoError };

class AviWriter {
 public:
  bool begin(Sink &sink, IndexEntry *index, uint32_t capacity, uint16_t width,
             uint16_t height, unsigned targetFps,
             uint32_t fileLimit = kDefaultFileLimit) {
    sink_ = &sink;
    index_ = index;
    capacity_ = capacity;
    width_ = width;
    height_ = height;
    targetFps_ = targetFps;
    fileLimit_ = fileLimit;
    stats_ = {};
    failed_ = false;
    active_ = false;
    if (!index || !capacity || !width || !height || !targetFps ||
        fileLimit < kHeaderBytes + 32) return false;
    uint8_t header[kHeaderBytes];
    makeHeader(header, width, height, targetFps, stats_, false);
    if (!sink.seek(0) || !writeAll(header, sizeof(header))) return false;
    active_ = true;
    return true;
  }

  AppendResult append(const uint8_t *jpeg, size_t bytes) {
    if (!active_ || failed_) return AppendResult::IoError;
    if (!jpeg || bytes < 4 || bytes > 2U * 1024U * 1024U ||
        jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
        jpeg[bytes - 2] != 0xff || jpeg[bytes - 1] != 0xd9)
      return AppendResult::InvalidJpeg;
    const uint64_t chunkBytes = 8 + bytes + (bytes & 1);
    const uint64_t finalBytes = kHeaderBytes + uint64_t(stats_.dataBytes) +
                                chunkBytes + 8 + uint64_t(stats_.frames + 1) * 16;
    if (stats_.frames == capacity_ || finalBytes > fileLimit_)
      return AppendResult::Rotate;
    uint8_t chunk[8] = {'0', '0', 'd', 'c', 0, 0, 0, 0};
    put32(chunk + 4, bytes);
    const uint8_t padding = 0;
    if (!writeAll(chunk, sizeof(chunk)) || !writeAll(jpeg, bytes) ||
        ((bytes & 1) && !writeAll(&padding, 1))) return AppendResult::IoError;
    index_[stats_.frames] = {4 + stats_.dataBytes, uint32_t(bytes)};
    ++stats_.frames;
    stats_.dataBytes += uint32_t(chunkBytes);
    if (bytes > stats_.maxFrameBytes) stats_.maxFrameBytes = bytes;
    return AppendResult::Stored;
  }

  bool sync() {
    if (!active_ || failed_ || !sink_->sync()) {
      failed_ = true;
      return false;
    }
    return true;
  }

  bool finish(uint32_t durationMs) {
    if (!active_ || failed_ || !stats_.frames) return false;
    stats_.durationMs = durationMs ? durationMs : 1;
    uint8_t indexHeader[8] = {'i', 'd', 'x', '1', 0, 0, 0, 0};
    put32(indexHeader + 4, stats_.frames * 16);
    if (!writeAll(indexHeader, sizeof(indexHeader))) return false;
    for (uint32_t i = 0; i < stats_.frames; ++i) {
      uint8_t entry[16] = {'0', '0', 'd', 'c', 0x10, 0, 0, 0};
      put32(entry + 8, index_[i].offset);
      put32(entry + 12, index_[i].bytes);
      if (!writeAll(entry, sizeof(entry))) return false;
    }
    // Commit data and index before publishing a header that advertises them.
    if (!sync()) return false;
    uint8_t header[kHeaderBytes];
    makeHeader(header, width_, height_, targetFps_, stats_, true);
    if (!sink_->seek(0) || !writeAll(header, sizeof(header)) || !sync()) return false;
    active_ = false;
    return true;
  }

  const VideoStats &stats() const { return stats_; }
  uint32_t estimatedFinalBytes() const {
    return kHeaderBytes + stats_.dataBytes + 8 + stats_.frames * 16;
  }

 private:
  bool writeAll(const void *data, size_t bytes) {
    if (sink_->write(data, bytes) == bytes) return true;
    failed_ = true;
    return false;
  }
  Sink *sink_ = nullptr;
  IndexEntry *index_ = nullptr;
  uint32_t capacity_ = 0;
  uint16_t width_ = 0, height_ = 0;
  unsigned targetFps_ = 0;
  uint32_t fileLimit_ = 0;
  VideoStats stats_;
  bool active_ = false, failed_ = false;
};

// Debounced one-shot long press. Holding the button cannot stop and restart.
class HoldButton {
 public:
  bool update(bool pressed, uint32_t now) {
    if (pressed != raw_) {
      raw_ = pressed;
      changedAt_ = now;
    }
    if (uint32_t(now - changedAt_) >= 30 && stable_ != raw_) {
      stable_ = raw_;
      if (stable_) pressedAt_ = now;
      else fired_ = false;
    }
    if (stable_ && !fired_ && uint32_t(now - pressedAt_) >= 1000) {
      fired_ = true;
      return true;
    }
    return false;
  }
 private:
  bool raw_ = false, stable_ = false, fired_ = false;
  uint32_t changedAt_ = 0, pressedAt_ = 0;
};

}  // namespace recorder
