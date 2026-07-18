#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "utils/file_lock.hpp"
#include "utils/file_util.h"

using namespace devilution;

namespace {

std::string GetTmpPathName(const char *suffix = ".lck")
{
	const auto *current_test = ::testing::UnitTest::GetInstance()->current_test_info();
	std::string result = "Test_FileLock_";
	result.append(current_test->test_case_name());
	result += '_';
	result.append(current_test->name());
	result.append(suffix);
	return result;
}

TEST(FileLock, TryAcquireSucceedsOnFreshPath)
{
	const std::string path = GetTmpPathName();
	RemoveFile(path.c_str());

	FileLock lock = FileLock::TryAcquire(path.c_str());
	EXPECT_TRUE(lock.IsHeld());
	EXPECT_EQ(lock.Path(), path);
	lock.Release();
	EXPECT_FALSE(lock.IsHeld());
	EXPECT_FALSE(FileExists(path.c_str()));
}

TEST(FileLock, SecondAcquireFailsWhileHeld)
{
	const std::string path = GetTmpPathName();
	RemoveFile(path.c_str());

	FileLock holder = FileLock::TryAcquire(path.c_str());
	ASSERT_TRUE(holder.IsHeld());

	FileLock contender = FileLock::TryAcquire(path.c_str());
	EXPECT_FALSE(contender.IsHeld());

	holder.Release();

	FileLock reclaimer = FileLock::TryAcquire(path.c_str());
	EXPECT_TRUE(reclaimer.IsHeld());
	reclaimer.Release();
}

TEST(FileLock, MoveTransfersOwnership)
{
	const std::string path = GetTmpPathName();
	RemoveFile(path.c_str());

	FileLock src = FileLock::TryAcquire(path.c_str());
	ASSERT_TRUE(src.IsHeld());
	FileLock dst = std::move(src);
	EXPECT_FALSE(src.IsHeld());
	EXPECT_TRUE(dst.IsHeld());
	EXPECT_TRUE(FileExists(path.c_str()));

	dst.Release();
	EXPECT_FALSE(FileExists(path.c_str()));
}

TEST(FileLock, DestructorRemovesFile)
{
	const std::string path = GetTmpPathName();
	RemoveFile(path.c_str());

	{
		FileLock lock = FileLock::TryAcquire(path.c_str());
		ASSERT_TRUE(lock.IsHeld());
		EXPECT_TRUE(FileExists(path.c_str()));
	}
	EXPECT_FALSE(FileExists(path.c_str()));
}

TEST(FileLock, IsPathLockedFalseForMissingFile)
{
	const std::string path = GetTmpPathName();
	RemoveFile(path.c_str());

	EXPECT_FALSE(IsPathLocked(path.c_str()));
}

TEST(FileLock, IsPathLockedFalseForMalformedFile)
{
	const std::string path = GetTmpPathName();
	RemoveFile(path.c_str());

	// File exists but lacks a parseable PID — treat as not held.
	{
		std::ofstream out(path);
		ASSERT_TRUE(out.is_open());
		out << "no-pid-here";
	}
	EXPECT_FALSE(IsPathLocked(path.c_str()));
	RemoveFile(path.c_str());
}

TEST(FileLock, IsPathLockedTrueForLiveLock)
{
	const std::string path = GetTmpPathName();
	RemoveFile(path.c_str());

	FileLock holder = FileLock::TryAcquire(path.c_str());
	ASSERT_TRUE(holder.IsHeld());

	// Our own PID is written into the lock file, which is by definition
	// live for the duration of this test.
	EXPECT_TRUE(IsPathLocked(path.c_str()));

	holder.Release();
	EXPECT_FALSE(IsPathLocked(path.c_str()));
}

} // namespace
