#include "FlycastAccessor.h"

#ifdef __linux__
#include "Linux/LinuxFlycastProcess.h"
#elif _WIN32
#include "Windows/WindowsFlycastProcess.h"
#elif __APPLE__
#include "Mac/MacFlycastProcess.h"
#endif

#include <cstring>
#include <memory>

#include "../Common/CommonUtils.h"
#include "../Common/MemoryCommon.h"

namespace FlycastComm
{

// --- statics -----------------------------------------------------------------
IFlycastProcess* FlycastAccessor::m_instance = nullptr;
FlycastAccessor::FlycastStatus FlycastAccessor::m_status =
    FlycastAccessor::FlycastStatus::notRunning;

// --- helpers -----------------------------------------------------------------
void FlycastAccessor::init()
{
#ifdef __linux__
  m_instance = new LinuxFlycastProcess();
  return nullptr;
#elif _WIN32
  m_instance = new WindowsFlycastProcess();
#elif __APPLE__
  m_instance = new MacFlycastProcess();
  return nullptr;
#endif
}

void FlycastAccessor::free()
{
  delete m_instance;
}

// --- public API --------------------------------------------------------------

void FlycastAccessor::hook()
{
  init();
  if (!m_instance->findPID())
  {
    m_status = FlycastStatus::notRunning;
  }
  else if (!m_instance->obtainEmuRAMInformations())
  {
    m_status = FlycastStatus::noEmu;
  }
  else
  {
    m_status = FlycastStatus::hooked;
  }
}

void FlycastAccessor::unHook()
{
  delete m_instance;
  m_instance = nullptr;
  m_status = FlycastStatus::notRunning;
}

bool FlycastAccessor::readFromRAM(u32 offset, char* buffer, size_t size, bool withBSwap)
{
  if (!m_instance || m_status != FlycastStatus::hooked)
    return false;
  return m_instance->readFromRAM(offset, buffer, size, withBSwap);
}

bool FlycastAccessor::writeToRAM(u32 offset, const char* buffer, size_t size, bool withBSwap)
{
  if (!m_instance || m_status != FlycastStatus::hooked)
    return false;
  return m_instance->writeToRAM(offset, buffer, size, withBSwap);
}

Common::MemOperationReturnCode FlycastAccessor::readEntireRAM(char* buffer)
{
  if (!m_instance || m_status != FlycastStatus::hooked)
  {
    return Common::MemOperationReturnCode::operationFailed;
  }

  const size_t ramSize = getRAMTotalSize();
  if (!FlycastComm::FlycastAccessor::readFromRAM(0, buffer, ramSize, false))
  {
    return Common::MemOperationReturnCode::operationFailed;
  }
  return Common::MemOperationReturnCode::OK;
}

std::string FlycastAccessor::getFormattedValueFromMemory(u32 ramIndex, Common::MemType memType,
                                                         size_t memSize, Common::MemBase memBase,
                                                         bool memIsUnsigned)
{
  std::unique_ptr<char[]> buffer(new char[memSize]);
  readFromRAM(ramIndex, buffer.get(), memSize, false);
  return Common::formatMemoryToString(buffer.get(), memType, memSize, memBase, memIsUnsigned,
                                      Common::shouldBeBSwappedForType(memType));
}

int FlycastAccessor::getPID()
{
  return m_instance ? m_instance->getPID() : -1;
}

u64 FlycastAccessor::getEmuRAMAddressStart()
{
  return m_instance ? m_instance->getEmuRAMAddressStart() : 0;
}

FlycastAccessor::FlycastStatus FlycastAccessor::getStatus()
{
  return m_status;
}

bool FlycastAccessor::isARAMAccessible()
{
  return (m_instance && m_instance->isARAMAccessible());
}

u64 FlycastAccessor::getRAMAddressStart()
{
  return m_instance ? m_instance->getEmuRAMAddressStart() : 0;
}

u64 FlycastAccessor::getARAMAddressStart()
{
  return m_instance ? m_instance->getARAMAddressStart() : 0;
}

// If you want this dynamic, you can derive it from the discovered committed region
// that contains getRAMAddressStart(); for now we return the canonical Dreamcast size.
size_t FlycastAccessor::getRAMTotalSize()
{
  // Dreamcast main RAM is 16 MiB
  return 16 * 1024 * 1024;
}

bool FlycastAccessor::isValidConsoleAddress(u32 consoleAddress)
{
  const size_t ramSize = getRAMTotalSize();
  return consoleAddress < static_cast<u32>(ramSize);
}

}  // namespace FlycastComm
