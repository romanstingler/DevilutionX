/**
 * @file utils/file_lock.hpp
 *
 * Sidecar-file based exclusive locking for save files.
 *
 * Each save archive (packed) or save directory (unpacked) gets a sibling
 * marker file whose presence indicates that another instance is actively
 * using the save. The marker is created atomically with O_EXCL semantics
 * and persists across the MpqWriter rename-to-.tmp / recreate cycle
 * because it lives at a different path.
 *
 * The locking is optional: if acquisition fails the caller decides what
 * to do (skip at character select, soft-fail at session start, ...).
 */
#pragma once

#include <cstdint>
#include <string>

namespace devilution {

/**
 * @brief RAII handle for a sidecar lock file.
 *
 * Constructed empty. Becomes "held" via `TryAcquire(path)`; the destructor
 * removes the file. On unsupported platforms the type compiles down to an
 * empty handle that never holds.
 */
class FileLock {
public:
	FileLock() = default;
	~FileLock();

	FileLock(const FileLock &) = delete;
	FileLock &operator=(const FileLock &) = delete;

	FileLock(FileLock &&other) noexcept;
	FileLock &operator=(FileLock &&other) noexcept;

	/// @brief Atomically create the sidecar lock file at `path`. On any
	/// failure (already held, missing parent directory, permission
	/// denied, unsupported platform) the returned object is empty.
	static FileLock TryAcquire(const char *path);

	/// @brief Explicit early release. Safe to call when not held.
	void Release();

	/// @brief True when this handle currently owns the lock.
	bool IsHeld() const { return held_; }
	explicit operator bool() const { return held_; }

	/// @brief Diagnostic path of the lock file (empty when not held).
	const std::string &Path() const { return path_; }

private:
	explicit FileLock(std::string path)
	    : path_(std::move(path)), held_(true) {}

	std::string path_;
	bool held_ = false;
};

/**
 * @brief Returns true if `path` is locked by another process.
 *
 * On platforms without a portable liveness check the probe is purely
 * file-presence based; the function therefore also rejects the lock as
 * stale when the recorded PID looks malformed (zero / no digits).
 *
 * Never steals the lock — the caller decides whether to acquire it.
 */
bool IsPathLocked(const char *path);

/**
 * @brief Returns true if a process with the given PID is currently alive.
 *
 * On unsupported platforms this always returns false so a stale lock file
 * always looks reclaimable rather than eternally occupied.
 */
bool IsProcessAlive(uint32_t pid);

} // namespace devilution
