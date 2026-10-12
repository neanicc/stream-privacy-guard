#include "plugin-support.hpp"

#include <util/base.h>

#include <cstdarg>
#include <cstdio>

namespace spg::plugin {

void log(int level, const char *format, ...) noexcept {
  // Prefix the format string itself so one blogva call keeps the line atomic.
  char prefixed[512];
  const int written = std::snprintf(prefixed, sizeof(prefixed), "[%s] %s", kPluginName, format);
  if (written < 0 || static_cast<std::size_t>(written) >= sizeof(prefixed)) {
    return;  // a format this long is a programming error; drop it rather than truncate mid-specifier
  }
  va_list args;
  va_start(args, format);
  blogva(level, prefixed, args);
  va_end(args);
}

}  // namespace spg::plugin
