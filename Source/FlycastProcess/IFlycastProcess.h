// Interface to perform operations in Flycast's process
#pragma once

#include <cstddef>

#include "../Common/CommonTypes.h"

namespace FlycastComm
{
class IFlycastProcess
{
public:
  IFlycastProcess() = default;
  virtual ~IFlycastProcess() = default;

  IFlycastProcess(const IFlycastProcess&) = delete;
  IFlycastProcess& operator=(const IFlycastProcess&) = delete;

  // Process discovery & memory mapping
  virtual bool findPID() = 0;
  virtual bool obtainEmuRAMInformations() = 0;

  // Raw reads/writes using RAM-offset addressing
  virtual bool readFromRAM(u32 offset, char* buffer, size_t size, bool withBSwap) = 0;
  virtual bool writeToRAM(u32 offset, const char* buffer, size_t size, bool withBSwap) = 0;

  // --- helpers/getters (non-virtual) ----------------------------------------
  int getPID() const { return m_PID; }
  u64 getEmuRAMAddressStart() const { return m_emuRAMAddressStart; };

  // Start address of ARAM/AICA (if discovered)
  u64 getARAMAddressStart() const { return m_emuARAMAdressStart; }
  bool isARAMAccessible() const { return m_ARAMAccessible; }

  // Offset from RAM to ARAM, 0 if ARAM not accessible.
  u64 getARAMOffsetFromRAM() const
  {
    if (!m_ARAMAccessible || m_emuARAMAdressStart == 0 || m_emuRAMAddressStart == 0)
      return 0;
    return m_emuARAMAdressStart - m_emuRAMAddressStart;
  }

protected:
  int m_PID = -1;
  u64 m_emuRAMAddressStart = 0;   // main RAM (SH-4 RAM) base in host
  u64 m_emuARAMAdressStart = 0;   // AICA/ARAM base in host (optional)
  bool m_ARAMAccessible = false;  // true if ARAM mapped & usable via RPM/WPM
};
}  // namespace FlycastComm
