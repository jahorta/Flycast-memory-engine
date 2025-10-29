// Wrapper around IFlycastProcess (DME-style, offset-based RAM access)
#pragma once

#include "../Common/CommonTypes.h"
#include "../Common/MemoryCommon.h"
#include "IFlycastProcess.h"

#include <string>

namespace FlycastComm
{
class FlycastAccessor
{
public:
  enum class FlycastStatus
  {
    hooked,      // attached & RAM discovered
    notRunning,  // process not found
    noEmu,       // process found but RAM base unresolved
    unHooked     
  };

  static void init();
  static void free();

  // Initialize/find process and resolve RAM base.
  static void hook();
  static void unHook();

  // Read/write at RAM+offset (Dreamcast is little-endian)
  static bool readFromRAM(u32 offset, char* buffer, size_t size, bool withBSwap);
  static bool writeToRAM(u32 offset, const char* buffer, size_t size, bool withBSwap);
  static Common::MemOperationReturnCode readEntireRAM(char* buffer);
  static std::string getFormattedValueFromMemory(u32 ramIndex, Common::MemType memType,
                                                 size_t memSize, Common::MemBase memBase,
                                                 bool memIsUnsigned);

  // Status / metadata
  static int getPID();
  static u64 getEmuRAMAddressStart();
  static FlycastStatus getStatus();
  static bool isARAMAccessible();
  static u64 getRAMAddressStart();
  static u64 getARAMAddressStart();
  static bool isMEM2Present() { return false; }

  // Optional helper (returns 0 if unknown); Flycast SH-4 RAM is usually 16 MiB.
  static size_t getRAMTotalSize();
  static bool isValidConsoleAddress(u32 address);

private:
  static IFlycastProcess* m_instance;
  static FlycastStatus m_status;
};
}  // namespace FlycastComm
