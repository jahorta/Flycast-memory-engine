#ifdef __linux__

#include "LinuxFlycastProcess.h"
#include "../../Common/CommonUtils.h"
#include "../../Common/MemoryCommon.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/uio.h>
#include <unordered_map>

namespace FlycastComm
{

// --- helpers -----------------------------------------------------------------

static bool parse_maps_line(const std::string& line, uint64_t& start, uint64_t& end,
                            std::string& perms)
{
  // Format: start-end perms offset dev inode [pathname]
  // Example: 55c40b9e5000-55c40bc0a000 rw-p 00000000 00:00 0                          [heap]
  std::istringstream iss(line);
  std::string addresses;
  if (!(iss >> addresses))
    return false;
  if (!(iss >> perms))
    return false;

  const auto dash = addresses.find('-');
  if (dash == std::string::npos)
    return false;

  start = std::stoull(addresses.substr(0, dash), nullptr, 16);
  end = std::stoull(addresses.substr(dash + 1), nullptr, 16);
  return end > start;
}

bool LinuxFlycastProcess::readProcMaps(std::vector<Region>& out_regions)
{
  out_regions.clear();
  std::ifstream maps("/proc/" + std::to_string(m_PID) + "/maps");
  if (!maps.is_open())
    return false;

  std::string line;
  while (std::getline(maps, line))
  {
    uint64_t start = 0, end = 0;
    std::string perms;
    if (!parse_maps_line(line, start, end, perms))
      continue;

    // Keep readable & writable regions (Flycast RAM/VRAM/ARAM are RW)
    if (perms.size() >= 2 && perms[0] == 'r' && perms[1] == 'w')
    {
      Region r;
      r.base = start;
      r.size = end - start;
      out_regions.push_back(r);
    }
  }
  return !out_regions.empty();
}

bool LinuxFlycastProcess::addressInRegionList(uint64_t addr, const std::vector<Region>& regs) const
{
  for (const auto& r : regs)
  {
    if (addr >= r.base && addr < (r.base + r.size))
      return true;
  }
  return false;
}

bool LinuxFlycastProcess::triangulateArenaBase(const std::vector<Region>& regs,
                                               uint64_t& out_ram_base) const
{
  // Flycast virtmem fixed layout:
  //   VRAM  at ram_base + 0x04000000
  //   MAIN  at ram_base + 0x0C000000
  //   AICA  at ram_base + 0x20000000
  static constexpr uint64_t OFF_VRAM = 0x04000000ull;
  static constexpr uint64_t OFF_MAIN = 0x0C000000ull;
  static constexpr uint64_t OFF_AICA = 0x20000000ull;

  struct Acc
  {
    int v = 0, m = 0, a = 0;
  };
  std::unordered_map<uint64_t, Acc> cand;

  for (const auto& r : regs)
  {
    cand[r.base - OFF_VRAM].v++;
    cand[r.base - OFF_MAIN].m++;
    cand[r.base - OFF_AICA].a++;
  }

  uint64_t best = 0;
  int bestScore = -1;

  for (const auto& kv : cand)
  {
    const uint64_t base = kv.first;
    const Acc& acc = kv.second;
    if (base == 0)
      continue;

    const uint64_t main_addr = base + OFF_MAIN;
    const uint64_t vram_addr = base + OFF_VRAM;

    if (!addressInRegionList(main_addr, regs))
      continue;
    if (!addressInRegionList(vram_addr, regs))
      continue;

    int score = (acc.m > 0) + (acc.v > 0) + (acc.a > 0);
    if (score > bestScore)
    {
      bestScore = score;
      best = base;
    }
  }

  if (bestScore >= 2)
  {
    out_ram_base = best;
    return true;
  }
  return false;
}

// --- PID discovery -----------------------------------------------------------

bool LinuxFlycastProcess::findPID()
{
  DIR* dp = opendir("/proc/");
  if (!dp)
    return false;

  const char* envOverride = std::getenv("DME_FLYCAST_PROCESS_NAME");

  m_PID = -1;
  dirent* de = nullptr;
  while ((de = readdir(dp)))
  {
    // numeric PID dirs only
    char* endp = nullptr;
    long aPID = strtol(de->d_name, &endp, 10);
    if (!de->d_name[0] || *endp != '\0')
      continue;

    std::ifstream comm("/proc/" + std::string(de->d_name) + "/comm");
    std::string name;
    if (!comm.is_open() || !std::getline(comm, name))
      continue;

    bool match = false;
    if (envOverride)
      match = (name == envOverride);
    else
      match = (name == "flycast" || name == "flycast-qt" || name == "flycast-qt6");

    if (match)
    {
      m_PID = static_cast<int>(aPID);
      break;
    }
  }
  closedir(dp);
  return m_PID != -1;
}

// --- obtain RAM info ---------------------------------------------------------

bool LinuxFlycastProcess::obtainEmuRAMInformations()
{
  if (m_PID <= 0)
    return false;

  std::vector<Region> regs;
  if (!readProcMaps(regs))
    return false;

  uint64_t ram_base = 0;
  if (!triangulateArenaBase(regs, ram_base))
    return false;

  static constexpr uint64_t OFF_VRAM = 0x04000000ull;
  static constexpr uint64_t OFF_MAIN = 0x0C000000ull;
  static constexpr uint64_t OFF_AICA = 0x20000000ull;

  m_emuRAMAddressStart = ram_base + OFF_MAIN;
  m_emuARAMAdressStart = ram_base + OFF_AICA;  // optional, often present
  m_ARAMAccessible = true;

  // Probe read a few bytes at RAM start
  uint8_t probe[16] = {};
  iovec local{probe, sizeof(probe)};
  iovec remote{reinterpret_cast<void*>(m_emuRAMAddressStart), sizeof(probe)};
  ssize_t nread = process_vm_readv(m_PID, &local, 1, &remote, 1, 0);
  if (nread != static_cast<ssize_t>(sizeof(probe)))
    return false;

  return true;
}

// --- read/write --------------------------------------------------------------

bool LinuxFlycastProcess::readFromRAM(u32 offset, char* buffer, size_t size, bool withBSwap)
{
  if (m_PID <= 0 || !buffer || size == 0 || m_emuRAMAddressStart == 0)
    return false;

  uint64_t addr = m_emuRAMAddressStart + static_cast<uint64_t>(offset);
  iovec local{buffer, size};
  iovec remote{reinterpret_cast<void*>(addr), size};

  const ssize_t nread = process_vm_readv(m_PID, &local, 1, &remote, 1, 0);
  if (nread != static_cast<ssize_t>(size))
    return false;

  if (withBSwap)
  {
    switch (size)
    {
    case 2:
    {
      uint16_t v;
      std::memcpy(&v, buffer, 2);
      v = Common::bSwap16(v);
      std::memcpy(buffer, &v, 2);
      break;
    }
    case 4:
    {
      uint32_t v;
      std::memcpy(&v, buffer, 4);
      v = Common::bSwap32(v);
      std::memcpy(buffer, &v, 4);
      break;
    }
    case 8:
    {
      uint64_t v;
      std::memcpy(&v, buffer, 8);
      v = Common::bSwap64(v);
      std::memcpy(buffer, &v, 8);
      break;
    }
    default:
      break;
    }
  }
  return true;
}

bool LinuxFlycastProcess::writeToRAM(u32 offset, const char* buffer, size_t size, bool withBSwap)
{
  if (m_PID <= 0 || !buffer || size == 0 || m_emuRAMAddressStart == 0)
    return false;

  std::vector<uint8_t> tmp;
  const void* src = buffer;
  if (withBSwap)
  {
    tmp.assign(buffer, buffer + size);
    switch (size)
    {
    case 2:
    {
      uint16_t v;
      std::memcpy(&v, tmp.data(), 2);
      v = Common::bSwap16(v);
      std::memcpy(tmp.data(), &v, 2);
      break;
    }
    case 4:
    {
      uint32_t v;
      std::memcpy(&v, tmp.data(), 4);
      v = Common::bSwap32(v);
      std::memcpy(tmp.data(), &v, 4);
      break;
    }
    case 8:
    {
      uint64_t v;
      std::memcpy(&v, tmp.data(), 8);
      v = Common::bSwap64(v);
      std::memcpy(tmp.data(), &v, 8);
      break;
    }
    default:
      break;
    }
    src = tmp.data();
  }

  uint64_t addr = m_emuRAMAddressStart + static_cast<uint64_t>(offset);
  iovec local{const_cast<void*>(src), size};
  iovec remote{reinterpret_cast<void*>(addr), size};
  const ssize_t nwrote = process_vm_writev(m_PID, &local, 1, &remote, 1, 0);
  return nwrote == static_cast<ssize_t>(size);
}

}  // namespace FlycastComm
#endif
