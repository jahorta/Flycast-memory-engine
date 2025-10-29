#ifdef __linux__
#pragma once

#include <string>
#include <vector>

#include "../IFlycastProcess.h"

namespace FlycastComm
{
class LinuxFlycastProcess : public IFlycastProcess
{
public:
  LinuxFlycastProcess() = default;

  bool findPID() override;
  bool obtainEmuRAMInformations() override;
  bool readFromRAM(u32 offset, char* buffer, size_t size, bool withBSwap) override;
  bool writeToRAM(u32 offset, const char* buffer, size_t size, bool withBSwap) override;

private:
  struct Region
  {
    uint64_t base = 0;
    uint64_t size = 0;
  };

  bool readProcMaps(std::vector<Region>& out_regions);
  bool addressInRegionList(uint64_t addr, const std::vector<Region>& regs) const;
  bool triangulateArenaBase(const std::vector<Region>& regs, uint64_t& out_ram_base) const;
};
}  // namespace FlycastComm
#endif
