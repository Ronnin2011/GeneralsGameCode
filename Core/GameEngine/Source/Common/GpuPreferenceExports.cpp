#include <windows.h>

// Ronin @feature 12/03/2026 Request high-performance GPU on hybrid systems.
// These exports are consumed by NVIDIA Optimus and AMD PowerXpress.
extern "C" {

__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

}
