/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <cstddef>
#include <string>

#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/devices/host_path_file.h>
#include <rex/filesystem/entry.h>
#include <rex/logging.h>

namespace rex::filesystem {

namespace {
// [DIAG5] Added 2026-09-12 (fable2_052.log round / "Round 8"). Per explicit
// user direction, this is a targeted trace of the REAL file-read/streaming
// path for gui_textures.bnk, replacing further blind guest-memory scanning
// (DIAG3/DIAG4 in glyph_system.cpp, both exhausted in round 7/8). This is the
// generic host-file read implementation used for ALL host-backed files
// (HostPathFile does not override ReadAsync, so async requests fall back
// here too) -- see AI_HANDOFF.md "Round 8" section for the full pipeline
// hypothesis this feeds into.
//
// Button A's real 64x64 RGBA8 pixel payload was located, via static offline
// binary-diff analysis (round 7), at this exact byte range inside
// gui_textures.bnk (and its _kbm/_x360 sibling variants -- all three files
// share this layout since they differ only in these button-pixel clusters):
constexpr size_t kDiagAPayloadFileOffset = 0x140D555;
constexpr size_t kDiagAPayloadSize = 16384;
// Bounded so this can never spam the log even if the range is re-read many
// times (e.g. re-streamed every load, or read in smaller chunks).
constexpr int kDiagAReadLogCap = 32;
std::atomic<int> g_diagAReadLogCount{0};
}  // namespace

HostPathFile::HostPathFile(uint32_t file_access, HostPathEntry* entry,
                           std::unique_ptr<rex::filesystem::FileHandle> file_handle)
    : File(file_access, entry), file_handle_(std::move(file_handle)) {}

HostPathFile::~HostPathFile() = default;

void HostPathFile::Destroy() {
  delete this;
}

X_STATUS HostPathFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                size_t* out_bytes_read) {
  if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  // [DIAG5] Log reads that overlap button A's known pixel-payload byte range
  // inside gui_textures*.bnk, to reveal the destination buffer address this
  // generic host-file-read path lands the data in (and, by name/offset, to
  // finally confirm -- rather than assume -- when and how that specific 16KB
  // range is actually consumed). Name-filtered and bounded, so this cannot
  // affect or spam unrelated file I/O.
  if (entry_ != nullptr) {
    const std::string& diag_entry_name = entry_->name();
    if (diag_entry_name.find("gui_textures") != std::string::npos) {
      const size_t range_start = byte_offset;
      const size_t range_end = byte_offset + buffer.size();
      const size_t payload_start = kDiagAPayloadFileOffset;
      const size_t payload_end = kDiagAPayloadFileOffset + kDiagAPayloadSize;
      const bool overlaps = range_start < payload_end && payload_start < range_end;
      if (overlaps &&
          g_diagAReadLogCount.fetch_add(1, std::memory_order_relaxed) < kDiagAReadLogCap) {
        REXFS_INFO(
            "[DIAG5] HostPathFile::ReadSync entry='{}' overlaps known A-button "
            "payload range [0x{:X}, 0x{:X}): read_offset=0x{:X} read_size={} "
            "dest_buffer={:p}",
            diag_entry_name, payload_start, payload_end, byte_offset, buffer.size(),
            fmt::ptr(buffer.data()));
      }
    }
  }

  if (file_handle_->Read(byte_offset, buffer.data(), buffer.size(), out_bytes_read)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_written) {
  if (!(file_access_ &
        (FileAccess::kGenericWrite | FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(), out_bytes_written)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::SetLength(size_t length) {
  if (!(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->SetLength(length)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

}  // namespace rex::filesystem
