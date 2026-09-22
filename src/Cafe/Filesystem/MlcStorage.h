#pragma once

#include <filesystem>

// The on-disk layout of the emulated NAND (mlc01). It is described here rather
// than in a front end because every front end needs the same directories to
// exist before a title can be mounted, and a second description of them would
// drift.
namespace MlcStorage
{
	// Creates the directories and default content files an mlc needs, and
	// proves the location is writable. Returns false when any of that fails;
	// the caller decides how to tell the user.
	bool CreateDefaultFiles(const std::filesystem::path& mlc);
} // namespace MlcStorage
