/* This file is part of pr-downloader (GPL v2 or later), see the LICENSE file */

#ifndef GIT_RAPID_BUILDER_H
#define GIT_RAPID_BUILDER_H

#include "GitRapidTypes.h"

#include <string>

class GitRapidBuilder
{
public:
	explicit GitRapidBuilder(int timeoutSeconds);

	bool Build(const GitRapidVersionInfo& version,
		   GitRapidBuildResult& resultOut, std::string& errorOut) const;

private:
	bool DownloadBlob(const GitRapidFileInfo& file, std::string& dataOut,
			  std::string& errorOut) const;

	int m_timeoutSeconds = 20;
};

#endif
