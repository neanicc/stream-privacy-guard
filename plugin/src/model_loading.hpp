#pragma once

// The module's detection models (SPG-14b): one ModelHub per loaded module,
// shared by every Privacy Guard filter. Searched, in order: the models
// directory in the plugin's data directory (a packaged build), then
// $SPG_CACHE_DIR/models or ~/.cache/spg/models (scripts/fetch_models.py).
// Loading runs on the hub's thread and logs one line when it ends.

#include "models/model_hub.hpp"

namespace spg::plugin {

[[nodiscard]] ModelHub& module_models() noexcept;

}  // namespace spg::plugin
