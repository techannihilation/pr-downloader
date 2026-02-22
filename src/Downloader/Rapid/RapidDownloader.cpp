/* This file is part of pr-downloader (GPL v2 or later), see the LICENSE file */

#include "RapidDownloader.h"
#include "GitRapidBuilder.h"
#include "GitRapidResolver.h"
#include "FileSystem/FileSystem.h"
#include "Util.h"
#include "Logger.h"
#include "Repo.h"
#include "Sdp.h"

#include <stdio.h>
#include <cstdlib>
#include <string>
#include <string.h>
#include <list>
#include <vector>
#include <zlib.h>
#include <algorithm> //std::min
#include <set>

#ifndef _WIN32
#include <regex.h>
#endif
#undef min
#undef max

namespace {

static bool ParseBool(const std::string& value)
{
	if (value == "1" || value == "true" || value == "yes" || value == "on") {
		return true;
	}
	if (value == "0" || value == "false" || value == "no" || value == "off") {
		return false;
	}
	return !value.empty();
}

static int ParseInt(const std::string& value, int fallback)
{
	char* end = nullptr;
	const long parsed = std::strtol(value.c_str(), &end, 10);
	if (end == nullptr || *end != '\0') {
		return fallback;
	}
	if (parsed <= 0) {
		return fallback;
	}
	return static_cast<int>(parsed);
}

static std::string JoinDepends(const std::vector<std::string>& depends)
{
	std::string out;
	for (size_t i = 0; i < depends.size(); ++i) {
		if (i > 0) {
			out.push_back('|');
		}
		out += depends[i];
	}
	return out;
}

} // namespace

CRapidDownloader::CRapidDownloader()
    : reposgzurl(REPO_MASTER)
{
}

void CRapidDownloader::addRemoteSdp(CSdp&& sdp)
{
	sdps.push_back(std::move(sdp));
}

bool CRapidDownloader::list_compare(const CSdp& first, const CSdp& second)
{
	const std::string name1 = first.getShortName();
	const std::string name2 = second.getShortName();
	const unsigned len = std::min(name1.size(), name2.size());
	for (unsigned i = 0; i < len; i++) {
		if (tolower(name1[i]) < tolower(name2[i])) {
			return true;
		}
	}
	return false;
}

bool CRapidDownloader::download_name(IDownload* download, int reccounter,
				     std::string name)
{
	LOG_DEBUG("%s %s", name.c_str(), download->name.c_str());
	if (reccounter > 10)
		return false;
	LOG_DEBUG("Using rapid to download %s", download->name.c_str());
	std::set<std::string> downloaded;
	bool matched = false;

	for (CSdp& sdp : sdps) {
		if (!match_download_name(sdp.getName(),
					 name.empty() ? download->name : name)) {
			continue;
		}
		matched = true;

		// already downloaded, skip (i.e. stable entries are // twice in versions.gz)
		if (downloaded.find(sdp.getMD5()) != downloaded.end()) {
			continue;
		}
		downloaded.insert(sdp.getMD5());

		LOG_INFO ("[Download] %s", sdp.getName().c_str());

		if (!sdp.download(download)) {
			return false;
		}
		if (sdp.getDepends().empty()) {
			continue;
		}
		if (!download_name(download, reccounter + 1, sdp.getDepends())) {
			return false;
		}
	}
	return matched;
}

bool CRapidDownloader::IsGitCandidate(const std::string& name,
				      DownloadEnum::Category cat) const
{
	if (!rapidGitEnabled || rapidGitManifestUrl.empty()) {
		return false;
	}
	if (!(cat == DownloadEnum::CAT_GAME || cat == DownloadEnum::CAT_COUNT ||
	      cat == DownloadEnum::CAT_NONE)) {
		return false;
	}
	if (name.empty() || name == "*") {
		return false;
	}
	return name.find(':') != std::string::npos;
}

bool CRapidDownloader::TryResolveViaGit(const std::string& tag)
{
	if (!IsGitCandidate(tag, DownloadEnum::CAT_GAME)) {
		return false;
	}

	if (rapidGitResolvedTags.find(tag) != rapidGitResolvedTags.end()) {
		return true;
	}

	GitRapidResolver resolver(rapidGitApiTimeoutSeconds);
	GitRapidVersionInfo version;
	std::string error;
	if (!resolver.Resolve(rapidGitManifestUrl, rapidGitManifestTtlSeconds, tag,
			      version, error)) {
		LOG_INFO("Git rapid resolve failed for '%s': %s", tag.c_str(),
			 error.c_str());
		return false;
	}

	GitRapidBuilder builder(rapidGitApiTimeoutSeconds);
	GitRapidBuildResult buildResult;
	if (!builder.Build(version, buildResult, error)) {
		LOG_WARN("Git rapid build failed for '%s': %s", tag.c_str(),
			 error.c_str());
		return false;
	}

	const std::string displayName =
	    version.displayName.empty() ? tag : version.displayName;
	for (auto it = sdps.begin(); it != sdps.end();) {
		if (it->getShortName() == tag) {
			it = sdps.erase(it);
			continue;
		}
		++it;
	}
	addRemoteSdp(
	    CSdp { tag, buildResult.sdpMd5, displayName, JoinDepends(version.depends), "" });
	rapidGitResolvedTags.insert(tag);

	LOG_INFO("Git rapid resolved %s -> %s.sdp", tag.c_str(),
		 buildResult.sdpMd5.c_str());
	return true;
}

void CRapidDownloader::CollectSearchResults(std::list<IDownload*>& result,
					    const std::string& name,
					    DownloadEnum::Category cat) const
{
	std::set<std::string> added;
	for (const CSdp& sdp : sdps) {
		if (!match_download_name(sdp.getShortName(), name) &&
		    !match_download_name(sdp.getName(), name)) {
			continue;
		}

		const std::string key = sdp.getShortName();
		if (added.find(key) != added.end()) {
			continue;
		}
		added.insert(key);

		IDownload* dl =
		    new IDownload(sdp.getName().c_str(), name, cat, IDownload::TYP_RAPID);
		dl->addMirror(sdp.getShortName().c_str());
		result.push_back(dl);
	}
}

bool CRapidDownloader::search(std::list<IDownload*>& result,
			      const std::string& name,
			      DownloadEnum::Category cat)
{
	LOG_DEBUG("%s", name.c_str());
	bool usedGit = false;
	if (IsGitCandidate(name, cat)) {
		usedGit = TryResolveViaGit(name);
	}
	if (!usedGit) {
		updateRepos(name);
	}
	sdps.sort(list_compare);
	CollectSearchResults(result, name, cat);

	if (result.empty() && usedGit) {
		LOG_INFO("Git rapid yielded no matches for '%s', falling back to repos.gz",
			 name.c_str());
		if (updateRepos(name)) {
			sdps.sort(list_compare);
			CollectSearchResults(result, name, cat);
		}
	}

	return true;
}

bool CRapidDownloader::download(IDownload* download, int /*max_parallel*/)
{
	LOG_DEBUG("%s", download->name.c_str());
	if (download->dltype != IDownload::TYP_RAPID) { // skip non-rapid downloads
		LOG_DEBUG("skipping non rapid-dl");
		return true;
	}

	bool triedGit = false;
	if (IsGitCandidate(download->origin_name, download->cat)) {
		triedGit = true;
		if (TryResolveViaGit(download->origin_name) && download_name(download, 0)) {
			return true;
		}
		LOG_INFO("Git rapid download fallback to repos.gz for '%s'",
			 download->origin_name.c_str());
	}

	if (!updateRepos(download->origin_name) && !triedGit) {
		return false;
	}
	return download_name(download, 0);
}

bool CRapidDownloader::match_download_name(const std::string& str1,
					   const std::string& str2)
{
	return str2 == "" || str1 == str2 || str2 == "*";
	// FIXME: add regex support for win32
	/*
  #ifndef _WIN32
          regex_t regex;
          if (regcomp(&regex, str2.c_str(), 0)==0) {
                  int res=regexec(&regex, str1.c_str(),0, nullptr, 0 );
                  regfree(&regex);
                  if (res==0) {
                          return true;
                  }
          }
  #endif
  */
}

bool CRapidDownloader::setOption(const std::string& key,
				 const std::string& value)
{
	LOG_INFO("setOption %s = %s", key.c_str(), value.c_str());
	if (key == "masterurl") {
		reposgzurl = value;
		return true;
	}
	if (key == "repo_timeout_seconds") {
		repoHttpTimeoutSeconds = ParseInt(value, 0);
		return true;
	}
	if (key == "forceupdate") {
		forceUpdate = true;
		rapidGitResolvedTags.clear();
		return true;
	}
	if (key == "git_enabled") {
		rapidGitEnabled = ParseBool(value);
		rapidGitResolvedTags.clear();
		return true;
	}
	if (key == "git_manifest_url") {
		rapidGitManifestUrl = value;
		rapidGitResolvedTags.clear();
		return true;
	}
	if (key == "git_manifest_ttl") {
		rapidGitManifestTtlSeconds =
		    ParseInt(value, rapidGitManifestTtlSeconds);
		rapidGitResolvedTags.clear();
		return true;
	}
	if (key == "git_api_timeout") {
		rapidGitApiTimeoutSeconds =
		    ParseInt(value, rapidGitApiTimeoutSeconds);
		return true;
	}
	return IDownloader::setOption(key, value);
}

bool CRapidDownloader::UpdateReposGZ()
{
	const bool force = forceUpdate;
	forceUpdate = false;

	std::string tmp;
	if (!urlToPath(reposgzurl, tmp)) {
		LOG_ERROR("Invalid path: %s", tmp.c_str());
		return false;
	}
	path = fileSystem->getSpringDir() + PATH_DELIMITER + "rapid" +
	       PATH_DELIMITER + tmp;
	fileSystem->createSubdirs(CFileSystem::DirName(path));
	LOG_DEBUG("%s", reposgzurl.c_str());
	// first try already downloaded file, as repo master file rarely changes
	if (!force && (fileSystem->fileExists(path)) && (!fileSystem->isOlder(path, REPO_MASTER_RECHECK_TIME)) && parse())
		return true;
	IDownload dl(path);
	dl.addMirror(reposgzurl);
	dl.timeoutSeconds = repoHttpTimeoutSeconds;
	return httpDownload->download(&dl) && parse();
}

static bool ParseFD(FILE* f, const std::string& path, std::list<CRepo>& repos, CRapidDownloader* rapid)
{
	repos.clear();
	gzFile fp = gzdopen(fileno(f), "rb");
	if (fp == Z_NULL) {
		LOG_ERROR("Could not open %s", path.c_str());
		return false;
	}
	char buf[IO_BUF_SIZE];
	int i = 0;
	while (gzgets(fp, buf, sizeof(buf)) != Z_NULL) {
		const std::string line = buf;
		const std::vector<std::string> items = tokenizeString(line, ',');
		if (items.size() <= 2) { // create new repo from url
			LOG_ERROR("Parse Error %s, Line %d: %s", path.c_str(), i, buf);
			gzclose(fp);
			return false;
		}
		i++;
		CRepo repotmp = CRepo(items[1], items[0], rapid);
		repos.push_back(repotmp);
	}
	gzclose(fp);
	if (i <= 0) {
		LOG_ERROR("Broken %s: %d", path.c_str(), i);
		return false;
	}
	LOG_INFO("Found %d repos in %s", repos.size(), path.c_str());
	return true;
}

bool CRapidDownloader::parse()
{
	FILE* f = fileSystem->propen(path, "rb");
	if (f == nullptr) {
		return false;
	}

	const bool res = ParseFD(f, path, repos, this);
	fclose(f);
	if (!res) {
		CFileSystem::removeFile(path);
	}

	return res;
}

bool CRapidDownloader::updateRepos(const std::string& searchstr)
{
	const std::string::size_type pos = searchstr.find(':');
	if (pos != std::string::npos) { // a tag is found, set it
		const std::string tag = searchstr.substr(0, pos);
		// FIXME: tag isn't used??
	}

	LOG_DEBUG("%s", "Updating repos...");
	if (!UpdateReposGZ()) {
		return false;
	}

	std::list<IDownload*> dls;
	std::list<CRepo*> usedrepos;
	for (CRepo& repo : repos) {
		IDownload* dl = new IDownload();
		if (!repo.getDownload(*dl)) {
			delete dl;
			continue;
		}
		dl->timeoutSeconds = repoHttpTimeoutSeconds;
		usedrepos.push_back(&repo);
		dls.push_back(dl);
	}
	LOG_DEBUG("Downloading ...");
	httpDownload->download(dls);
	for (CRepo* repo : usedrepos) {
		repo->parse();
	}
	IDownloader::freeResult(dls);
	return true;
}
