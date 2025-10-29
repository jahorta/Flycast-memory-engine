#ifdef __APPLE__

#include "MacFlycastProcess.h"
#include "../../Common/CommonUtils.h"
#include "../../Common/MemoryCommon.h"

#include <mach/mach_vm.h>
#include <memory>
#include <string>
#include <string_view>
#include <sys/sysctl.h>
#include <vector>

namespace FlycastComm
{

// --- PID discovery (sysctl) --------------------------------------------------

bool MacFlycastProcess::findPID()
{
  static const int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
  size_t size = 0;
  if (sysctl((int*)mib, 4, nullptr, &size, nullptr, 0) == -1)
    return false;

  auto procs = std::make_unique<kinfo_proc[]>(size / sizeof(kinfo_proc));
  if (sysctl((int*)mib, 4, procs.get(), &size, nullptr, 0) == -1)
    return false;

  const char* envOverride = std::getenv("DME_FLYCAST_PROCESS_NAME");

  m_PID = -1;
  for (size_t i = 0; i < size / sizeof(kinfo_proc); ++i)
  {
    const std::string_view name{procs[i].kp_proc.p_comm};
    bool match = false;
    if (envOverride)
      match = (name == envOverride);
    else
      match =
          (name == "Flycast" || name == "flycast" || name == "flycast-qt" || name == "flycast-qt6");
    if (match)
      m_PID = procs[i].kp_proc.p_pid;
  }
  return m_PID != -1;
}

// --- region enumeration & triangulation -------------------------------------

bool MacFlycastProcess::enumerateRegions(std::vector<Region>& out)
{
  out.clear();

  m_currentTask = current_task();
  if (task_for_pid(m_currentTask, m_PID, &m_task) != KERN_SUCCESS)
    return false;

  mach_vm_address_t addr = 0;
  mach_vm_size_t sz = 0;
  while (true)
  {
    vm_region_basic_info_data_64_t binfo{};
    mach_msg_type_number_t bcnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;

    kern_return_t kr = mach_vm_region(m_task, &addr, &sz, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&binfo, &bcnt, &obj);
    if (kr != KERN_SUCCESS)
      break;

    // keep RW regions only
    if ((binfo.protection & VM_PROT_READ) && (binfo.protection & VM_PROT_WRITE))
      out.push_back(Region{static_cast<uint64_t>(addr), static_cast<uint64_t>(sz)});

    addr += sz;
  }

  return !out.empty();
}

bool MacFlycastProcess::addressInRegions(uint64_t a, const std::vector<Region>& regs) const
{
  for (const auto& r : regs)
    if (a >= r.base && a < (r.base + r.size))
      return true;
  return false;
}

bool MacFlycastProcess::triangulateArenaBase(const std::vector<Region>& regs,
                                             uint64_t& out_ram_base) const
{
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

    if (!addressInRegions(main_addr, regs))
      continue;
    if (!addressInRegions(vram_addr, regs))
      continue;

    const int score = (acc.m > 0) + (acc.v > 0) + (acc.a > 0);
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

// --- obtain RAM info ---------------------------------------------------------

bool MacFlycastProcess::obtainEmuRAMInformations()
{
  if (m_PID <= 0)
    return false;

  std::vector<Region> regs;
  if (!enumerateRegions(regs))
    return false;

  uint64_t ram_base = 0;
  if (!triangulateArenaBase(regs, ram_base))
    return false;

  static constexpr uint64_t OFF_VRAM = 0x04000000ull;
  static constexpr uint64_t OFF_MAIN = 0x0C000000ull;
  static constexpr uint64_t OFF_AICA = 0x20000000ull;

  m_emuRAMAddressStart = ram_base + OFF_MAIN;
  m_emuARAMAdressStart = ram_base + OFF_AICA;
  m_ARAMAccessible = true;

  // Probe read
  uint8_t probe[16] = {};
  vm_size_t nread = 0;
  const kern_return_t kr =
      vm_read_overwrite(m_task, static_cast<mach_vm_address_t>(m_emuRAMAddressStart), sizeof(probe),
                        reinterpret_cast<vm_address_t>(probe), &nread);
  if (kr != KERN_SUCCESS || nread != sizeof(probe))
    return false;

  return true;
}

// --- read/write --------------------------------------------------------------

bool MacFlycastProcess::readFromRAM(u32 offset, char* buffer, size_t size, bool withBSwap)
{
  if (!buffer || size == 0 || m_emuRAMAddressStart == 0)
    return false;

  vm_size_t nread = 0;
  const mach_vm_address_t addr =
      static_cast<mach_vm_address_t>(m_emuRAMAddressStart + static_cast<uint64_t>(offset));
  if (vm_read_overwrite(m_task, addr, size, reinterpret_cast<vm_address_t>(buffer), &nread) !=
      KERN_SUCCESS)
    return false;
  if (nread != size)
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

bool MacFlycastProcess::writeToRAM(u32 offset, const char* buffer, size_t size, bool withBSwap)
{
  if (!buffer || size == 0 || m_emuRAMAddressStart == 0)
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

  const mach_vm_address_t addr =
      static_cast<mach_vm_address_t>(m_emuRAMAddressStart + static_cast<uint64_t>(offset));
  // vm_write takes ownership of the provided memory for the duration of the call only.
  kern_return_t kr = vm_write(m_task, addr, reinterpret_cast<vm_offset_t>(src),
                              static_cast<mach_msg_type_number_t>(size));
  return kr == KERN_SUCCESS;
}

}  // namespace FlycastComm
#endif
