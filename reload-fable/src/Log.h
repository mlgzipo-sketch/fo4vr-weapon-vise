// ManualReloadF: log helpers (printf-style lines in ManualReloadF.log)
#pragma once

namespace mrf
{
    // a line always written
    void note(const char* format, ...) noexcept;
    // a line written only with bDebugLog=1
    void detail(const char* format, ...) noexcept;
}
