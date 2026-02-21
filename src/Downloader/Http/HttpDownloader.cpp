/* This file is part of pr-downloader (GPL v2 or later), see the LICENSE file */

#include "HttpDownloader.h"

#include <algorithm>
#include <cctype>
#include <stdio.h>
#include <string>
#include <sstream>
#include <stdlib.h>
#include <set>
#include <map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

#include <json/reader.h>

#include "DownloadData.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/File.h"
#include "FileSystem/HashMD5.h"
#include "FileSystem/HashSHA1.h"
#include "Util.h"
#include "Logger.h"
#include "Downloader/Mirror.h"
#include "Downloader/CurlWrapper.h"

static std::vector<std::string> g_mapBaseUrls;
static long g_mapDownloadTimeoutSeconds = 0;
static long g_engineDownloadTimeoutSeconds = 0;

struct EngineProviderConfig
{
	std::string type;
	std::string url;
	std::string name;
};

static constexpr const char* kEngineProviderGithubReleases = "github_releases";
static constexpr const char* kEngineProviderSpringFiles = "springfiles";
static constexpr const char* kDefaultEngineGithubReleasesUrl = "https://api.github.com/repos/beyond-all-reason/RecoilEngine/releases?per_page=100";
static constexpr const char* kDefaultEngineSpringFilesUrl = "https://springfiles.springrts.com/json.php";
static std::vector<EngineProviderConfig> g_engineProviders;

static std::string NormalizeBaseUrl(std::string url)
{
	while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back()))) {
		url.pop_back();
	}
	size_t first = 0;
	while (first < url.size() && std::isspace(static_cast<unsigned char>(url[first]))) {
		++first;
	}
	if (first > 0) {
		url.erase(0, first);
	}
	if (!url.empty() && url.back() != '/') {
		url.push_back('/');
	}
	return url;
}

static void AppendUniqueUrl(std::vector<std::string>& urls, const std::string& url)
{
	if (url.empty()) {
		return;
	}
	if (std::find(urls.begin(), urls.end(), url) != urls.end()) {
		return;
	}
	urls.push_back(url);
}

static std::vector<std::string> ParseBaseUrlList(const std::string& value)
{
	std::vector<std::string> urls;
	std::string token;
	token.reserve(value.size());

	auto flushToken = [&]() {
		const std::string normalized = NormalizeBaseUrl(token);
		AppendUniqueUrl(urls, normalized);
		token.clear();
	};

	for (char ch : value) {
		if (ch == '\n' || ch == '\r' || ch == ',' || ch == ';') {
			flushToken();
			continue;
		}
		token.push_back(ch);
	}
	flushToken();
	return urls;
}

static long ParsePositiveLong(const std::string& value, long fallback)
{
	char* end = nullptr;
	const long parsed = strtol(value.c_str(), &end, 10);
	if (end == nullptr || *end != '\0' || parsed <= 0) {
		return fallback;
	}
	return parsed;
}

static std::vector<EngineProviderConfig> MakeDefaultEngineProviders()
{
	return {
	    {kEngineProviderGithubReleases, kDefaultEngineGithubReleasesUrl, "BAR GitHub"},
	    {kEngineProviderSpringFiles, kDefaultEngineSpringFilesUrl, "SpringFiles"},
	};
}

static void EnsureEngineProvidersInitialized()
{
	if (g_engineProviders.empty()) {
		g_engineProviders = MakeDefaultEngineProviders();
	}
}

static std::string ToLowerAscii(std::string s)
{
	for (char& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

static bool ParseEngineProvidersOption(const std::string& value,
				       std::vector<EngineProviderConfig>& providersOut,
				       std::string& errorOut)
{
	Json::Value root;
	Json::Reader reader;
	if (!reader.parse(value, root) || !root.isArray()) {
		errorOut = "engine_providers must be a JSON array";
		return false;
	}

	std::set<std::string> seen;
	for (Json::Value::ArrayIndex i = 0; i < root.size(); ++i) {
		const Json::Value item = root[i];
		if (!item.isObject()) {
			errorOut = "engine provider entry must be an object";
			return false;
		}
		if (!item["type"].isString() || !item["url"].isString()) {
			errorOut = "engine provider requires string type and url";
			return false;
		}

		EngineProviderConfig provider;
		provider.type = ToLowerAscii(item["type"].asString());
		provider.url = item["url"].asString();
		provider.name = item["name"].isString() ? item["name"].asString() : "";

		if (provider.type != kEngineProviderGithubReleases &&
		    provider.type != kEngineProviderSpringFiles) {
			errorOut = "unsupported engine provider type: " + provider.type;
			return false;
		}
		if (provider.url.empty()) {
			errorOut = "engine provider url cannot be empty";
			return false;
		}

		const std::string dedupeKey = provider.type + "\n" + provider.url;
		if (seen.insert(dedupeKey).second) {
			providersOut.push_back(std::move(provider));
		}
	}

	if (providersOut.empty()) {
		errorOut = "engine providers array is empty";
		return false;
	}
	return true;
}

static bool EndsWithCaseInsensitive(const std::string& value, const std::string& suffix)
{
	if (value.size() < suffix.size())
		return false;
	const size_t offset = value.size() - suffix.size();
	for (size_t i = 0; i < suffix.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(value[offset + i])) !=
		    std::tolower(static_cast<unsigned char>(suffix[i]))) {
			return false;
		}
	}
	return true;
}

static std::string UrlDecode(const std::string& input)
{
	std::string output;
	output.reserve(input.size());

	auto fromHex = [](char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};

	for (size_t i = 0; i < input.size(); ++i) {
		if (input[i] == '%' && i + 2 < input.size()) {
			const int hi = fromHex(input[i + 1]);
			const int lo = fromHex(input[i + 2]);
			if (hi >= 0 && lo >= 0) {
				output.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		if (input[i] == '+') {
			output.push_back(' ');
			continue;
		}
		output.push_back(input[i]);
	}
	return output;
}

static std::string StripQueryAndFragment(const std::string& input)
{
	size_t end = input.size();
	const size_t query = input.find('?');
	if (query != std::string::npos) {
		end = std::min(end, query);
	}
	const size_t fragment = input.find('#');
	if (fragment != std::string::npos) {
		end = std::min(end, fragment);
	}
	return input.substr(0, end);
}

static std::string BasenameFromPath(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	if (slash == std::string::npos)
		return path;
	return path.substr(slash + 1);
}

static std::string NormalizeMapToken(const std::string& input)
{
	std::string out;
	out.reserve(input.size());
	bool prevSpace = true;
	for (unsigned char ch : input) {
		if (std::isalnum(ch)) {
			out.push_back(static_cast<char>(std::tolower(ch)));
			prevSpace = false;
		} else if (!prevSpace) {
			out.push_back(' ');
			prevSpace = true;
		}
	}
	if (!out.empty() && out.back() == ' ')
		out.pop_back();
	return out;
}

static std::vector<std::string> SplitTokens(const std::string& input)
{
	std::vector<std::string> tokens;
	std::string token;
	for (char ch : input) {
		if (ch == ' ') {
			if (!token.empty()) {
				tokens.push_back(token);
				token.clear();
			}
			continue;
		}
		token.push_back(ch);
	}
	if (!token.empty()) {
		tokens.push_back(token);
	}
	return tokens;
}

static int MapNameScore(const std::string& requestedNorm, const std::string& candidateNorm)
{
	if (requestedNorm.empty() || candidateNorm.empty()) {
		return 0;
	}
	if (requestedNorm == candidateNorm) {
		return 10000;
	}
	if (candidateNorm.find(requestedNorm) != std::string::npos ||
	    requestedNorm.find(candidateNorm) != std::string::npos) {
		return 7000 - std::abs(static_cast<int>(requestedNorm.size()) - static_cast<int>(candidateNorm.size()));
	}

	const std::vector<std::string> reqTokens = SplitTokens(requestedNorm);
	const std::vector<std::string> candTokens = SplitTokens(candidateNorm);
	std::set<std::string> reqSet(reqTokens.begin(), reqTokens.end());
	int common = 0;
	for (const std::string& tok : candTokens) {
		if (reqSet.find(tok) != reqSet.end()) {
			++common;
		}
	}
	if (common == 0) {
		return 0;
	}
	const int tokenPenalty = std::abs(static_cast<int>(reqTokens.size()) - static_cast<int>(candTokens.size())) * 10;
	const int lengthPenalty = std::abs(static_cast<int>(requestedNorm.size()) - static_cast<int>(candidateNorm.size()));
	return common * 100 - tokenPenalty - lengthPenalty;
}

static std::vector<std::string> ExtractMapFilesFromListing(const std::string& listing, const std::string& baseUrl)
{
	std::set<std::string> files;
	size_t pos = 0;

	while ((pos = listing.find("href=", pos)) != std::string::npos) {
		pos += 5;
		if (pos >= listing.size()) {
			break;
		}

		size_t start = pos;
		size_t end = std::string::npos;
		const char quote = listing[pos];
		if (quote == '"' || quote == '\'') {
			start = pos + 1;
			end = listing.find(quote, start);
		} else {
			start = pos;
			end = start;
			while (end < listing.size() && !std::isspace(static_cast<unsigned char>(listing[end])) && listing[end] != '>') {
				++end;
			}
		}
		if (end == std::string::npos) {
			break;
		}

		std::string href = StripQueryAndFragment(listing.substr(start, end - start));
		pos = end + 1;
		if (href.empty()) {
			continue;
		}

		if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0) {
			if (href.rfind(baseUrl, 0) != 0) {
				continue;
			}
			href = href.substr(baseUrl.size());
		}

		href = BasenameFromPath(href);
		href = UrlDecode(href);
		if (!EndsWithCaseInsensitive(href, ".sd7") && !EndsWithCaseInsensitive(href, ".sdz")) {
			continue;
		}
		files.insert(href);
	}

	return std::vector<std::string>(files.begin(), files.end());
}

static bool SearchMapsFromCustomBaseUrl(std::list<IDownload*>& res,
					const std::string& requestedName,
					const std::string& baseUrl)
{
	if (baseUrl.empty()) {
		return false;
	}

	std::string listing;
	LOG_INFO("Map search: querying custom map index: %s", baseUrl.c_str());
	if (!CHttpDownloader::DownloadUrl(baseUrl, listing, g_mapDownloadTimeoutSeconds)) {
		LOG_WARN("Map search: custom map index request failed: %s", baseUrl.c_str());
		return false;
	}

	const std::vector<std::string> mapFiles = ExtractMapFilesFromListing(listing, baseUrl);
	if (mapFiles.empty()) {
		LOG_WARN("Map search: no .sd7/.sdz files found on custom map index: %s",
			 baseUrl.c_str());
		return false;
	}

	const std::string requestedNorm = NormalizeMapToken(requestedName);
	int bestScore = 0;
	std::string bestFile;

	for (const std::string& file : mapFiles) {
		const std::string lower = ToLowerAscii(file);
		const size_t dot = lower.find_last_of('.');
		if (dot == std::string::npos) {
			continue;
		}
		const std::string fileBase = file.substr(0, dot);
		const std::string candidateNorm = NormalizeMapToken(fileBase);
		const int score = MapNameScore(requestedNorm, candidateNorm);
		if (score > bestScore) {
			bestScore = score;
			bestFile = file;
		}
	}

	if (bestFile.empty()) {
		LOG_INFO("Map search: custom index had no matching file for '%s'", requestedName.c_str());
		return false;
	}

	std::string filename = fileSystem->getSpringDir();
	filename += PATH_DELIMITER;
	filename += "maps";
	filename += PATH_DELIMITER;
	filename += CFileSystem::EscapeFilename(bestFile);

	IDownload* dl = new IDownload(filename, requestedName, DownloadEnum::CAT_MAP);
	dl->addMirror(baseUrl + bestFile);
	res.push_back(dl);

	LOG_INFO("Map search: matched custom map '%s' -> %s", bestFile.c_str(),
		 (baseUrl + bestFile).c_str());
	return true;
}

static bool SearchMapsFromCustomBase(std::list<IDownload*>& res,
				     const std::string& requestedName)
{
	for (const std::string& baseUrl : g_mapBaseUrls) {
		if (SearchMapsFromCustomBaseUrl(res, requestedName, baseUrl)) {
			return true;
		}
	}
	return false;
}

static bool IsEngineCategory(DownloadEnum::Category cat)
{
	switch (cat) {
		case DownloadEnum::CAT_ENGINE:
		case DownloadEnum::CAT_ENGINE_LINUX:
		case DownloadEnum::CAT_ENGINE_LINUX64:
		case DownloadEnum::CAT_ENGINE_WINDOWS:
		case DownloadEnum::CAT_ENGINE_WINDOWS64:
		case DownloadEnum::CAT_ENGINE_MACOSX:
			return true;
		default:
			return false;
	}
}

static std::string CollapseWhitespace(const std::string& input)
{
	std::string out;
	out.reserve(input.size());

	bool inWhitespace = true;
	for (unsigned char ch : input) {
		if (std::isspace(ch)) {
			inWhitespace = true;
			continue;
		}
		if (!out.empty() && inWhitespace) {
			out.push_back(' ');
		}
		inWhitespace = false;
		out.push_back(static_cast<char>(ch));
	}
	return out;
}

static std::string NormalizeEngineVersionToken(std::string version)
{
	version = CollapseWhitespace(version);
	if (version.rfind("spring ", 0) == 0) {
		version.erase(0, 7);
	}
	const std::string::size_type spacePos = version.find(' ');
	const std::string token = (spacePos == std::string::npos) ? version : version.substr(0, spacePos);

	// Normalize date-style versions like YYYY.M.D(.0) to YYYY.MM.DD
	auto isDigits = [](const std::string& s) -> bool {
		if (s.empty())
			return false;
		for (unsigned char ch : s) {
			if (!std::isdigit(ch))
				return false;
		}
		return true;
	};

	auto splitDot = [](const std::string& s) -> std::vector<std::string> {
		std::vector<std::string> parts;
		std::string cur;
		for (char ch : s) {
			if (ch == '.') {
				parts.push_back(cur);
				cur.clear();
			} else {
				cur.push_back(ch);
			}
		}
		parts.push_back(cur);
		return parts;
	};

	const auto parts = splitDot(token);
	const bool maybeDate = (parts.size() == 3 || parts.size() == 4);
	if (maybeDate && parts[0].size() == 4 && isDigits(parts[0]) && isDigits(parts[1]) && isDigits(parts[2])) {
		const int month = std::stoi(parts[1]);
		const int day = std::stoi(parts[2]);
		const bool hasOptionalZeroSuffix = (parts.size() == 4);
		const bool optionalZeroIsValid = (!hasOptionalZeroSuffix) || parts[3] == "0";

		if (month >= 1 && month <= 12 && day >= 1 && day <= 31 && optionalZeroIsValid) {
			auto pad2 = [](int value) -> std::string {
				std::string s = std::to_string(value);
				if (s.size() == 1)
					return "0" + s;
				return s;
			};
			return parts[0] + "." + pad2(month) + "." + pad2(day);
		}
	}

	return token;
}

static bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle)
{
	if (needle.empty())
		return true;
	if (haystack.size() < needle.size())
		return false;
	for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
		bool ok = true;
		for (size_t j = 0; j < needle.size(); ++j) {
			if (std::tolower(static_cast<unsigned char>(haystack[i + j])) != std::tolower(static_cast<unsigned char>(needle[j]))) {
				ok = false;
				break;
			}
		}
		if (ok)
			return true;
	}
	return false;
}

static bool ParseBarSpringAssetName(const std::string& assetName, bool& isBar105, std::string& versionOut, std::string& platformOut)
{
	const std::string prefixBar = "spring_bar_.BAR.";
	const std::string prefixBar105 = "spring_bar_.BAR105.";
	std::string prefix;
	if (assetName.rfind(prefixBar105, 0) == 0) {
		isBar105 = true;
		prefix = prefixBar105;
	} else if (assetName.rfind(prefixBar, 0) == 0) {
		isBar105 = false;
		prefix = prefixBar;
	} else {
		return false;
	}

	const auto underscorePos = assetName.find('_', prefix.size());
	if (underscorePos == std::string::npos)
		return false;

	const auto suffixPos = assetName.rfind(".7z");
	if (suffixPos == std::string::npos || suffixPos <= underscorePos)
		return false;

	versionOut = assetName.substr(prefix.size(), underscorePos - prefix.size());
	platformOut = assetName.substr(underscorePos + 1, suffixPos - (underscorePos + 1));
	return !versionOut.empty() && !platformOut.empty();
}

static bool ParseRecoilEngineAssetName(const std::string& assetName, std::string& versionOut, std::string& platformOut)
{
	// Current BAR engine assets are distributed from beyond-all-reason/RecoilEngine with names like:
	// recoil_2025.06.14_amd64-linux.7z
	// recoil_2025.06.14_amd64-windows.7z
	const std::string prefix = "recoil_";
	if (assetName.rfind(prefix, 0) != 0) {
		return false;
	}
	if (assetName.find("dbgsym") != std::string::npos) {
		return false;
	}
	if (assetName.find("tracy") != std::string::npos) {
		return false;
	}

	const auto afterPrefix = prefix.size();
	const auto underscorePos = assetName.find('_', afterPrefix);
	if (underscorePos == std::string::npos) {
		return false;
	}
	const auto suffixPos = assetName.rfind(".7z");
	if (suffixPos == std::string::npos || suffixPos <= underscorePos) {
		return false;
	}

	versionOut = assetName.substr(afterPrefix, underscorePos - afterPrefix);
	platformOut = assetName.substr(underscorePos + 1, suffixPos - (underscorePos + 1));
	return !versionOut.empty() && !platformOut.empty();
}

static bool BarPlatformMatches(DownloadEnum::Category cat, const std::string& platformToken)
{
	auto startsWith = [](const std::string& s, const std::string& prefix) -> bool {
		return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
	};

	switch (cat) {
		case DownloadEnum::CAT_ENGINE_LINUX64:
			return startsWith(platformToken, "linux-64-minimal-portable");
		case DownloadEnum::CAT_ENGINE_LINUX:
			return startsWith(platformToken, "linux-32-minimal-portable") || startsWith(platformToken, "linux32-minimal-portable");
		case DownloadEnum::CAT_ENGINE_WINDOWS64:
			return startsWith(platformToken, "windows-64-minimal-portable");
		case DownloadEnum::CAT_ENGINE_WINDOWS:
			return startsWith(platformToken, "windows-32-minimal-portable") || startsWith(platformToken, "windows32-minimal-portable");
		case DownloadEnum::CAT_ENGINE_MACOSX:
			return startsWith(platformToken, "macos");
		default:
			return false;
	}
}

static bool RecoilPlatformMatches(DownloadEnum::Category cat, const std::string& platformToken)
{
	switch (cat) {
		case DownloadEnum::CAT_ENGINE_LINUX64:
			return platformToken == "amd64-linux";
		case DownloadEnum::CAT_ENGINE_WINDOWS64:
			return platformToken == "amd64-windows";
		default:
			return false;
	}
}

static bool SearchBarGithubSpringReleases(std::list<IDownload*>& res,
					  const std::string& requestedName,
					  DownloadEnum::Category cat,
					  const std::string& apiUrl,
					  long timeoutSeconds,
					  const std::string& providerName)
{
	std::string json;
	CHttpDownloader http;
	const std::string label = providerName.empty() ? apiUrl : providerName;
	LOG_INFO("Engine search: querying GitHub provider '%s': %s",
		 label.c_str(), apiUrl.c_str());
	if (!http.DownloadUrl(apiUrl, json, timeoutSeconds)) {
		LOG_WARN("Engine search: GitHub provider request failed: %s",
			 label.c_str());
		return false;
	}

	Json::Value root;
	Json::Reader reader;
	if (!reader.parse(json, root) || !root.isArray()) {
		LOG_WARN("Engine search: GitHub provider JSON parse failed: %s",
			 label.c_str());
		return false;
	}

	const bool wantBar105 = ContainsCaseInsensitive(requestedName, "BAR105");
	const std::string requestedToken = NormalizeEngineVersionToken(requestedName);

	bool added = false;
	for (Json::Value::ArrayIndex i = 0; i < root.size(); ++i) {
		const Json::Value release = root[i];
		const Json::Value assets = release["assets"];
		if (!assets.isArray())
			continue;

		for (Json::Value::ArrayIndex j = 0; j < assets.size(); ++j) {
			const Json::Value asset = assets[j];
			if (!asset.isObject())
				continue;

			const Json::Value nameV = asset["name"];
			const Json::Value urlV = asset["browser_download_url"];
			if (!nameV.isString() || !urlV.isString())
				continue;

			const std::string assetName = nameV.asString();
			const std::string downloadUrl = urlV.asString();

			std::string assetVersion;
			std::string platformToken;

			// Support both legacy spring_bar_.BAR.* assets and current RecoilEngine recoil_* assets.
			bool matchedFormat = false;
			if (assetName.rfind("recoil_", 0) == 0) {
				matchedFormat = ParseRecoilEngineAssetName(assetName, assetVersion, platformToken) && RecoilPlatformMatches(cat, platformToken);
				// BAR105 is a legacy naming variant; recoil_ assets don't encode BAR/BAR105.
				if (wantBar105) {
					matchedFormat = false;
				}
			} else {
				bool isBar105 = false;
				matchedFormat = ParseBarSpringAssetName(assetName, isBar105, assetVersion, platformToken) && BarPlatformMatches(cat, platformToken) && (wantBar105 == isBar105);
			}

			if (!matchedFormat) {
				continue;
			}

			const std::string normalizedAssetVersion = NormalizeEngineVersionToken(assetVersion);
			if (normalizedAssetVersion != requestedToken)
				continue;

			std::string filename = fileSystem->getSpringDir();
			filename += PATH_DELIMITER;
			filename += "engine";
			filename += PATH_DELIMITER;
			filename += CFileSystem::EscapeFilename(assetName);

			IDownload* dl = new IDownload(filename, requestedName, cat);
			dl->addMirror(downloadUrl);
			// Ensure extracted engine folder name matches what the lobby expects for this battle/version.
			dl->version = requestedName;
			res.push_back(dl);
			added = true;
			LOG_INFO("Engine search: matched GitHub asset '%s' -> %s",
				 assetName.c_str(), downloadUrl.c_str());
		}
	}

	if (!added) {
		LOG_INFO("Engine search: no GitHub assets matched for '%s' (provider: %s)",
			 requestedName.c_str(), label.c_str());
	}
	return added;
}

static size_t WriteMemoryCallback(void* contents, size_t size, size_t nmemb,
				  void* userp)
{
	if (IDownloader::AbortDownloads()) {
		return -1;
	}

	const size_t realsize = size * nmemb;
	std::string* res = static_cast<std::string*>(userp);
	res->append((char*)contents, realsize);
	return realsize;
}

static int progress_func(DownloadData* data, double total, double done, double,
			 double)
{
	if (IDownloader::AbortDownloads()) {
		return -1;
	}

	data->download->progress = done;
	if (IDownloader::listener != nullptr) {
		IDownloader::listener(done, total);
	}
	if (data->got_ranges) {
		LOG_PROGRESS(done, total, done >= total);
	}
	return 0;
}

// downloads url into res
bool CHttpDownloader::DownloadUrl(const std::string& url, std::string& res,
				  long timeoutSeconds)
{
	DownloadData d;
	d.got_ranges = false;
	d.download = new IDownload();
	d.download->addMirror(url);
	d.download->name = url;
	d.download->origin_name = url;

	CurlWrapper curlw;
	curl_easy_setopt(curlw.GetHandle(), CURLOPT_URL, CurlWrapper::escapeUrl(url).c_str());
	curl_easy_setopt(curlw.GetHandle(), CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curlw.GetHandle(), CURLOPT_WRITEDATA, (void*)&res);
	curl_easy_setopt(curlw.GetHandle(), CURLOPT_PROGRESSDATA, &d);
	curl_easy_setopt(curlw.GetHandle(), CURLOPT_XFERINFOFUNCTION, progress_func);
	curl_easy_setopt(curlw.GetHandle(), CURLOPT_NOPROGRESS, 0L);
	if (timeoutSeconds > 0) {
		curl_easy_setopt(curlw.GetHandle(), CURLOPT_TIMEOUT, timeoutSeconds);
		curl_easy_setopt(curlw.GetHandle(), CURLOPT_CONNECTTIMEOUT, timeoutSeconds);
		curl_easy_setopt(curlw.GetHandle(), CURLOPT_LOW_SPEED_TIME, timeoutSeconds);
	}
	const CURLcode curlres = curl_easy_perform(curlw.GetHandle());

	delete d.download;
	d.download = nullptr;
	if (curlres != CURLE_OK) {
		const long effectiveTimeout = (timeoutSeconds > 0) ? timeoutSeconds : 30;
		LOG_ERROR("Error in curl %s (%s) [url=%s timeout=%lds]",
			  curl_easy_strerror(curlres), curlw.GetError().c_str(),
			  url.c_str(), effectiveTimeout);
	}
	return curlres == CURLE_OK;
}

static std::string getRequestUrl(const std::string& searchBaseUrl,
				 const std::string& name,
				 DownloadEnum::Category cat)
{
	std::string url = searchBaseUrl;
	if (url.find('?') == std::string::npos) {
		url.push_back('?');
	} else if (!url.empty() && url.back() != '?' && url.back() != '&') {
		url.push_back('&');
	}
	if (cat != DownloadEnum::CAT_NONE) {
		url += "category=" + DownloadEnum::getCat(cat) + std::string("&");
	}
	return url + std::string("springname=") + name;
}

bool CHttpDownloader::ParseResult(const std::string& /*name*/,
				  const std::string& json,
				  std::list<IDownload*>& res)
{
	Json::Value result; // will contains the root value after parsing.
	Json::Reader reader;
	const bool parsingSuccessful = reader.parse(json, result);
	if (!parsingSuccessful) {
		LOG_ERROR("Couldn't parse result: %s %s",
			  reader.getFormattedErrorMessages().c_str(), json.c_str());
		return false;
	}

	if (!result.isArray()) {
		LOG_ERROR("Returned json isn't an array!");
		return false;
	}

	for (Json::Value::ArrayIndex i = 0; i < result.size(); i++) {
		const Json::Value resfile = result[i];

		if (!resfile.isObject()) {
			LOG_ERROR("Entry isn't object!");
			return false;
		}
		if (!resfile["category"].isString()) {
			LOG_ERROR("No category in result");
			return false;
		}
		if (!resfile["springname"].isString()) {
			LOG_ERROR("No springname in result");
			return false;
		}
		std::string filename = fileSystem->getSpringDir();
		const std::string category = resfile["category"].asString();
		const std::string springname = resfile["springname"].asString();
		filename += PATH_DELIMITER;

		if (category == "map") {
			filename += "maps";
		} else if (category == "game") {
			filename += "games";
		} else if (category.find("engine") ==
			   0) { // engine_windows, engine_linux, engine_macosx
			filename += "engine";
		} else
			LOG_ERROR("Unknown Category %s", category.c_str());
		filename += PATH_DELIMITER;

		if ((!resfile["mirrors"].isArray()) || (!resfile["filename"].isString())) {
			LOG_ERROR("Invalid type in result");
			return false;
		}
		filename.append(
		    CFileSystem::EscapeFilename(resfile["filename"].asString()));

		const DownloadEnum::Category cat = DownloadEnum::getCatFromStr(category);
		IDownload* dl = new IDownload(filename, springname, cat);
		if (category == "map" && resfile["filename"].isString()) {
			for (const std::string& baseUrl : g_mapBaseUrls) {
				dl->addMirror(baseUrl + resfile["filename"].asString());
			}
		}
		const Json::Value mirrors = resfile["mirrors"];
		for (Json::Value::ArrayIndex j = 0; j < mirrors.size(); j++) {
			if (!mirrors[j].isString()) {
				LOG_ERROR("Invalid type in result");
			} else {
				dl->addMirror(mirrors[j].asString());
			}
		}

		if (resfile["version"].isString()) {
			const std::string& version = resfile["version"].asString();
			dl->version = version;
		}
		if (resfile["md5"].isString()) {
			dl->hash = new HashMD5();
			dl->hash->Set(resfile["md5"].asString());
		}
		if (resfile["size"].isInt()) {
			dl->size = resfile["size"].asInt();
		}
		if (resfile["depends"].isArray()) {
			for (Json::Value::ArrayIndex i = 0; i < resfile["depends"].size(); i++) {
				if (resfile["depends"][i].isString()) {
					const std::string& dep = resfile["depends"][i].asString();
					dl->addDepend(dep);
				}
			}
		}
		res.push_back(dl);
	}
	LOG_DEBUG("Parsed %d results", res.size());
	return true;
}

bool CHttpDownloader::search(std::list<IDownload*>& res,
			     const std::string& name,
			     DownloadEnum::Category cat)
{
	LOG_DEBUG("%s", name.c_str());
	bool ok = false;

	// For maps, prefer custom map base URL as primary source.
	if (cat == DownloadEnum::CAT_MAP && SearchMapsFromCustomBase(res, name)) {
		return true;
	}

	if (IsEngineCategory(cat)) {
		EnsureEngineProvidersInitialized();
		for (const EngineProviderConfig& provider : g_engineProviders) {
			if (provider.type == kEngineProviderGithubReleases) {
				if (SearchBarGithubSpringReleases(
					res, name, cat, provider.url,
					g_engineDownloadTimeoutSeconds,
					provider.name)) {
					return true;
				}
				continue;
			}

			if (provider.type == kEngineProviderSpringFiles) {
				const std::string url = getRequestUrl(provider.url, name, cat);
				const std::string label = provider.name.empty()
							      ? provider.url
							      : provider.name;
				LOG_INFO("Engine search: querying SpringFiles provider '%s': %s",
					 label.c_str(), url.c_str());

				std::string dlres;
				if (!DownloadUrl(url, dlres, g_engineDownloadTimeoutSeconds)) {
					continue;
				}

				std::list<IDownload*> providerResults;
				if (ParseResult(name, dlres, providerResults) &&
				    !providerResults.empty()) {
					res.splice(res.end(), providerResults);
					return true;
				}
				IDownloader::freeResult(providerResults);
			}
		}

		return false;
	}

	std::string dlres;
	const std::string url = getRequestUrl(HTTP_SEARCH_URL, name, cat);
	LOG_INFO("Content search: querying SpringFiles: %s", url.c_str());
	const long queryTimeout = (cat == DownloadEnum::CAT_MAP) ? g_mapDownloadTimeoutSeconds : 0;
	if (DownloadUrl(url, dlres, queryTimeout) && ParseResult(name, dlres, res)) {
		ok = !res.empty();
	}

	return ok;
}

bool CHttpDownloader::setOption(const std::string& key, const std::string& value)
{
	if (key == "map_base_url") {
		g_mapBaseUrls.clear();
		AppendUniqueUrl(g_mapBaseUrls, NormalizeBaseUrl(value));
		LOG_INFO("setOption %s = %s", key.c_str(),
			 g_mapBaseUrls.empty() ? "" : g_mapBaseUrls.front().c_str());
		return true;
	}
	if (key == "map_base_urls") {
		g_mapBaseUrls = ParseBaseUrlList(value);
		LOG_INFO("setOption %s count = %d", key.c_str(),
			 static_cast<int>(g_mapBaseUrls.size()));
		return true;
	}
	if (key == "map_download_timeout_seconds") {
		g_mapDownloadTimeoutSeconds = ParsePositiveLong(value, 0);
		LOG_INFO("setOption %s = %ld", key.c_str(), g_mapDownloadTimeoutSeconds);
		return true;
	}
	if (key == "engine_download_timeout_seconds") {
		g_engineDownloadTimeoutSeconds = ParsePositiveLong(value, 0);
		LOG_INFO("setOption %s = %ld", key.c_str(), g_engineDownloadTimeoutSeconds);
		return true;
	}
	if (key == "engine_providers") {
		std::vector<EngineProviderConfig> providers;
		std::string error;
		if (!ParseEngineProvidersOption(value, providers, error)) {
			LOG_ERROR("setOption %s parse failed: %s. Falling back to defaults.",
				  key.c_str(), error.c_str());
			g_engineProviders = MakeDefaultEngineProviders();
			return false;
		}
		g_engineProviders = std::move(providers);
		LOG_INFO("setOption %s count = %d", key.c_str(),
			 static_cast<int>(g_engineProviders.size()));
		return true;
	}
	return IDownloader::setOption(key, value);
}

static size_t multi_write_data(void* ptr, size_t size, size_t nmemb,
			       DownloadData* data)
{
	if (IDownloader::AbortDownloads())
		return -1;

	// LOG_DEBUG("%d %d",size,  nmemb);
	if (!data->got_ranges) {
		LOG_INFO("Server refused ranges"); // The server refused ranges , download
						   // only from this piece , overwrite from
						   // 0 , and drop everything else

		data->download->write_only_from = data;
		data->got_ranges = true; // Silence the error
	}
	if (data->download->write_only_from != nullptr &&
	    data->download->write_only_from != data)
		return size * nmemb;
	else if (data->download->write_only_from != nullptr) {
		return data->download->file->Write((const char*)ptr, size * nmemb, 0);
	}
	return data->download->file->Write((const char*)ptr, size * nmemb,
					   data->start_piece);
}

static size_t multiHeader(void* ptr, size_t size, size_t nmemb,
			  DownloadData* data)
{
	// no chunked transfer, don't check headers
	if (data->download->pieces.empty()) {
		LOG_DEBUG("Unchunked transfer!");
		data->got_ranges = true;
		return size * nmemb;
	}
	const std::string buf((char*)ptr, size * nmemb - 1);
	int start, end, total;
	int count = sscanf(buf.c_str(), "Content-Range: bytes %d-%d/%d", &start, &end,
			   &total);
	if (count == 3) {
		int piecesize = data->download->file->GetPiecesSize(data->pieces);
		if (end - start + 1 != piecesize) {
			LOG_DEBUG("piecesize %d doesn't match server size: %d", piecesize,
				  end - start + 1);
			return -1;
		}
		data->got_ranges = true;
	}
	LOG_DEBUG("%s", buf.c_str());
	return size * nmemb;
}

bool CHttpDownloader::getRange(std::string& range, int start_piece,
			       int num_pieces, int piecesize)
{
	std::ostringstream s;
	s << (int)(piecesize * start_piece) << "-"
	  << (piecesize * start_piece) + piecesize * num_pieces - 1;
	range = s.str();
	LOG_DEBUG("%s", range.c_str());
	return true;
}

void CHttpDownloader::showProcess(IDownload* download, bool force)
{
	const int done = download->getProgress();
	const int size = download->size;
	if (listener != nullptr) {
		listener(done, size);
	}
	LOG_PROGRESS(done, size, force);
}

std::vector<unsigned int>
CHttpDownloader::verifyAndGetNextPieces(CFile& file, IDownload* download)
{
	std::vector<unsigned int> pieces;
	if (download->isFinished()) {
		return pieces;
	}
	// verify file by md5 if pieces.size == 0
	if ((download->pieces.empty()) && (download->hash != nullptr) &&
	    (download->hash->isSet())) {
		HashMD5 md5;
		file.Hash(md5);
		if (md5.compare(download->hash)) {
			LOG_INFO("md5 correct: %s", md5.toString().c_str());
			download->state = IDownload::STATE_FINISHED;
			showProcess(download, true);
			return pieces;
		} else {
			LOG_INFO("md5 sum missmatch %s %s", download->hash->toString().c_str(),
				 md5.toString().c_str());
		}
	}

	HashSHA1 sha1;
	unsigned alreadyDl = 0;
	for (unsigned i = 0; i < download->pieces.size();
	     i++) { // find first not downloaded piece
		showProcess(download, false);
		IDownload::piece& p = download->pieces[i];
		if (p.state == IDownload::STATE_FINISHED) {
			alreadyDl++;
			LOG_DEBUG("piece %d marked as downloaded", i);
			if (pieces.size() > 0)
				break; // Contiguos non-downloaded area finished
			continue;
		} else if (p.state == IDownload::STATE_NONE) {
			if ((p.sha->isSet()) &&
			    (!file.IsNewFile())) { // reuse piece, if checksum is fine
				file.Hash(sha1, i);
				//	LOG("bla %s %s", sha1.toString().c_str(),
				// download.pieces[i].sha->toString().c_str());
				if (sha1.compare(download->pieces[i].sha)) {
					LOG_DEBUG("piece %d has already correct checksum, reusing", i);
					p.state = IDownload::STATE_FINISHED;
					showProcess(download, true);
					alreadyDl++;
					if (pieces.size() > 0)
						break; // Contiguos non-downloaded area finished
					continue;
				}
			}
			pieces.push_back(i);
			if (pieces.size() ==
			    download->pieces.size() / download->parallel_downloads)
				break;
		}
	}
	if (pieces.size() == 0 && download->pieces.size() != 0) {
		LOG_DEBUG("Finished\n");
		download->state = IDownload::STATE_FINISHED;
		showProcess(download, true);
	}
	LOG_DEBUG("Pieces to download: %d", pieces.size());
	return pieces;
}

bool CHttpDownloader::setupDownload(DownloadData* piece)
{
	std::vector<unsigned int> pieces =
	    verifyAndGetNextPieces(*(piece->download->file), piece->download);
	if (piece->download->isFinished())
		return false;
	if (piece->download->file) {
		piece->download->size = piece->download->file->GetPieceSize(-1);
		LOG_DEBUG("Size is %d", piece->download->size);
	}
	piece->start_piece = pieces.size() > 0 ? pieces[0] : -1;
	assert(piece->download->pieces.size() <= 0 || piece->start_piece >= 0);
	piece->pieces = pieces;
	piece->curlw = std::unique_ptr<CurlWrapper>(new CurlWrapper());

	CURL* curle = piece->curlw->GetHandle();
	piece->mirror = piece->download->getFastestMirror();
	if (piece->mirror == nullptr) {
		LOG_ERROR("No mirror found for %s", piece->download->name.c_str());
		return false;
	}

	curl_easy_setopt(curle, CURLOPT_WRITEFUNCTION, multi_write_data);
	curl_easy_setopt(curle, CURLOPT_WRITEDATA, piece);
	curl_easy_setopt(curle, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curle, CURLOPT_PROGRESSDATA, piece);
	curl_easy_setopt(curle, CURLOPT_XFERINFOFUNCTION, progress_func);
	curl_easy_setopt(curle, CURLOPT_URL, CurlWrapper::escapeUrl(piece->mirror->url).c_str());
	long timeoutSeconds = 0;
	if (piece->download->cat == DownloadEnum::CAT_MAP &&
	    g_mapDownloadTimeoutSeconds > 0) {
		timeoutSeconds = g_mapDownloadTimeoutSeconds;
	} else if (IsEngineCategory(piece->download->cat) &&
		   g_engineDownloadTimeoutSeconds > 0) {
		timeoutSeconds = g_engineDownloadTimeoutSeconds;
	} else if (piece->download->timeoutSeconds > 0) {
		timeoutSeconds = piece->download->timeoutSeconds;
	}
	if (timeoutSeconds > 0) {
		curl_easy_setopt(curle, CURLOPT_TIMEOUT, timeoutSeconds);
		curl_easy_setopt(curle, CURLOPT_CONNECTTIMEOUT, timeoutSeconds);
		curl_easy_setopt(curle, CURLOPT_LOW_SPEED_TIME, timeoutSeconds);
	}

	curl_easy_setopt(curle, CURLOPT_SSL_VERIFYPEER, piece->download->validateTLS);
	LOG_DEBUG("Validating TLS: %d", piece->download->validateTLS);

	if ((piece->download->size > 0) && (piece->start_piece >= 0) &&
	    piece->download->pieces.size() > 0) { // don't set range, if size unknown
		std::string range;
		if (!getRange(range, piece->start_piece, piece->pieces.size(),
			      piece->download->piecesize)) {
			LOG_ERROR("Error getting range for download");
			return false;
		}
		// set range for request, format is <start>-<end>
		if (!(piece->start_piece == 0 &&
		      piece->pieces.size() == piece->download->pieces.size()))
			curl_easy_setopt(curle, CURLOPT_RANGE, range.c_str());
		// parse server response	header as well
		curl_easy_setopt(curle, CURLOPT_HEADERFUNCTION, multiHeader);
		curl_easy_setopt(curle, CURLOPT_WRITEHEADER, piece);
		for (std::vector<unsigned int>::iterator it = piece->pieces.begin();
		     it != piece->pieces.end(); ++it)
			piece->download->pieces[*it].state = IDownload::STATE_DOWNLOADING;
	} else { //
		LOG_DEBUG("single piece transfer");
		piece->got_ranges = true;

		// this sets the header If-Modified-Since -> downloads only when remote file
		// is newer than local file
		const long timestamp = piece->download->file->GetTimestamp();
		if ((timestamp >= 0) &&
		    (piece->download->hash ==
		     nullptr)) { // timestamp known + hash not known -> only dl when changed
			curl_easy_setopt(curle, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
			curl_easy_setopt(curle, CURLOPT_TIMEVALUE, timestamp);
			curl_easy_setopt(curle, CURLOPT_FILETIME, 1);
		}
	}
	return true;
}

DownloadData* CHttpDownloader::getDataByHandle(const std::vector<DownloadData*>& downloads,
				 const CURL* easy_handle) const
{
	// search corresponding data structure
	for (size_t i = 0; i < downloads.size(); i++) {
		if (downloads[i]->curlw == nullptr) { // inactive download
			continue;
		}
		if (downloads[i]->curlw->GetHandle() == easy_handle) {
			return downloads[i];
		}
	}
	return nullptr;
}

void CHttpDownloader::VerifyPieces(DownloadData& data, HashSHA1& sha1)
{
	for (size_t idx = 0; idx < data.pieces.size(); idx++) {
		IDownload::piece& p = data.download->pieces[idx];
		if (p.sha->isSet()) {
			data.download->file->Hash(sha1, idx);

			if (sha1.compare(p.sha)) { // piece valid
				p.state = IDownload::STATE_FINISHED;
				showProcess(data.download, true);
				// LOG("piece %d verified!", idx);
			} else { // piece download broken, mark mirror as broken (for this
				 // file)
				p.state = IDownload::STATE_NONE;
				data.mirror->status = Mirror::STATUS_BROKEN;
				// FIXME: cleanup curl handle here + process next dl
				LOG_WARN("Piece %d is invalid", idx);
			}
		} else {
			LOG_INFO("sha1 checksum seems to be not set, can't check received "
				 "piece %d-%d",
				 data.start_piece, data.pieces.size());
		}
	}
}


bool CHttpDownloader::processMessages(CURLM* curlm,
				      std::vector<DownloadData*>& downloads)
{
	int msgs_left;
	HashSHA1 sha1;
	bool aborted = false;
	while (struct CURLMsg* msg = curl_multi_info_read(curlm, &msgs_left)) {
		switch (msg->msg) {
			case CURLMSG_DONE: { // a piece has been downloaded, verify it
				DownloadData* data = getDataByHandle(downloads, msg->easy_handle);
				if (data == nullptr) {
					LOG_ERROR("Couldn't find download in download list");
					return true;
				}
				switch (msg->data.result) {
					case CURLE_OK:
						break;
					case CURLE_HTTP_RETURNED_ERROR: // some 4* HTTP-Error (file not found,
									// access denied,...)
					default:
						long http_code = 0;
						curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &http_code);
						LOG_ERROR("CURL error(%d:%d): %s %d (%s)", msg->msg, msg->data.result,
							  curl_easy_strerror(msg->data.result), http_code,
							  data->mirror != nullptr ? data->mirror->url.c_str() : "<unknown>");
						if (data->start_piece >= 0) {
							data->download->pieces[data->start_piece].state =
							    IDownload::STATE_NONE;
						}
						if (data->mirror != nullptr) {
							data->mirror->status = Mirror::STATUS_BROKEN;
						}
						// FIXME: cleanup curl handle here + process next dl
				}

				if (data->start_piece < 0) { // single-piece download (no ranges/pieces)
					if (msg->data.result == CURLE_OK) {
						data->download->state = IDownload::STATE_FINISHED;
						showProcess(data->download, true);
						double dlSpeed = 0.0;
						curl_easy_getinfo(data->curlw->GetHandle(), CURLINFO_SPEED_DOWNLOAD_T, &dlSpeed);
						if (data->mirror != nullptr) {
							data->mirror->UpdateSpeed(dlSpeed);
							if (data->mirror->status == Mirror::STATUS_UNKNOWN) {
								data->mirror->status = Mirror::STATUS_OK;
							}
						}
						LOG_INFO("single piece finished");
					}

					curl_multi_remove_handle(curlm, data->curlw->GetHandle());
					data->curlw = nullptr;
					break;
				}

				assert(data->download->file != nullptr);
				assert(data->start_piece < (int)data->download->pieces.size());

				VerifyPieces(*data, sha1);

				// get speed at which this piece was downloaded + update mirror info
				double dlSpeed;
				curl_easy_getinfo(data->curlw->GetHandle(), CURLINFO_SPEED_DOWNLOAD_T,
						  &dlSpeed);
				data->mirror->UpdateSpeed(dlSpeed);
				if (data->mirror->status ==
				    Mirror::STATUS_UNKNOWN) // set mirror status only when unset
					data->mirror->status = Mirror::STATUS_OK;

				// remove easy handle, as its finished
				curl_multi_remove_handle(curlm, data->curlw->GetHandle());
				data->curlw = nullptr;
				LOG_INFO("piece finished");
				// piece finished / failed, try a new one
				if (!setupDownload(data)) {
					LOG_DEBUG(
					    "No piece found, all pieces finished / currently downloading");
					break;
				}
				const int ret = curl_multi_add_handle(curlm, data->curlw->GetHandle());
				if (ret != CURLM_OK) {
					LOG_ERROR("curl_multi_perform_error: %d %d", ret,
						  CURLM_BAD_EASY_HANDLE);
				}
				break;
			}
			default:
				LOG_ERROR("Unhandled message %d", msg->msg);
		}
	}
	return aborted;
}

static void CleanupDownloads(std::list<IDownload*>& download,
			     std::vector<DownloadData*>& downloads)
{
	// close all open files
	for (IDownload* dl : download) {
		if (dl->file != nullptr) {
			dl->file->Close();
		}
	}
	for (size_t i = 0; i < downloads.size(); i++) {
		long timestamp = 0;
		if ((downloads[i]->curlw != nullptr) &&
		    curl_easy_getinfo(downloads[i]->curlw->GetHandle(), CURLINFO_FILETIME, &timestamp) == CURLE_OK) {
			if (timestamp > 0) {
				// decrease local timestamp if download failed to force redownload next time
				if (!downloads[i]->download->isFinished())
					timestamp--;
				downloads[i]->download->file->SetTimestamp(timestamp);
			}
			delete downloads[i]->download->file;
			downloads[i]->download->file = nullptr;
		}
		delete downloads[i];
	}

	downloads.clear();
}

void VerifySinglePieceDownload(IDownload& dl)
{
	if ((dl.hash == nullptr) || (dl.file == nullptr))
		return;

	if (dl.file->Hash(*dl.hash)) {
		dl.state = IDownload::STATE_FINISHED;
	}

}

bool CHttpDownloader::download(std::list<IDownload*>& download,
			       int max_parallel)
{
	std::vector<DownloadData*> downloads;
	CURLM* curlm = curl_multi_init();
	for (IDownload* dl : download) {
		if (dl->isFinished()) {
			continue;
		}
		if (dl->dltype != IDownload::TYP_HTTP) {
			LOG_DEBUG("skipping non http-dl")
			continue;
		}
		const int count = std::min(
		    max_parallel,
		    std::max(
			1, std::min((int)dl->pieces.size(),
				    dl->getMirrorCount()))); // count of parallel downloads
		if (dl->getMirrorCount() <= 0) {
			LOG_WARN("No mirrors found");
			return false;
		}
		LOG_DEBUG("Using %d parallel downloads", count);
		dl->parallel_downloads = count;
		if (dl->file == nullptr) {
			dl->file = new CFile();
			if (!dl->file->Open(dl->name, dl->size, dl->piecesize)) {
				delete dl->file;
				dl->file = nullptr;
				return false;
			}
		}
		for (int i = 0; i < count; i++) {
			DownloadData* dlData = new DownloadData();
			dlData->download = dl;
			// no piece found (all pieces already downloaded), skip
			if (!setupDownload(dlData)) {
				if (dl->state != IDownload::STATE_FINISHED) {
					LOG_ERROR("Failed to setup download %d/%d", i, count);
				}
				delete dlData;
				continue;
			}
			downloads.push_back(dlData);
			curl_multi_add_handle(curlm, dlData->curlw->GetHandle());
		}
	}
	if (downloads.empty()) {
		LOG_DEBUG("Nothing to download!");
		CleanupDownloads(download, downloads);
		return true;
	}

	bool aborted = false;
	int running = 1;
	int last = -1;
	while (running > 0 && !aborted) {
		CURLMcode ret = CURLM_CALL_MULTI_PERFORM;
		while (ret == CURLM_CALL_MULTI_PERFORM) {
			ret = curl_multi_perform(curlm, &running);
		}
		if (ret == CURLM_OK) {
			//			showProcess(download, file);
			if (last != running) { // count of running downloads changed
				aborted = processMessages(curlm, downloads);
				last = running++;
			}
		} else {
			LOG_ERROR("curl_multi_perform_error: %d", ret);
			aborted = true;
		}

		fd_set rSet;
		fd_set wSet;
		fd_set eSet;

		FD_ZERO(&rSet);
		FD_ZERO(&wSet);
		FD_ZERO(&eSet);
		int count = 0;
		timeval t;
		t.tv_sec = 1;
		t.tv_usec = 0;
		curl_multi_fdset(curlm, &rSet, &wSet, &eSet, &count);
		// sleep for one sec / until something happened
		select(count + 1, &rSet, &wSet, &eSet, &t);
	}

	for (IDownload* download: download) {
		VerifySinglePieceDownload(*download);
	}

	LOG("\n");

	if (!aborted) {
		LOG_DEBUG("download complete");
	}
	CleanupDownloads(download, downloads);
	curl_multi_cleanup(curlm);
	return !aborted;
}
