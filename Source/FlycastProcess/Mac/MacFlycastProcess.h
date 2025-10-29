#ifdef __APPLE__
#pragma once

#include <vector>

#include <mach/mach.h>
#include "../IFlycastProcess.h"

namespace FlycastComm
{
class MacFlycastProcess : public IFlycastProcess
{
public:
  MacFlycastProcess() = default;

  bool findPID() override;
  bool obtainEmuRAMInformations() override;
  bool readFromRAM(u32 offset, char* buffer, size_t size, bool withBSwap) override;
  bool writeToRAM(u32 offset, const char* buffer, size_t size, bool withBSwap) override;

private:
  task_t m_task = MACH_PORT_NULL;
  task_t m_currentTask = MACH_PORT_NULL;

  struct Region
  {
    uint64_t base, size;
  };
  bool enumerateRegions(std::vector<Region>& out);
  bool addressInRegions(uint64_t addr, const std::vector<Region>& regs) const;
  bool triangulateArenaBase(const std::vector<Region>& regs, uint64_t& out_ram_base) const;
};
}  // namespace FlycastComm
#endif
