/**
 * @file pfile.h
 *
 * Interface of the save game encoding functionality.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <expected.hpp>

#include "DiabloUI/diabloui.h"
#include "player.h"
#include "utils/file_lock.hpp"

#ifdef UNPACKED_SAVES
#include "utils/file_util.h"
#else
#include "mpq/mpq_reader.hpp"
#include "mpq/mpq_writer.hpp"
#endif

namespace devilution {

#define MAX_CHARACTERS 99

extern bool gbValidSaveFile;

/**
 * @brief Session-scope exclusive lock on the active character's save file.
 *
 * Acquired after the active save has been read at session start. Released
 * when the player returns to the main menu. Existence of the sidecar file
 * tells concurrent processes to skip this slot in their enumeration and
 * refuse to start a session against it.
 */
extern std::optional<FileLock> gSaveFileLock;

/**
 * @brief Session-scope exclusive lock on the stash save file.
 *
 * Same lifecycle as `gSaveFileLock`. If acquisition fails we mark the
 * stash as disabled for the session instead of refusing to start.
 */
extern std::optional<FileLock> gStashFileLock;

#ifdef UNPACKED_SAVES
struct SaveReader {
	explicit SaveReader(std::string &&dir)
	    : dir_(std::move(dir))
	{
	}

	const std::string &dir() const
	{
		return dir_;
	}

	std::unique_ptr<std::byte[]> ReadFile(const char *filename, std::size_t &fileSize, int32_t &error);

	bool HasFile(const char *path)
	{
		return ::devilution::FileExists((dir_ + path).c_str());
	}

private:
	std::string dir_;
};

struct SaveWriter {
	explicit SaveWriter(std::string &&dir)
	    : dir_(std::move(dir))
	{
	}

	bool WriteFile(const char *filename, const std::byte *data, size_t size);

	bool HasFile(const char *path)
	{
		return ::devilution::FileExists((dir_ + path).c_str());
	}

	void RenameFile(const char *from, const char *to)
	{
		::devilution::RenameFile((dir_ + from).c_str(), (dir_ + to).c_str());
	}

	void RemoveHashEntry(const char *path)
	{
		RemoveFile((dir_ + path).c_str());
	}

	void RemoveHashEntries(bool (*fnGetName)(uint8_t, char *));

private:
	std::string dir_;
};

#else
using SaveReader = MpqArchive;
using SaveWriter = MpqWriter;
#endif

/**
 * @brief Comparison result of pfile_compare_hero_demo
 */
struct HeroCompareResult {
	enum Status : uint8_t {
		ReferenceNotFound,
		Same,
		Difference,
	};
	Status status;
	std::string message;
};

std::optional<SaveReader> OpenSaveArchive(uint32_t saveNum);
std::optional<SaveReader> OpenStashArchive();
const char *pfile_get_password();
std::unique_ptr<std::byte[]> ReadArchive(SaveReader &archive, const char *pszName, size_t *pdwLen = nullptr);
void pfile_write_hero(bool writeGameData = false);

/**
 * @brief Probe-only occupancy check for a character save slot.
 *
 * Returns true if a sidecar lock file is present for the slot AND the
 * owning process is currently alive. Does not steal stale locks.
 */
bool IsSaveSlotLocked(uint32_t saveNum);

/**
 * @brief Probe-only occupancy check for the stash save file.
 */
bool IsStashLocked();

/**
 * @brief Returns the path to the sidecar lock file for a character slot.
 *
 * Packed: `<save><ext>.lck`. Unpacked: `<saveDir>/.lock`.
 */
std::string GetSaveLockPath(uint32_t saveNum);

/**
 * @brief Returns the path to the sidecar lock file for the stash.
 */
std::string GetStashLockPath();

/**
 * @brief Attempt to acquire sidecar locks on the active save and stash.
 *
 * Character lock is mandatory; on failure shows a modal and returns
 * false. Stash lock is best-effort: on failure, the stash is marked
 * disabled for the session and a non-fatal modal is shown. The caller
 * should call `pfile_release_session_locks` on session end.
 *
 * Must be called after `pfile_read_player_from_save` so the slot index
 * is final.
 */
bool AcquireSessionSaveLocks();

/**
 * @brief Release any currently held session save / stash locks.
 */
void ReleaseSessionSaveLocks();

#ifndef DISABLE_DEMOMODE
/**
 * @brief Save a reference game-state (save game) for the demo recording
 * @param demo that is recorded
 */
void pfile_write_hero_demo(int demo);
/**
 * @brief Compares the actual game-state (savegame) with a reference game-state (save game from demo recording)
 * @param demo for the comparison
 * @param logDetails in case of a difference log details
 * @return The comparison result.
 */
HeroCompareResult pfile_compare_hero_demo(int demo, bool logDetails);
#endif

void sfile_write_stash();
bool pfile_ui_set_hero_infos(bool (*uiAddHeroInfo)(_uiheroinfo *));
void pfile_ui_set_class_stats(HeroClass playerClass, _uidefaultstats *classStats);
uint32_t pfile_ui_get_first_unused_save_num();
bool pfile_ui_save_create(_uiheroinfo *heroinfo);
bool pfile_delete_save(_uiheroinfo *heroInfo);
void pfile_read_player_from_save(uint32_t saveNum, Player &player);
void pfile_save_level();
tl::expected<void, std::string> pfile_convert_levels();
void pfile_remove_temp_files();
std::unique_ptr<std::byte[]> pfile_read(const char *pszName, size_t *pdwLen);
void pfile_update(bool forceSave);

} // namespace devilution
