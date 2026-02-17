/* This file is part of pr-downloader (GPL v2 or later), see the LICENSE file */

#ifndef GIT_RAPID_TYPES_H
#define GIT_RAPID_TYPES_H

#include <string>
#include <vector>

struct GitRapidFileInfo
{
	std::string path;
	std::string blobSha;
	std::string blobUrl;
	int size = -1;
};

struct GitRapidVersionInfo
{
	std::string tag;
	std::string commit;
	std::string displayName;
	std::vector<std::string> depends;
	std::vector<GitRapidFileInfo> files;
};

struct GitRapidBuildResult
{
	std::string sdpMd5;
};

#endif
