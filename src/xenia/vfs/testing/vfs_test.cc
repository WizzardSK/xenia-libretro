/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>
#include <vector>

#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/gdfx_util.h"
#include "xenia/vfs/xbe_metadata.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::vfs::test {

TEST_CASE("STFS Decode date and time", "[stfs_decode]") {
  SECTION("10 June 2022 19:46:00 UTC - Decode") {
    const uint16_t date = 0x54CA;
    const uint16_t time = 0x9DBD;
    const uint64_t result = 132993639580000000;

    const uint64_t timestamp = decode_fat_timestamp(date, time);

    REQUIRE(timestamp == result);
  }
}

// Writes a directory entry at |offset| in |image|.
static void WriteGdfxEntry(std::vector<uint8_t>& image, size_t offset,
                           uint16_t node_l, uint16_t node_r, uint32_t sector,
                           uint32_t length, const char* name) {
  const uint8_t name_length = uint8_t(std::strlen(name));
  std::memcpy(&image[offset + 0], &node_l, sizeof(node_l));
  std::memcpy(&image[offset + 2], &node_r, sizeof(node_r));
  std::memcpy(&image[offset + 4], &sector, sizeof(sector));
  std::memcpy(&image[offset + 8], &length, sizeof(length));
  image[offset + 12] = 0;
  image[offset + 13] = name_length;
  std::memcpy(&image[offset + 14], name, name_length);
}

TEST_CASE("GDFX directory padding", "[gdfx]") {
  SECTION("Padding that starts a sector stands in for no entry") {
    REQUIRE(GdfxPaddedEntryOrdinal(0) == 0);
    REQUIRE(GdfxPaddedEntryOrdinal(0x200) == 0);
  }

  SECTION("Padding after an entry stands in for the next sector's first") {
    REQUIRE(GdfxPaddedEntryOrdinal(1) == 0x200);
    REQUIRE(GdfxPaddedEntryOrdinal(0x1F0) == 0x200);
    REQUIRE(GdfxPaddedEntryOrdinal(0xFFFF) == 0x10000);
  }

  SECTION("A lookup follows padding to the next sector") {
    // A two sector root directory at sector 1 whose second entry didn't fit
    // in the first sector.
    std::vector<uint8_t> image(8 * kGdfxSectorSize);
    const size_t root = kGdfxSectorSize;
    WriteGdfxEntry(image, root, 0, 0x1F0, 6, 0x10, "a.bin");
    std::memset(&image[root + 0x1F0 * 4], 0xFF, kGdfxSectorSize - 0x1F0 * 4);
    WriteGdfxEntry(image, root + kGdfxSectorSize, 0, 0, 5, 0x100,
                   "default.xex");

    const GdfxPartitionInfo partition = {0, 1, 2 * kGdfxSectorSize};
    auto location =
        GdfxFindFile(image.data(), image.size(), partition, "default.xex");
    REQUIRE(location);
    REQUIRE(location->offset == 5 * kGdfxSectorSize);
    REQUIRE(location->length == 0x100);
  }

  SECTION("A lookup in an empty directory finds nothing") {
    std::vector<uint8_t> image(4 * kGdfxSectorSize);
    std::memset(&image[kGdfxSectorSize], 0xFF, kGdfxSectorSize);
    const GdfxPartitionInfo partition = {0, 1, kGdfxSectorSize};
    REQUIRE(!GdfxFindFile(image.data(), image.size(), partition, "a.bin"));
  }
}

template <typename T>
static void Put(std::vector<uint8_t>& data, size_t offset, T value) {
  std::memcpy(&data[offset], &value, sizeof(value));
}

// An executable with a certificate and a 4x4 title image in |texel_format|.
static std::vector<uint8_t> MakeXbe(uint32_t texel_format) {
  constexpr uint32_t kBase = 0x10000;
  std::vector<uint8_t> xbe(0x1000 + 0x20 + 4 * 4 * 4);
  std::memcpy(&xbe[0], "XBEH", 4);
  Put<uint32_t>(xbe, 0x104, kBase);
  Put<uint32_t>(xbe, 0x108, 0x1000);
  Put<uint32_t>(xbe, 0x118, kBase + 0x200);
  Put<uint32_t>(xbe, 0x11C, 1);
  Put<uint32_t>(xbe, 0x120, kBase + 0x300);
  // The certificate.
  Put<uint32_t>(xbe, 0x200 + 0x8, 0x4C410005);
  const char16_t name[] = u"Jedi";
  std::memcpy(&xbe[0x200 + 0xC], name, sizeof(name));
  Put<uint32_t>(xbe, 0x200 + 0xA8, 1);
  Put<uint32_t>(xbe, 0x200 + 0xAC, 6);
  // The title image section and its name.
  Put<uint32_t>(xbe, 0x300 + 0xC, 0x1000);
  Put<uint32_t>(xbe, 0x300 + 0x10, 0x20 + 4 * 4 * 4);
  Put<uint32_t>(xbe, 0x300 + 0x14, kBase + 0x400);
  std::memcpy(&xbe[0x400], "$$XTIMAGE", 10);
  // One 4x4 texture, its texels right after the bundle's header.
  std::memcpy(&xbe[0x1000], "XPR0", 4);
  Put<uint32_t>(xbe, 0x1004, 0x20 + 4 * 4 * 4);
  Put<uint32_t>(xbe, 0x1008, 0x20);
  Put<uint32_t>(xbe, 0x1000 + 24, (texel_format << 8) | (2 << 20) | (2 << 24));
  std::memset(&xbe[0x1020], 0x80, 4 * 4 * 4);
  return xbe;
}

TEST_CASE("XBE metadata", "[xbe]") {
  SECTION("The certificate describes the game") {
    const auto xbe = MakeXbe(0x12);
    auto m = ExtractXbeMetadata(xbe.data(), xbe.size());
    REQUIRE(m);
    REQUIRE(m->title_id == 0x4C410005);
    REQUIRE(m->title_name == "Jedi");
    REQUIRE(m->disc_number == 1);
    REQUIRE(m->version == 6);
  }

  SECTION("The title image comes back as PNG") {
    for (uint32_t format : {0x06u, 0x12u}) {
      const auto xbe = MakeXbe(format);
      auto m = ExtractXbeMetadata(xbe.data(), xbe.size());
      REQUIRE(m);
      REQUIRE(m->icon_png.size() > 8);
      REQUIRE(std::memcmp(m->icon_png.data(), "\x89PNG", 4) == 0);
    }
  }

  SECTION("A format it doesn't decode leaves no icon") {
    const auto xbe = MakeXbe(0x0B);
    auto m = ExtractXbeMetadata(xbe.data(), xbe.size());
    REQUIRE(m);
    REQUIRE(m->icon_png.empty());
  }

  SECTION("Anything else is not an executable") {
    auto xbe = MakeXbe(0x12);
    xbe[0] = 0;
    REQUIRE(!ExtractXbeMetadata(xbe.data(), xbe.size()));
  }
}

}  // namespace xe::vfs::test
