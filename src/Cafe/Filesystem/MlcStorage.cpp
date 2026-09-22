#include "Cafe/Filesystem/MlcStorage.h"

#include "Cemu/ncrypto/ncrypto.h"

#include <boost/algorithm/string.hpp>

#include <fstream>

namespace fs = std::filesystem;

bool MlcStorage::CreateDefaultFiles(const fs::path& mlc)
{
	auto CreateDirectoriesIfNotExist = [](const fs::path& path)
	{
		std::error_code ec;
		if (!fs::exists(path, ec))
			return fs::create_directories(path, ec);
		return true;
	};
	// list of directories to create
	const fs::path directories[] = {
		mlc,
		mlc / "sys",
		mlc / "usr",
		mlc / "usr/title/00050000", // base
		mlc / "usr/title/0005000c", // dlc
		mlc / "usr/title/0005000e", // update
		mlc / "usr/save/00050010/1004a000/user/common/db", // Mii Maker save folders {0x500101004A000, 0x500101004A100, 0x500101004A200}
		mlc / "usr/save/00050010/1004a100/user/common/db",
		mlc / "usr/save/00050010/1004a200/user/common/db",
		mlc / "sys/title/0005001b/1005c000/content" // lang files
	};
	for(auto& path : directories)
	{
		if(!CreateDirectoriesIfNotExist(path))
			return false;
	}
	// create sys/usr folder in mlc01
	try
	{
		const auto langDir = fs::path(mlc).append("sys/title/0005001b/1005c000/content");
		auto langFile = fs::path(langDir).append("language.txt");
		if (!fs::exists(langFile))
		{
			std::ofstream file(langFile);
			if (file.is_open())
			{
				const char* langStrings[] = { "ja","en","fr","de","it","es","zh","ko","nl","pt","ru","zh" };
				for (const char* lang : langStrings)
					file << fmt::format(R"("{}",)", lang) << std::endl;

				file.flush();
				file.close();
			}
		}

		auto countryFile = fs::path(langDir).append("country.txt");
		if (!fs::exists(countryFile))
		{
			std::ofstream file(countryFile);
			for (sint32 i = 0; i < NCrypto::GetCountryCount(); i++)
			{
				const char* countryCode = NCrypto::GetCountryAsString(i);
				if (boost::iequals(countryCode, "NN"))
					file << "NULL," << std::endl;
				else
					file << fmt::format(R"("{}",)", countryCode) << std::endl;
			}
			file.flush();
			file.close();
		}
		// create a dummy file in the mlc folder to check if it's writable
		const auto dummyFile = fs::path(mlc).append("writetestdummy");
		std::ofstream file(dummyFile);
		if (!file.is_open())
			return false;
		file.close();
		fs::remove(dummyFile);
	}
	catch (const std::exception& ex)
	{
		return false;
	}
	return true;
}
