/* This file is part of pr-downloader (GPL v2 or later), see the LICENSE file */

#include "GitRapidResolver.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include <json/reader.h>

#include "Downloader/CurlWrapper.h"
#include "Downloader/IDownloader.h"
#include "FileSystem/FileSystem.h"
#include "Logger.h"
#include "Util.h"

namespace {

static size_t WriteToString(const void* data, size_t size, size_t nmemb,
			    void* userData)
{
	if (IDownloader::AbortDownloads()) {
		return 0;
	}

	std::string* output = static_cast<std::string*>(userData);
	const size_t bytes = size * nmemb;
	output->append(static_cast<const char*>(data), bytes);
	return bytes;
}

static std::string ReadFile(const std::string& path)
{
	std::string contents;
	if (!fileSystem->fileExists(path)) {
		return contents;
	}

	FILE* f = fileSystem->propen(path, "rb");
	if (f == nullptr) {
		return contents;
	}

	char buffer[4096];
	while (!std::feof(f)) {
		const size_t read = std::fread(buffer, 1, sizeof(buffer), f);
		if (read > 0) {
			contents.append(buffer, read);
		}
		if (std::ferror(f)) {
			contents.clear();
			break;
		}
	}

	std::fclose(f);
	return contents;
}

static bool WriteFile(const std::string& path, const std::string& contents)
{
	const std::string dir = CFileSystem::DirName(path);
	if (!fileSystem->createSubdirs(dir)) {
		return false;
	}

	FILE* f = fileSystem->propen(path, "wb");
	if (f == nullptr) {
		return false;
	}

	const size_t written = std::fwrite(contents.data(), 1, contents.size(), f);
	const bool ok = (written == contents.size()) && (std::fclose(f) == 0);
	return ok;
}

static std::string BuildCachePath(const std::string& manifestUrl)
{
	std::string urlPath;
	if (!urlToPath(manifestUrl, urlPath)) {
		urlPath = "manifest";
	}
	return fileSystem->getSpringDir() + PATH_DELIMITER + "rapid" +
	       PATH_DELIMITER + "git" + PATH_DELIMITER + urlPath;
}

static std::string ReplaceAll(std::string value, const std::string& from,
			      const std::string& to)
{
	if (from.empty()) {
		return value;
	}

	size_t pos = 0;
	while ((pos = value.find(from, pos)) != std::string::npos) {
		value.replace(pos, from.size(), to);
		pos += to.size();
	}
	return value;
}

static std::string ApplyTemplate(const std::string& templ,
				 const std::map<std::string, std::string>& values)
{
	std::string out = templ;
	for (const auto& pair : values) {
		out = ReplaceAll(out, "{" + pair.first + "}", pair.second);
	}
	return out;
}

static std::string NormalizePath(std::string path)
{
	for (char& c : path) {
		if (c == '\\') {
			c = '/';
		}
	}
	while (!path.empty() && (path.front() == '/' || path.front() == '.')) {
		path.erase(path.begin());
	}
	return path;
}

static std::string NormalizePrefix(std::string prefix)
{
	prefix = NormalizePath(prefix);
	while (!prefix.empty() && prefix.back() == '/') {
		prefix.pop_back();
	}
	return prefix;
}

static bool HasPrefix(const std::string& value, const std::string& prefix)
{
	if (prefix.empty()) {
		return true;
	}
	if (value.size() < prefix.size()) {
		return false;
	}
	if (value.compare(0, prefix.size(), prefix) != 0) {
		return false;
	}
	return value.size() == prefix.size() || value[prefix.size()] == '/';
}

static std::vector<std::string> ParseDepends(const Json::Value& entry)
{
	std::vector<std::string> depends;
	if (!entry["depends"].isArray()) {
		return depends;
	}

	for (Json::Value::ArrayIndex i = 0; i < entry["depends"].size(); ++i) {
		if (!entry["depends"][i].isString()) {
			continue;
		}
		depends.push_back(entry["depends"][i].asString());
	}
	return depends;
}

static std::string JsonStringOrDefault(const Json::Value& object,
				       const std::string& key,
				       const std::string& fallback = "")
{
	if (!object.isObject() || !object[key].isString()) {
		return fallback;
	}
	return object[key].asString();
}

} // namespace

GitRapidResolver::GitRapidResolver(int timeoutSeconds)
{
	if (timeoutSeconds > 0) {
		m_timeoutSeconds = timeoutSeconds;
	}
}

bool GitRapidResolver::DownloadText(const std::string& url, std::string& output,
				    std::string& errorOut) const
{
	output.clear();

	CurlWrapper curl;
	curl_easy_setopt(curl.GetHandle(), CURLOPT_URL,
			 CurlWrapper::escapeUrl(url).c_str());
	curl_easy_setopt(curl.GetHandle(), CURLOPT_WRITEFUNCTION, WriteToString);
	curl_easy_setopt(curl.GetHandle(), CURLOPT_WRITEDATA, &output);
	curl_easy_setopt(curl.GetHandle(), CURLOPT_TIMEOUT, m_timeoutSeconds);

	const CURLcode code = curl_easy_perform(curl.GetHandle());
	if (code != CURLE_OK) {
		errorOut = curl_easy_strerror(code);
		if (!curl.GetError().empty()) {
			errorOut += " (" + curl.GetError() + ")";
		}
		return false;
	}

	return true;
}

bool GitRapidResolver::LoadManifest(const std::string& manifestUrl,
				    int manifestTtlSeconds,
				    std::string& manifestJson,
				    std::string& errorOut) const
{
	const std::string cachePath = BuildCachePath(manifestUrl);
	if (fileSystem->fileExists(cachePath) &&
	    !fileSystem->isOlder(cachePath, std::max(0, manifestTtlSeconds))) {
		manifestJson = ReadFile(cachePath);
		if (!manifestJson.empty()) {
			return true;
		}
	}

	if (!DownloadText(manifestUrl, manifestJson, errorOut)) {
		if (fileSystem->fileExists(cachePath)) {
			manifestJson = ReadFile(cachePath);
			if (!manifestJson.empty()) {
				LOG_WARN("Git rapid: manifest download failed, using stale cache %s",
					 cachePath.c_str());
				return true;
			}
		}
		return false;
	}

	if (!WriteFile(cachePath, manifestJson)) {
		LOG_WARN("Git rapid: couldn't write manifest cache %s", cachePath.c_str());
	}
	return true;
}

bool GitRapidResolver::ParseTag(const std::string& manifestJson,
				const std::string& tag,
				GitRapidVersionInfo& versionOut,
				std::string& errorOut) const
{
	Json::Value root;
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	std::string errs;
	const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	if (!reader || !reader->parse(manifestJson.data(), manifestJson.data() + manifestJson.size(), &root, &errs)) {
		errorOut = "manifest parse error: " + errs;
		return false;
	}
	if (!root.isObject()) {
		errorOut = "manifest root must be a JSON object";
		return false;
	}

	const Json::Value* tagsNode = nullptr;
	if (root["tags"].isObject()) {
		tagsNode = &root["tags"];
	} else {
		tagsNode = &root;
	}
	if (!tagsNode->isObject()) {
		errorOut = "manifest tags must be a JSON object";
		return false;
	}

	if (!tagsNode->isMember(tag)) {
		errorOut = "tag not found in manifest: " + tag;
		return false;
	}
	const Json::Value& entry = (*tagsNode)[tag];
	if (!entry.isObject()) {
		errorOut = "manifest tag entry must be an object: " + tag;
		return false;
	}

	versionOut = GitRapidVersionInfo();
	versionOut.tag = tag;
	versionOut.commit = JsonStringOrDefault(entry, "commit");
	if (versionOut.commit.empty()) {
		errorOut = "manifest tag entry missing commit: " + tag;
		return false;
	}

	versionOut.displayName = JsonStringOrDefault(entry, "mod_name");
	if (versionOut.displayName.empty()) {
		versionOut.displayName = JsonStringOrDefault(entry, "name", tag);
	}
	versionOut.depends = ParseDepends(entry);

	std::string treeUrlTemplate = JsonStringOrDefault(entry, "tree_url");
	std::string blobUrlTemplate = JsonStringOrDefault(entry, "blob_url");
	const std::string repoApi =
	    JsonStringOrDefault(entry, "repo_api", JsonStringOrDefault(root, "repo_api"));
	const std::string rootPath =
	    NormalizePrefix(JsonStringOrDefault(entry, "path_prefix",
					       JsonStringOrDefault(root, "path_prefix")));

	if (treeUrlTemplate.empty() && !repoApi.empty()) {
		treeUrlTemplate = repoApi + "/git/trees/{commit}?recursive=1";
	}
	if (blobUrlTemplate.empty()) {
		blobUrlTemplate = JsonStringOrDefault(root, "blob_url");
	}
	if (treeUrlTemplate.empty()) {
		errorOut = "manifest tag entry missing tree_url/repo_api: " + tag;
		return false;
	}

	const std::map<std::string, std::string> commonValues = {
	    {"tag", tag},
	    {"commit", versionOut.commit},
	};
	const std::string treeUrl = ApplyTemplate(treeUrlTemplate, commonValues);

	std::string treeJson;
	if (!DownloadText(treeUrl, treeJson, errorOut)) {
		errorOut = "tree download failed: " + errorOut;
		return false;
	}

	Json::Value treeRoot;
	std::string treeErrs;
	const std::unique_ptr<Json::CharReader> treeReader(builder.newCharReader());
	if (!treeReader || !treeReader->parse(treeJson.data(), treeJson.data() + treeJson.size(), &treeRoot, &treeErrs)) {
		errorOut = "tree parse error: " + treeErrs;
		return false;
	}

	const Json::Value& nodes = treeRoot["tree"].isArray() ? treeRoot["tree"] :
				    treeRoot["files"];
	if (!nodes.isArray()) {
		errorOut = "tree response must contain array field tree/files";
		return false;
	}
	if (treeRoot["truncated"].isBool() && treeRoot["truncated"].asBool()) {
		errorOut = "tree response is truncated";
		return false;
	}

	for (Json::Value::ArrayIndex i = 0; i < nodes.size(); ++i) {
		const Json::Value& node = nodes[i];
		if (!node.isObject()) {
			continue;
		}

		const std::string type = JsonStringOrDefault(node, "type");
		if (!type.empty() && type != "blob" && type != "file") {
			continue;
		}

		std::string path = NormalizePath(JsonStringOrDefault(node, "path"));
		if (path.empty()) {
			continue;
		}
		if (!HasPrefix(path, rootPath)) {
			continue;
		}
		if (!rootPath.empty()) {
			path.erase(0, rootPath.size());
			if (!path.empty() && path[0] == '/') {
				path.erase(path.begin());
			}
			if (path.empty()) {
				continue;
			}
		}

		const std::string blobSha = JsonStringOrDefault(node, "sha");
		std::string blobUrl = JsonStringOrDefault(node, "url");
		if (!blobUrlTemplate.empty()) {
			const std::map<std::string, std::string> nodeValues = {
			    {"tag", tag},
			    {"commit", versionOut.commit},
			    {"path", path},
			    {"sha", blobSha},
			    {"blob_sha", blobSha},
			};
			blobUrl = ApplyTemplate(blobUrlTemplate, nodeValues);
		}
		if (blobUrl.empty()) {
			continue;
		}

		GitRapidFileInfo fileInfo;
		fileInfo.path = path;
		fileInfo.blobSha = blobSha;
		fileInfo.blobUrl = blobUrl;
		if (node["size"].isInt()) {
			fileInfo.size = node["size"].asInt();
		}

		versionOut.files.push_back(fileInfo);
	}

	if (versionOut.files.empty()) {
		errorOut = "no blob files matched in tree response";
		return false;
	}

	return true;
}

bool GitRapidResolver::Resolve(const std::string& manifestUrl,
			       int manifestTtlSeconds, const std::string& tag,
			       GitRapidVersionInfo& versionOut,
			       std::string& errorOut) const
{
	if (manifestUrl.empty()) {
		errorOut = "manifest url is empty";
		return false;
	}
	if (tag.empty() || tag == "*" || tag.find(':') == std::string::npos) {
		errorOut = "tag is not rapid-like";
		return false;
	}

	std::string manifestJson;
	if (!LoadManifest(manifestUrl, manifestTtlSeconds, manifestJson, errorOut)) {
		errorOut = "manifest load failed: " + errorOut;
		return false;
	}

	return ParseTag(manifestJson, tag, versionOut, errorOut);
}
