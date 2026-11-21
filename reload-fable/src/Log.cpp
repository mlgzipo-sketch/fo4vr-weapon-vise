#include "Log.h"

#include "Runtime.h"

#include <cstdarg>
#include <cstdio>

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;

    void note(const char* format, ...) noexcept
    {
        char text[512]{};
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        logNote(text);
    }

    void detail(const char* format, ...) noexcept
    {
        char text[512]{};
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        logInfo(text);
    }
}
