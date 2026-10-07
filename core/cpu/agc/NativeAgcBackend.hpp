#pragma once
#include <cpu/SceAgcImports.hpp>
namespace AgcDriver::Metal { class MetalDriver; }
namespace Cpu {
// The session owns driver lifetime. Destroy gates/stop CPU before the driver.
SceAgcBackend MakeNativeAgcBackend(AgcDriver::Metal::MetalDriver& driver);
}
