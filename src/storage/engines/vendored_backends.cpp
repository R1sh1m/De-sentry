#include "desentry/storage/engines/vendored_backends.h"

#include <map>

namespace desentry {

// Each adapter defines its own factory inside its #ifdef. Declaring them
// here (rather than in the header) keeps the header free of symbols that may
// not exist in a given build configuration.
#ifdef DESENTRY_WITH_SQLITE
std::unique_ptr<EngineBackend> MakeSqliteBackend();
#endif
#ifdef DESENTRY_WITH_LMDB
std::unique_ptr<EngineBackend> MakeLmdbBackend();
#endif
#ifdef DESENTRY_WITH_DUCKDB
std::unique_ptr<EngineBackend> MakeDuckDbBackend();
#endif
#if defined(DESENTRY_WITH_SQLITE_VEC) && defined(DESENTRY_WITH_SQLITE)
std::unique_ptr<EngineBackend> MakeSqliteVecBackend();
#endif

namespace {

const std::map<std::string, std::string>& OptionMap() {
  static const std::map<std::string, std::string> kOptions = {
      {"sqlite", "DESENTRY_WITH_SQLITE"},
      {"lmdb", "DESENTRY_WITH_LMDB"},
      {"duckdb", "DESENTRY_WITH_DUCKDB"},
      {"sqlite_vec", "DESENTRY_WITH_SQLITE_VEC"},
  };
  return kOptions;
}

}  // namespace

bool VendoredBackendAvailable(const std::string& engine_name) {
#ifdef DESENTRY_WITH_SQLITE
  if (engine_name == "sqlite") return true;
#endif
#ifdef DESENTRY_WITH_LMDB
  if (engine_name == "lmdb") return true;
#endif
#ifdef DESENTRY_WITH_DUCKDB
  if (engine_name == "duckdb") return true;
#endif
#if defined(DESENTRY_WITH_SQLITE_VEC) && defined(DESENTRY_WITH_SQLITE)
  if (engine_name == "sqlite_vec") return true;
#endif
  (void)engine_name;
  return false;
}

std::string VendoredBackendOption(const std::string& engine_name) {
  auto it = OptionMap().find(engine_name);
  return it == OptionMap().end() ? std::string() : it->second;
}

StatusOr<std::unique_ptr<EngineBackend>> MakeVendoredBackend(const std::string& engine_name) {
#ifdef DESENTRY_WITH_SQLITE
  if (engine_name == "sqlite") return MakeSqliteBackend();
#endif
#ifdef DESENTRY_WITH_LMDB
  if (engine_name == "lmdb") return MakeLmdbBackend();
#endif
#ifdef DESENTRY_WITH_DUCKDB
  if (engine_name == "duckdb") return MakeDuckDbBackend();
#endif
#if defined(DESENTRY_WITH_SQLITE_VEC) && defined(DESENTRY_WITH_SQLITE)
  if (engine_name == "sqlite_vec") return MakeSqliteVecBackend();
#endif

  const std::string option = VendoredBackendOption(engine_name);
  if (option.empty()) return Status::InvalidArgument("unknown engine: " + engine_name);
  // An actionable message beats a silent fallback to a different storage
  // layout: the operator asked for a specific engine and should be told
  // exactly why they did not get it.
  return Status::InvalidArgument("engine '" + engine_name + "' is not compiled into this build; " +
                                  "rebuild with -D" + option +
                                  "=ON after vendoring its source into third_party/ "
                                  "(see third_party/README.md)");
}

}  // namespace desentry
