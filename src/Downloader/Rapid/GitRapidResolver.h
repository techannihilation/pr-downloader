/* This file is part of pr-downloader (GPL v2 or later), see the LICENSE file */

#ifndef GIT_RAPID_RESOLVER_H
#define GIT_RAPID_RESOLVER_H

#include "GitRapidTypes.h"

#include <string>

class GitRapidResolver
{
public:
	explicit GitRapidResolver(int timeoutSeconds);

	bool Resolve(const std::string& manifestUrl, int manifestTtlSeconds,
		     const std::string& tag, GitRapidVersionInfo& versionOut,
		     std::string& errorOut) const;

private:
	bool LoadManifest(const std::string& manifestUrl, int manifestTtlSeconds,
			  std::string& manifestJson, std::string& errorOut) const;
	bool DownloadText(const std::string& url, std::string& output,
			  std::string& errorOut) const;
	bool ParseTag(const std::string& manifestJson, const std::string& tag,
		      GitRapidVersionInfo& versionOut, std::string& errorOut) const;

	int m_timeoutSeconds = 20;
};

#endif
