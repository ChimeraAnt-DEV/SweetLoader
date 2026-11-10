/**
 * @file CapturedImageSlotTest.cpp
 * @brief Host test for the captured-image slot's content-addressed write.
 *
 * A skin or cape can be re-uploaded unchanged (resource-pack reload, a skin change, a re-join), and
 * the capture path allocated a fresh vector + copied the payload on every such upload. The slot now
 * compares against what it already holds and skips the allocation when the bytes are identical.
 */
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "pl/runtime/ImageOverrideRegistry.hpp"

using pl::runtime::CapturedImageSlot;

namespace {
int checks = 0;
void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}
std::vector<std::uint8_t> Bytes(std::size_t n, std::uint8_t v) { return std::vector<std::uint8_t>(n, v); }
} // namespace

int main() {
  CapturedImageSlot slot;

  // A first write is always taken.
  {
    const auto bytes = Bytes(16, 0xAB);
    expect(slot.setIfChanged(bytes.data(), bytes.size(), 4, 4, 0, 1000),
           "the first write is accepted");
    expect(slot.has(), "the slot holds the image");
    expect(slot.get()->bytes.size() == 16, "the payload was stored");
  }

  // An identical re-upload is skipped: no new object, no copy, same stamp as before.
  {
    const auto bytes = Bytes(16, 0xAB);
    const auto before = slot.get();
    expect(!slot.setIfChanged(bytes.data(), bytes.size(), 4, 4, 0, 2000),
           "an identical re-upload is skipped");
    expect(slot.get() == before, "the held snapshot is unchanged");
    expect(slot.get()->stampMs == 1000, "the stamp was not advanced for an identical payload");
  }

  // A changed payload is taken, and the stamp advances.
  {
    const auto bytes = Bytes(16, 0xCD);
    expect(slot.setIfChanged(bytes.data(), bytes.size(), 4, 4, 0, 3000),
           "a changed payload is accepted");
    expect(slot.get()->bytes[0] == 0xCD, "the new payload replaced the old");
    expect(slot.get()->stampMs == 3000, "the stamp advanced");
  }

  // A same-size, same-shape payload that differs in one byte is taken (not a false "unchanged").
  {
    auto bytes = Bytes(16, 0xCD);
    bytes[7] = 0x01;
    expect(slot.setIfChanged(bytes.data(), bytes.size(), 4, 4, 0, 4000),
           "a one-byte change is detected");
    expect(slot.get()->bytes[7] == 0x01, "the changed byte was stored");
  }

  // A shape change with the same bytes is taken: the launcher keys the image on its dimensions.
  {
    const auto bytes = Bytes(16, 0xCD);
    expect(slot.setIfChanged(bytes.data(), bytes.size(), 2, 8, 0, 5000),
           "a shape change is accepted even with identical bytes");
    expect(slot.get()->width == 2 && slot.get()->height == 8, "the new shape was stored");
  }

  // An empty write clears the slot (fail-closed: no half image is ever held).
  {
    expect(!slot.setIfChanged(nullptr, 0, 0, 0, 0, 6000), "an empty write is refused");
    expect(!slot.has(), "the slot was cleared");
  }

  std::printf("CapturedImageSlotTest: %d checks passed\n", checks);
  return 0;
}
