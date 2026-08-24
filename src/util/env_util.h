#pragma once

#include <string>
#include <string_view>

#include "kvlog/env.h"
#include "kvlog/status.h"

namespace kvlog {

// Writes `data` to a new file `fname`, syncing it before closing if `sync`.
// On failure the partially written file is removed.
Status WriteStringToFile(Env* env, std::string_view data, const std::string& fname, bool sync);

// Reads the whole of `fname` into *data.
Status ReadFileToString(Env* env, const std::string& fname, std::string* data);

// Copies `src` to a new file `dst` (replacing any existing one) and syncs the
// copy before closing it. On failure the partial copy is removed.
Status CopyFileSynced(Env* env, const std::string& src, const std::string& dst);

}  // namespace kvlog
