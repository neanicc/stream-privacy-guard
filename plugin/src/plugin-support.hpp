#pragma once

namespace spg::plugin {

// Name and version reported to OBS and prefixed to log lines.
inline constexpr const char *kPluginName = "obs-spg";
inline constexpr const char *kPluginVersion = SPG_PLUGIN_VERSION;

// blog() with an "[obs-spg] " prefix. Never pass watch-list text or any other
// recognized on-screen text: OBS logs are uploaded and shared for support.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void log(int level, const char *format, ...) noexcept;

}  // namespace spg::plugin
