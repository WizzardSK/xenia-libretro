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

}  // namespace xe::vfs::test
