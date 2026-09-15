#include "../../firmware/camera_recorder/avi_writer.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

class MemorySink : public recorder::Sink {
 public:
  std::vector<uint8_t> data;
  size_t position = 0;
  size_t failAfter = std::numeric_limits<size_t>::max();
  bool syncOkay = true;
  size_t write(const void *bytes, size_t size) override {
    const size_t room = position < failAfter ? failAfter - position : 0;
    const size_t count = size < room ? size : room;
    if (position + count > data.size()) data.resize(position + count);
    if (count) std::memcpy(data.data() + position, bytes, count);
    position += count;
    return count;
  }
  bool seek(uint32_t offset) override {
    if (offset > data.size()) return false;
    position = offset;
    return true;
  }
  bool sync() override { return syncOkay; }
};

int main(int argc, char **argv) {
  assert(argc == 3);
  std::ifstream input(argv[1], std::ios::binary);
  const std::vector<uint8_t> jpeg{std::istreambuf_iterator<char>(input), {}};
  assert(jpeg.size() > 4 && jpeg.size() % 2 == 1);
  recorder::IndexEntry index[16];
  recorder::AviWriter writer;
  MemorySink normal;
  assert(writer.begin(normal, index, 16, 32, 24, 10));
  for (unsigned i = 0; i < 7; ++i)
    assert(writer.append(jpeg.data(), jpeg.size()) == recorder::AppendResult::Stored);
  assert(writer.finish(900));
  assert(!writer.finish(900));
  std::ofstream output(argv[2], std::ios::binary);
  output.write(reinterpret_cast<const char *>(normal.data.data()), normal.data.size());
  output.close();

  MemorySink capacity;
  assert(writer.begin(capacity, index, 1, 32, 24, 10));
  assert(writer.append(jpeg.data(), jpeg.size()) == recorder::AppendResult::Stored);
  const size_t savedSize = capacity.data.size();
  assert(writer.append(jpeg.data(), jpeg.size()) == recorder::AppendResult::Rotate);
  assert(capacity.data.size() == savedSize && writer.stats().frames == 1);
  assert(writer.finish(200));

  MemorySink bytesLimit;
  const uint32_t oneFrameLimit = 224 + 8 + jpeg.size() + 1 + 8 + 16;
  assert(writer.begin(bytesLimit, index, 16, 32, 24, 10, oneFrameLimit));
  assert(writer.append(jpeg.data(), jpeg.size()) == recorder::AppendResult::Stored);
  assert(writer.append(jpeg.data(), jpeg.size()) == recorder::AppendResult::Rotate);
  assert(writer.finish(100));
  assert(bytesLimit.data.size() == oneFrameLimit);

  MemorySink shortHeader;
  shortHeader.failAfter = 100;
  assert(!writer.begin(shortHeader, index, 16, 32, 24, 10));
  assert(writer.append(jpeg.data(), jpeg.size()) == recorder::AppendResult::IoError);

  MemorySink shortPayload;
  assert(writer.begin(shortPayload, index, 16, 32, 24, 10));
  shortPayload.failAfter = 224 + 9;
  assert(writer.append(jpeg.data(), jpeg.size()) == recorder::AppendResult::IoError);
  assert(writer.stats().frames == 0 && !writer.finish(100));
  assert(recorder::get32(shortPayload.data.data() + 44) == 0);

  MemorySink shortIndex;
  assert(writer.begin(shortIndex, index, 16, 32, 24, 10));
  assert(writer.append(jpeg.data(), jpeg.size()) == recorder::AppendResult::Stored);
  shortIndex.failAfter = shortIndex.data.size() + 10;
  assert(!writer.finish(100));
  assert(recorder::get32(shortIndex.data.data() + 44) == 0);

  MemorySink failedSync;
  assert(writer.begin(failedSync, index, 16, 32, 24, 10));
  assert(writer.append(jpeg.data(), jpeg.size()) == recorder::AppendResult::Stored);
  failedSync.syncOkay = false;
  assert(!writer.sync() && !writer.finish(100));
  assert(recorder::get32(failedSync.data.data() + 44) == 0);

  MemorySink invalid;
  assert(writer.begin(invalid, index, 16, 32, 24, 10));
  const uint8_t bad[] = {0, 0, 0, 0};
  assert(writer.append(bad, sizeof(bad)) == recorder::AppendResult::InvalidJpeg);
  assert(invalid.data.size() == 224 && !writer.finish(100));

  recorder::HoldButton button;
  assert(!button.update(false, 0));
  assert(!button.update(true, 100));
  assert(!button.update(false, 110));
  assert(!button.update(true, 120));
  assert(!button.update(true, 151));
  assert(!button.update(true, 1150));
  assert(button.update(true, 1151));
  assert(!button.update(true, 5000));  // A long hold never toggles twice.
  assert(!button.update(false, 5001));
  assert(!button.update(false, 5031));
  assert(!button.update(true, 5100));
  assert(!button.update(true, 5130));
  assert(button.update(true, 6130));

  recorder::HoldButton wrapped;
  const uint32_t start = 0xfffffe00U;
  assert(!wrapped.update(true, start));
  assert(!wrapped.update(true, start + 30));
  assert(wrapped.update(true, start + 1030));
  assert(!wrapped.update(true, start + 2030));
}
