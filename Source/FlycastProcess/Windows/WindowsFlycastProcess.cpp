#ifdef _WIN32

#include "WindowsFlycastProcess.h"

#include <Psapi.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>
#include <tlhelp32.h>
#include <unordered_map>
#include <vector>

#include "../../Common/MemoryCommon.h"  // for Common::bSwapXX helpers if you want withBSwap parity
#include "../../Common/CommonUtils.h"

namespace FlycastComm
{

// ---- small utils ------------------------------------------------------------

static std::wstring to_wlower(std::wstring s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towlower(c); });
  return s;
}

bool WindowsFlycastProcess::nameEqualsI(const wchar_t* a, const wchar_t* b)
{
  return to_wlower(a) == to_wlower(b);
}

WindowsFlycastProcess::WindowsFlycastProcess() = default;

WindowsFlycastProcess::~WindowsFlycastProcess()
{
  if (m_hProcess)
  {
    CloseHandle(m_hProcess);
    m_hProcess = nullptr;
  }
}

// ---- PID discovery ----------------------------------------------------------

bool WindowsFlycastProcess::openProcessHandle(DWORD pid)
{
  HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                             PROCESS_VM_OPERATION,
                         FALSE, pid);
  if (!h)
    return false;
  m_hProcess = h;
  m_PID = static_cast<int>(pid);
  return true;
}

bool WindowsFlycastProcess::findPID()
{
  // Optional env override (mirroring DME style)
  wchar_t envBuf[512];
  std::wstring overrideName;
  if (GetEnvironmentVariableW(L"FME_FLYCAST_PROCESS_NAME", envBuf, 512) > 0)
  {
    overrideName = to_wlower(envBuf);
  }

  const std::wstring defaultName = L"flycast.exe";

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return false;

  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(PROCESSENTRY32W);
  bool found = false;

  if (Process32FirstW(snap, &pe))
  {
    do
    {
      const std::wstring exe = to_wlower(pe.szExeFile);
      bool match = false;
      if (!overrideName.empty())
        match = (exe == overrideName);
      else
        match = (exe == defaultName);

      if (match)
      {
        if (openProcessHandle(pe.th32ProcessID))
        {
          found = true;
          break;
        }
      }
    } while (Process32NextW(snap, &pe));
  }

  CloseHandle(snap);
  return found;
}

// ---- region capture ---------------------------------------------------------

bool WindowsFlycastProcess::isReadableCommittedRegion(const MEMORY_BASIC_INFORMATION& mbi) const
{
  if (mbi.State != MEM_COMMIT)
    return false;
  // Exclude guard / noaccess
  if (mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD))
    return false;
  // Accept readable types
  return (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED);
}

bool WindowsFlycastProcess::addressInCommittedRegion(uint64_t addr) const
{
  for (const auto& r : m_regions)
  {
    if (addr >= r.base && addr < (r.base + r.size))
      return true;
  }
  return false;
}

bool WindowsFlycastProcess::isWorkingSetValid(uint64_t addr) const
{
  PSAPI_WORKING_SET_EX_INFORMATION ws{};
  ws.VirtualAddress = reinterpret_cast<void*>(addr);
  if (!QueryWorkingSetEx(m_hProcess, &ws, sizeof(ws)))
  {
    // If the query fails (e.g., permissions), don't veto.
    return true;
  }
  return ws.VirtualAttributes.Valid;
}

// ---- triangulation of ram_base ---------------------------------------------

// We use Flycast's fixed layout when virtmem is enabled:
//   VRAM  at ram_base + 0x04000000
//   Main  at ram_base + 0x0C000000
//   AICA  at ram_base + 0x20000000
// We don't need exact region sizes—only that committed regions exist at those offsets.
bool WindowsFlycastProcess::locateFlycastArenaBase(uint64_t& out_ram_base)
{
  // Collect committed regions from the target.
  m_regions.clear();
  uint8_t* p = nullptr;
  MEMORY_BASIC_INFORMATION mbi{};
  while (VirtualQueryEx(m_hProcess, p, &mbi, sizeof(mbi)) == sizeof(mbi))
  {
    if (isReadableCommittedRegion(mbi))
    {
      m_regions.push_back(Region{reinterpret_cast<uint64_t>(mbi.BaseAddress),
                                 static_cast<uint64_t>(mbi.RegionSize), mbi.State, mbi.Protect,
                                 mbi.Type});
    }
    // Move to next region
    uint8_t* next = reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    if (next <= p)  // overflow guard
      break;
    p = next;
  }
  if (m_regions.empty())
    return false;

  // Build candidate bases by subtracting the expected offsets from observed region bases.
  const uint64_t OFF_VRAM = 0x04000000ull;
  const uint64_t OFF_MAIN = 0x0C000000ull;
  const uint64_t OFF_AICA = 0x20000000ull;

  struct CandAcc
  {
    int hits_vram = 0;
    int hits_main = 0;
    int hits_aica = 0;
  };
  std::unordered_map<uint64_t, CandAcc> cand;

  for (const auto& r : m_regions)
  {
    // For each region, treat it as if it were at base+offset and accumulate a candidate base.
    cand[r.base - OFF_VRAM].hits_vram++;
    cand[r.base - OFF_MAIN].hits_main++;
    cand[r.base - OFF_AICA].hits_aica++;
  }

  // Pick a candidate that has at least VRAM+MAIN alignment and points into committed regions.
  uint64_t bestBase = 0;
  int bestScore = -1;

  for (const auto& [base, acc] : cand)
  {
    if (base == 0)
      continue;  // unlikely sentinel

    const uint64_t cand_main = base + OFF_MAIN;
    const uint64_t cand_vram = base + OFF_VRAM;

    if (!addressInCommittedRegion(cand_main))
      continue;
    if (!addressInCommittedRegion(cand_vram))
      continue;

    // Optional: sanity using working set validity (don't fail hard if it says false)
    const bool ws_ok_main = isWorkingSetValid(cand_main);
    const bool ws_ok_vram = isWorkingSetValid(cand_vram);
    if (!ws_ok_main || !ws_ok_vram)
    {
      // continue; // if you want to be strict; otherwise, just reduce score
    }

    // Score: prefer candidates seen from at least two offsets; tie-break with aica hit.
    int score =
        (acc.hits_main >= 1 ? 1 : 0) + (acc.hits_vram >= 1 ? 1 : 0) + (acc.hits_aica >= 1 ? 1 : 0);
    if (score > bestScore)
    {
      bestScore = score;
      bestBase = base;
    }
  }

  if (bestScore < 2)
    return false;

  out_ram_base = bestBase;
  return true;
}

// ---- public: obtain RAM info -----------------------------------------------

bool WindowsFlycastProcess::obtainEmuRAMInformations()
{
  if (!m_hProcess || m_PID <= 0)
    return false;

  uint64_t ram_base = 0;
  if (!locateFlycastArenaBase(ram_base))
    return false;

  // Cache the computed starts for DME-style accessors
  const uint64_t OFF_MAIN = 0x0C000000ull;
  const uint64_t OFF_VRAM = 0x04000000ull;
  const uint64_t OFF_AICA = 0x20000000ull;

  m_emuRAMAddressStart = ram_base + OFF_MAIN;
  m_emuARAMAdressStart = ram_base + OFF_AICA;  // optional, some DME code may use this
  m_ARAMAccessible = true;                     // We know a writable AICA window exists

  // Quick probe read to ensure it's readable
  uint8_t probe[16] = {};
  SIZE_T read = 0;
  if (!ReadProcessMemory(m_hProcess, reinterpret_cast<LPCVOID>(m_emuRAMAddressStart), probe,
                         sizeof(probe), &read) ||
      read != sizeof(probe))
  {
    return false;
  }

  return true;
}

// ---- public: read/write -----------------------------------------------------

bool WindowsFlycastProcess::readFromRAM(u32 offset, char* buffer, size_t size, bool withBSwap)
{
  if (!m_hProcess || m_emuRAMAddressStart == 0 || !buffer || size == 0)
    return false;

  SIZE_T bytesRead = 0;
  uint64_t addr = m_emuRAMAddressStart + static_cast<uint64_t>(offset);

  if (!ReadProcessMemory(m_hProcess, reinterpret_cast<LPCVOID>(addr), buffer, size, &bytesRead) ||
      bytesRead != size)
  {
    int err = GetLastError();
    return false;
  }

  if (withBSwap)
  {
    // Mirror DME behavior (swap by element size: 1/2/4/8). If your caller passes mixed sizes,
    // this is a no-op unless size is exactly one of these. Adjust if you need per-element swap.
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

bool WindowsFlycastProcess::writeToRAM(u32 offset, const char* buffer, size_t size, bool withBSwap)
{
  if (!m_hProcess || m_emuRAMAddressStart == 0 || !buffer || size == 0)
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

  SIZE_T bytesWritten = 0;
  uint64_t addr = m_emuRAMAddressStart + static_cast<uint64_t>(offset);

  if (!WriteProcessMemory(m_hProcess, reinterpret_cast<LPVOID>(addr), src, size, &bytesWritten) ||
      bytesWritten != size)
  {
    return false;
  }
  return true;
}

}  // namespace FlycastComm
#endif
