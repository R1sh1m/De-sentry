#include <algorithm>
#include <fstream>
#include <sstream>

#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/supervisor/supervisor.h"

#if defined(_WIN32)
  // GetLogicalDriveStringsA / GetDriveTypeA / GetDiskFreeSpaceExA come from
  // windows.h, already pulled in by platform.h.
#elif defined(__APPLE__)
  #include <sys/mount.h>
  #include <sys/param.h>
  #include <sys/statvfs.h>
#else
  #include <sys/statvfs.h>
#endif

namespace desentry {

namespace {

// Directory names never worth descending into. Scanning them is slow, finds
// nothing, and on macOS/Linux can trip permission prompts.
bool IsNoiseDirectory(const std::string& name) {
  static const char* kSkip[] = {".git",         "node_modules", ".cache",  "Library",
                                "AppData",      "Windows",      "Program Files",
                                "Program Files (x86)",          "System Volume Information",
                                "$RECYCLE.BIN", ".Trash",       "proc",    "sys",
                                "dev",          "snap"};
  for (const char* skip : kSkip) {
    if (name == skip) return true;
  }
  return false;
}

std::string JoinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  const char last = a.back();
  if (last == '/' || last == '\\') return a + b;
  return a + "/" + b;
}

void QueryFreeSpace(const std::string& path, uint64_t* total, uint64_t* free_bytes) {
  *total = 0;
  *free_bytes = 0;
#if defined(_WIN32)
  ULARGE_INTEGER available{}, capacity{}, remaining{};
  if (::GetDiskFreeSpaceExA(path.c_str(), &available, &capacity, &remaining)) {
    *total = static_cast<uint64_t>(capacity.QuadPart);
    *free_bytes = static_cast<uint64_t>(available.QuadPart);
  }
#else
  struct statvfs stats;
  if (::statvfs(path.c_str(), &stats) == 0) {
    *total = static_cast<uint64_t>(stats.f_blocks) * stats.f_frsize;
    *free_bytes = static_cast<uint64_t>(stats.f_bavail) * stats.f_frsize;
  }
#endif
}

// Recursively sums file sizes, bounded so a scan of a huge tree cannot hang
// the housekeeping pass.
uint64_t DirectorySize(const std::string& path, size_t depth = 0) {
  if (depth > 4) return 0;
  uint64_t total = 0;
  for (const std::string& name : ListDir(path)) {
    const std::string child = JoinPath(path, name);
    if (IsDirectory(child)) {
      total += DirectorySize(child, depth + 1);
    } else {
      total += FileSize(child);
    }
  }
  return total;
}

std::string ReadTextFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return std::string();
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

JsonValue DataDirCandidate::ToJson() const {
  JsonValue::Object o;
  o.emplace_back("path", JsonValue(path));
  o.emplace_back("has_node_config", JsonValue(has_node_config));
  o.emplace_back("has_identity", JsonValue(has_identity));
  o.emplace_back("has_data_file", JsonValue(has_data_file));
  o.emplace_back("has_manifest", JsonValue(has_manifest));
  o.emplace_back("removable", JsonValue(is_removable));
  o.emplace_back("encrypted", JsonValue(encrypted));
  o.emplace_back("node_name", JsonValue(node_name));
  o.emplace_back("node_id", JsonValue(node_id));
  o.emplace_back("free_bytes", JsonValue(static_cast<int64_t>(free_bytes)));
  o.emplace_back("used_bytes", JsonValue(static_cast<int64_t>(used_bytes)));
  o.emplace_back("existing_node", JsonValue(LooksLikeExistingNode()));
  o.emplace_back("adoptable", JsonValue(LooksAdoptable()));
  return JsonValue(std::move(o));
}

JsonValue MountPoint::ToJson() const {
  JsonValue::Object o;
  o.emplace_back("path", JsonValue(path));
  o.emplace_back("label", JsonValue(label));
  o.emplace_back("removable", JsonValue(removable));
  o.emplace_back("total_bytes", JsonValue(static_cast<int64_t>(total_bytes)));
  o.emplace_back("free_bytes", JsonValue(static_cast<int64_t>(free_bytes)));
  return JsonValue(std::move(o));
}

std::vector<MountPoint> HardwareScanner::ListMounts() {
  std::vector<MountPoint> mounts;
#if defined(_WIN32)
  char buffer[512] = {0};
  const DWORD len = ::GetLogicalDriveStringsA(sizeof(buffer) - 1, buffer);
  for (DWORD i = 0; i < len;) {
    const std::string root = &buffer[i];
    i += static_cast<DWORD>(root.size()) + 1;
    if (root.empty()) continue;
    const UINT type = ::GetDriveTypeA(root.c_str());
    if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_CDROM) continue;
    MountPoint mount;
    mount.path = root;
    mount.removable = (type == DRIVE_REMOVABLE);
    char label[MAX_PATH + 1] = {0};
    if (::GetVolumeInformationA(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) {
      mount.label = label;
    }
    if (mount.label.empty()) mount.label = root;
    QueryFreeSpace(root, &mount.total_bytes, &mount.free_bytes);
    mounts.push_back(std::move(mount));
  }
#elif defined(__APPLE__)
  struct statfs* filesystems = nullptr;
  const int count = ::getmntinfo(&filesystems, MNT_NOWAIT);
  for (int i = 0; i < count; ++i) {
    MountPoint mount;
    mount.path = filesystems[i].f_mntonname;
    mount.label = mount.path;
    // /Volumes is where macOS mounts everything that is not the boot disk,
    // which is the closest honest proxy for "removable" without asking
    // DiskArbitration.
    mount.removable = mount.path.rfind("/Volumes/", 0) == 0;
    QueryFreeSpace(mount.path, &mount.total_bytes, &mount.free_bytes);
    mounts.push_back(std::move(mount));
  }
#else
  // /proc/mounts is the authoritative list on Linux. Only real, writable
  // filesystems are offered; pseudo-filesystems would just be noise in the
  // app's picker.
  std::ifstream f("/proc/mounts");
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream fields(line);
    std::string device, mount_path, fstype;
    if (!(fields >> device >> mount_path >> fstype)) continue;
    static const char* kRealFs[] = {"ext2", "ext3", "ext4", "btrfs", "xfs",   "f2fs",
                                    "vfat", "exfat", "ntfs", "ntfs3", "zfs",  "apfs"};
    bool real = false;
    for (const char* candidate : kRealFs) {
      if (fstype == candidate) {
        real = true;
        break;
      }
    }
    if (!real) continue;
    MountPoint mount;
    mount.path = mount_path;
    mount.label = mount_path;
    mount.removable = mount_path.rfind("/media/", 0) == 0 || mount_path.rfind("/run/media/", 0) == 0 ||
                      mount_path.rfind("/mnt/", 0) == 0;
    QueryFreeSpace(mount.path, &mount.total_bytes, &mount.free_bytes);
    mounts.push_back(std::move(mount));
  }
#endif
  std::sort(mounts.begin(), mounts.end(),
            [](const MountPoint& a, const MountPoint& b) { return a.path < b.path; });
  return mounts;
}

StatusOr<JsonValue> HardwareScanner::ReadManifest(const std::string& dir) {
  const std::string text = ReadTextFile(JoinPath(dir, "manifest.json"));
  if (text.empty()) return Status::NotFound("no manifest.json in " + dir);
  try {
    return JsonValue::Parse(text);
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("manifest.json parse error: ") + e.what());
  }
}

Status HardwareScanner::WriteManifest(const std::string& dir, const JsonValue& manifest) {
  if (!MakeDirs(dir)) return Status::IOError("cannot create directory: " + dir);
  const std::string path = JoinPath(dir, "manifest.json");
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f.is_open()) return Status::IOError("cannot write " + tmp);
    f << manifest.Dump();
    f.flush();
    if (!f.good()) return Status::IOError("manifest write failed");
  }
  std::remove(path.c_str());
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return Status::IOError("cannot commit " + path);
  }
  return Status::OK();
}

DataDirCandidate HardwareScanner::Inspect(const std::string& path) {
  DataDirCandidate candidate;
  candidate.path = path;
  if (!IsDirectory(path)) return candidate;

  candidate.has_node_config = PathExists(JoinPath(path, "node.json"));
  candidate.has_identity = PathExists(JoinPath(path, "identity.key"));
  candidate.has_data_file =
      PathExists(JoinPath(path, "desentry.dsf")) || IsDirectory(JoinPath(path, "engines"));
  candidate.has_manifest = PathExists(JoinPath(path, "manifest.json"));

  uint64_t total = 0;
  QueryFreeSpace(path, &total, &candidate.free_bytes);
  if (candidate.LooksLikeExistingNode()) candidate.used_bytes = DirectorySize(path);

  if (candidate.has_node_config) {
    try {
      const JsonValue config = JsonValue::Parse(ReadTextFile(JoinPath(path, "node.json")));
      const JsonValue* name = config.Find("node_name");
      if (name && name->is_string()) candidate.node_name = name->AsString();
      const JsonValue* encrypted = config.Find("encrypt_at_rest");
      if (encrypted && encrypted->is_bool()) candidate.encrypted = encrypted->AsBool();
    } catch (const std::exception&) {
      // A malformed node.json still leaves a usable candidate -- the app can
      // offer to repair it. Failing the whole scan over one bad file would
      // hide every other node on the volume.
    }
  }
  if (candidate.has_manifest) {
    auto manifest = ReadManifest(path);
    if (manifest.ok()) {
      const JsonValue* node_id = manifest.value().Find("node_id");
      if (node_id && node_id->is_string()) candidate.node_id = node_id->AsString();
      const JsonValue* encrypted = manifest.value().Find("encrypted");
      if (encrypted && encrypted->is_bool()) candidate.encrypted = encrypted->AsBool();
      if (candidate.node_name.empty()) {
        const JsonValue* name = manifest.value().Find("node_name");
        if (name && name->is_string()) candidate.node_name = name->AsString();
      }
    }
  }
  return candidate;
}

std::vector<DataDirCandidate> HardwareScanner::ScanRoots(const std::vector<std::string>& roots,
                                                          size_t max_depth) {
  std::vector<DataDirCandidate> found;
  std::vector<MountPoint> mounts = ListMounts();

  auto is_removable_path = [&mounts](const std::string& path) {
    for (const MountPoint& mount : mounts) {
      if (mount.removable && path.rfind(mount.path, 0) == 0) return true;
    }
    return false;
  };

  // Iterative BFS rather than recursion: the depth bound is then a property
  // of the queue and cannot be defeated by a symlink loop.
  std::vector<std::pair<std::string, size_t>> queue;
  for (const std::string& root : roots) {
    if (IsDirectory(root)) queue.emplace_back(root, 0);
  }

  for (size_t i = 0; i < queue.size() && found.size() < kMaxCandidates; ++i) {
    const std::string dir = queue[i].first;
    const size_t depth = queue[i].second;

    DataDirCandidate candidate = Inspect(dir);
    if (candidate.LooksLikeExistingNode()) {
      candidate.is_removable = is_removable_path(dir);
      found.push_back(std::move(candidate));
      // An existing node's directory is a leaf: its subdirectories are its
      // own engine data, not more nodes.
      continue;
    }
    if (depth >= max_depth) continue;
    for (const std::string& name : ListDir(dir)) {
      if (name.empty() || name[0] == '.') continue;
      if (IsNoiseDirectory(name)) continue;
      const std::string child = JoinPath(dir, name);
      if (IsDirectory(child)) queue.emplace_back(child, depth + 1);
    }
  }
  return found;
}

std::vector<DataDirCandidate> HardwareScanner::ScanDefaults() {
  std::vector<std::string> roots;
  roots.push_back(AppDataDir());
#if defined(_WIN32)
  if (const char* home = std::getenv("USERPROFILE")) roots.push_back(home);
#else
  if (const char* home = std::getenv("HOME")) roots.push_back(home);
#endif
  for (const MountPoint& mount : ListMounts()) {
    // Only removable volumes are scanned wholesale. Walking every fixed disk
    // would be slow and is not what the user asked for -- fixed-disk nodes
    // are found under the app data directory or picked by hand.
    if (mount.removable) roots.push_back(mount.path);
  }
  return ScanRoots(roots);
}

}  // namespace desentry
