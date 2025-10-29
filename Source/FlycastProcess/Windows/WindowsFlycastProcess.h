#ifdef _WIN32
#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../IFlycastProcess.h"

namespace FlycastComm
{

// DME-style external reader/writer for Flycast (no Flycast changes required).
class WindowsFlycastProcess final : public IFlycastProcess
{
public:
  WindowsFlycastProcess();
  ~WindowsFlycastProcess() override;

  // Locate a running Flycast process. Matches env override first, then common exe names.
  bool findPID() override;

  // Discover guest memory mapping and compute main RAM host address.
  bool obtainEmuRAMInformations() override;

  // Basic read/write mirroring DME signatures.
  bool readFromRAM(u32 offset, char* buffer, size_t size, bool withBSwap) override;
  bool writeToRAM(u32 offset, const char* buffer, size_t size, bool withBSwap) override;

private:
  // Helpers
  bool openProcessHandle(DWORD pid);
  bool isReadableCommittedRegion(const MEMORY_BASIC_INFORMATION& mbi) const;
  bool addressInCommittedRegion(uint64_t addr) const;
  bool isWorkingSetValid(uint64_t addr) const;

  // Triangulate ram_base using regions we see in the process.
  bool locateFlycastArenaBase(uint64_t& out_ram_base);

  // Casing-insensitive match for process name
  static bool nameEqualsI(const wchar_t* a, const wchar_t* b);

private:
  HANDLE m_hProcess = nullptr;

  // Cached committed regions (after VirtualQueryEx)
  struct Region
  {
    uint64_t base;
    uint64_t size;
    DWORD state;
    DWORD protect;
    DWORD type;
  };
  std::vector<Region> m_regions;
};

}  // namespace FlycastComm
#endif
