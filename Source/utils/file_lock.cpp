/**
 * @file utils/file_lock.cpp
 *
 * Cross-platform sidecar lock file implementation.
 *
 * On Windows we use `CreateFileA` with `CREATE_NEW` (the Win32 analogue
 * of POSIX O_EXCL) so the create+open is atomic. On POSIX systems we
 * use `open(..., O_CREAT | O_EXCL | O_WRONLY, 0644)`.
 *
 * On Emscripten and other platforms without a reliable cross-process
 * filesystem primitive the type is a no-op: `TryAcquire` always
 * returns an empty handle and `IsPathLocked` always returns false. The
 * existing build matrix keeps working unchanged on those platforms.
 */
#include "utils/file_lock.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>

#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX 1
#include <windows.h>
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(__DJGPP__)
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace devilution {

namespace {

// Format of the lock file body. A future reader may inspect it for
// diagnostics but the existence of the file alone is what gates access.
constexpr char kLockFileFormat[] = "devilutionx-save-lock pid=%u host=%s\n";

bool WriteLockFileBody(const char *path)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		return false;
	}
	char host[256] = "unknown";
#if defined(_WIN32)
	DWORD size = sizeof(host);
	if (::GetComputerNameA(host, &size) == 0) {
		std::strcpy(host, "unknown");
	}
#elif !defined(__EMSCRIPTEN__) && !defined(__DJGPP__)
	if (::gethostname(host, sizeof(host) - 1) != 0) {
		std::strcpy(host, "unknown");
	}
	host[sizeof(host) - 1] = '\0';
#endif
	const uint32_t pid = GetLockFilePid();
	char body[320];
	const int written = std::snprintf(body, sizeof(body),
	    kLockFileFormat,
	    static_cast<unsigned>(pid), host);
	if (written <= 0 || static_cast<size_t>(written) >= sizeof(body)) {
		return false;
	}
	out.write(body, written);
	out.flush();
	return out.good();
}

// Returns the PID written in `path`, or 0 if the file does not contain
// a recognizable lock header.
uint32_t ReadLockFilePid(const char *path)
{
	std::ifstream in(path);
	if (!in) {
		return 0;
	}
	char line[320] = {};
	in.getline(line, sizeof(line));
	if (in.bad()) {
		return 0;
	}
	const char *prefix = "pid=";
	const char *match = std::strstr(line, prefix);
	if (match == nullptr) {
		return 0;
	}
	match += std::strlen(prefix);
	uint32_t pid = 0;
	for (; *match != '\0' && *match != ' '; ++match) {
		if (*match < '0' || *match > '9') {
			return 0;
		}
		pid = pid * 10u + static_cast<uint32_t>(*match - '0');
	}
	return pid;
}

} // namespace

uint32_t GetLockFilePid()
{
#if defined(_WIN32)
	return static_cast<uint32_t>(::GetCurrentProcessId());
#elif !defined(__EMSCRIPTEN__) && !defined(__DJGPP__)
	return static_cast<uint32_t>(::getpid());
#else
	return 0;
#endif
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(__DJGPP__)
bool IsProcessAlive(uint32_t pid)
{
	if (pid == 0) {
		return false;
	}
	// `kill(pid, 0)` returns 0 if the process exists and we may signal it,
	// -1 with EPERM if it exists but is in another session, -1 with ESRCH
	// if it does not exist.
	if (::kill(static_cast<pid_t>(pid), 0) == 0) {
		return true;
	}
	return errno == EPERM;
}
#elif defined(_WIN32)
bool IsProcessAlive(uint32_t pid)
{
	if (pid == 0) {
		return false;
	}
	// PROCESS_QUERY_LIMITED_INFORMATION is the least-privileged access
	// mask that still lets us check whether the process is still running.
	HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (handle == nullptr) {
		// E.g. ACCESS_DENIED on protected processes. Treat the existence
		// of an openable handle as authoritative for "alive"; otherwise
		// assume "alive" so we never accidentally steal a lock from a
		// running-but-protected process.
		const DWORD err = ::GetLastError();
		if (err == ERROR_ACCESS_DENIED || err == ERROR_INVALID_PARAMETER) {
			return err == ERROR_INVALID_PARAMETER ? false : true;
		}
		return false;
	}
	const DWORD wait = ::WaitForSingleObject(handle, 0);
	::CloseHandle(handle);
	return wait == WAIT_TIMEOUT;
}
#else
bool IsProcessAlive(uint32_t /*pid*/)
{
	return false;
}
#endif

FileLock::FileLock(FileLock &&other) noexcept
    : path_(std::move(other.path_)), held_(other.held_)
{
	other.held_ = false;
	other.path_.clear();
}

FileLock &FileLock::operator=(FileLock &&other) noexcept
{
	if (this != &other) {
		Release();
		path_ = std::move(other.path_);
		held_ = other.held_;
		other.held_ = false;
		other.path_.clear();
	}
	return *this;
}

FileLock::~FileLock()
{
	Release();
}

void FileLock::Release()
{
	if (!held_) {
		return;
	}
	held_ = false;
	if (!path_.empty()) {
		RemoveFile(path_.c_str());
	}
	path_.clear();
}

#if defined(_WIN32)
namespace {
bool CreateSidecarExclusive(const char *path)
{
	HANDLE handle = ::CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
	    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}
	::CloseHandle(handle);
	return true;
}
} // namespace

FileLock FileLock::TryAcquire(const char *path)
{
	if (path == nullptr || path[0] == '\0') {
		return {};
	}
	// CreateFileA returns ERROR_FILE_EXISTS (80) if the sidecar is held.
	if (!CreateSidecarExclusive(path)) {
		return {};
	}
	if (!WriteLockFileBody(path)) {
		RemoveFile(path);
		return {};
	}
	return FileLock(std::string(path));
}
#elif !defined(__EMSCRIPTEN__) && !defined(__DJGPP__)
FileLock FileLock::TryAcquire(const char *path)
{
	if (path == nullptr || path[0] == '\0') {
		return {};
	}
	const int fd = ::open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
	if (fd < 0) {
		return {};
	}
	::close(fd);
	if (!WriteLockFileBody(path)) {
		::unlink(path);
		return {};
	}
	return FileLock(std::string(path));
}
#else
FileLock FileLock::TryAcquire(const char * /*path*/)
{
	return {};
}
#endif

bool IsPathLocked(const char *path)
{
	if (path == nullptr || path[0] == '\0') {
		return false;
	}
	if (!FileExists(path)) {
		return false;
	}
	const uint32_t pid = ReadLockFilePid(path);
	if (pid == 0) {
		// Unreadable or malformed lock file. Treat as not held so the
		// caller can simply create a fresh one.
		return false;
	}
	if (GetLockFilePid() != 0 && pid == GetLockFilePid()) {
		// Self-lock — consider it held to avoid the same instance
		// acquiring a second copy and deadlocking on close.
		return true;
	}
	return IsProcessAlive(pid);
}

} // namespace devilution
