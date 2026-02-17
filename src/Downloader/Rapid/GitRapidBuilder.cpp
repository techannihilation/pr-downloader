/* This file is part of pr-downloader (GPL v2 or later), see the LICENSE file */

#include "GitRapidBuilder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <list>
#include <string>
#include <vector>

#include <json/reader.h>
#include <zlib.h>

#include "Downloader/CurlWrapper.h"
#include "Downloader/IDownloader.h"
#include "FileSystem/FileData.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/HashMD5.h"
#include "Logger.h"
#include "Util.h"
#include "lib/base64/base64.h"

namespace {

struct SdpEntry
{
	std::string name;
	std::array<unsigned char, 16> md5;
	uint32_t crc32 = 0;
	uint32_t size = 0;
};

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

static std::string NormalizeArchivePath(std::string path)
{
	for (char& c : path) {
		if (c == '\\') {
			c = '/';
		}
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	while (!path.empty() && (path.front() == '/' || path.front() == '.')) {
		path.erase(path.begin());
	}
	return path;
}

static std::string SanitizeBase64(const std::string& input)
{
	std::string cleaned;
	cleaned.reserve(input.size());
	for (unsigned char c : input) {
		if (!std::isspace(c)) {
			cleaned.push_back(static_cast<char>(c));
		}
	}
	return cleaned;
}

static std::string PackLE32(uint32_t value)
{
	std::string packed(4, '\0');
	packed[0] = static_cast<char>(value & 0xFF);
	packed[1] = static_cast<char>((value >> 8) & 0xFF);
	packed[2] = static_cast<char>((value >> 16) & 0xFF);
	packed[3] = static_cast<char>((value >> 24) & 0xFF);
	return packed;
}

static bool WriteGzipFile(const std::string& path, const std::string& data)
{
	if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
		return false;
	}

	const std::string dir = CFileSystem::DirName(path);
	if (!fileSystem->createSubdirs(dir)) {
		return false;
	}

	gzFile out = gzopen(path.c_str(), "wb");
	if (out == nullptr) {
		return false;
	}

	const int expected = static_cast<int>(data.size());
	const int written = gzwrite(out, data.data(), expected);
	const int closeRes = gzclose(out);
	return written == expected && closeRes == Z_OK;
}

static bool VerifyPoolFile(const std::string& path,
			   const std::array<unsigned char, 16>& md5,
			   uint32_t size)
{
	if (!fileSystem->fileExists(path)) {
		return false;
	}

	FileData fileData;
	std::memcpy(fileData.md5, md5.data(), md5.size());
	fileData.size = size;
	return fileSystem->fileIsValid(&fileData, path);
}

static std::string BuildTmpSdpPath(const std::string& tag, const std::string& commit)
{
	std::string safeTag = tag;
	for (char& c : safeTag) {
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
			c = '_';
		}
	}
	return fileSystem->getSpringDir() + PATH_DELIMITER + "packages" +
	       PATH_DELIMITER + "gitrapid-" + safeTag + "-" + commit.substr(0, 12) + ".tmp";
}

} // namespace

GitRapidBuilder::GitRapidBuilder(int timeoutSeconds)
{
	if (timeoutSeconds > 0) {
		m_timeoutSeconds = timeoutSeconds;
	}
}

bool GitRapidBuilder::DownloadBlob(const GitRapidFileInfo& file,
				   std::string& dataOut,
				   std::string& errorOut) const
{
	dataOut.clear();

	CurlWrapper curl;
	curl_easy_setopt(curl.GetHandle(), CURLOPT_URL,
			 CurlWrapper::escapeUrl(file.blobUrl).c_str());
	curl_easy_setopt(curl.GetHandle(), CURLOPT_WRITEFUNCTION, WriteToString);
	curl_easy_setopt(curl.GetHandle(), CURLOPT_WRITEDATA, &dataOut);
	curl_easy_setopt(curl.GetHandle(), CURLOPT_TIMEOUT, m_timeoutSeconds);

	const CURLcode code = curl_easy_perform(curl.GetHandle());
	if (code != CURLE_OK) {
		errorOut = curl_easy_strerror(code);
		if (!curl.GetError().empty()) {
			errorOut += " (" + curl.GetError() + ")";
		}
		return false;
	}

	Json::Value root;
	Json::Reader reader;
	if (!reader.parse(dataOut, root)) {
		// Raw payload mode.
		return true;
	}

	if (!root.isObject()) {
		return true;
	}

	if (root["message"].isString() && !root["content"].isString()) {
		errorOut = root["message"].asString();
		return false;
	}

	if (!root["content"].isString()) {
		return true;
	}

	const std::string encoding =
	    root["encoding"].isString() ? root["encoding"].asString() : "base64";
	const std::string content = root["content"].asString();
	if (encoding == "base64") {
		dataOut = base64_decode(SanitizeBase64(content));
		return true;
	}
	if (encoding == "utf-8") {
		dataOut = content;
		return true;
	}

	errorOut = "unsupported blob encoding: " + encoding;
	return false;
}

bool GitRapidBuilder::Build(const GitRapidVersionInfo& version,
			    GitRapidBuildResult& resultOut,
			    std::string& errorOut) const
{
	if (version.commit.empty()) {
		errorOut = "git rapid version missing commit";
		return false;
	}
	if (version.files.empty()) {
		errorOut = "git rapid version has no files";
		return false;
	}

	const std::string packagesDir =
	    fileSystem->getSpringDir() + PATH_DELIMITER + "packages";
	if (!fileSystem->createSubdirs(packagesDir)) {
		errorOut = "failed to create packages directory";
		return false;
	}

	std::vector<SdpEntry> entries;
	entries.reserve(version.files.size());

	for (const GitRapidFileInfo& file : version.files) {
		if (file.blobUrl.empty()) {
			errorOut = "blob url missing for path: " + file.path;
			return false;
		}

		std::string path = NormalizeArchivePath(file.path);
		if (path.empty()) {
			continue;
		}

		std::string blobData;
		if (!DownloadBlob(file, blobData, errorOut)) {
			errorOut = "blob download failed for " + path + ": " + errorOut;
			return false;
		}

		if (blobData.size() > std::numeric_limits<uint32_t>::max()) {
			errorOut = "blob too large for sdp entry: " + path;
			return false;
		}

		HashMD5 md5;
		md5.Init();
		if (blobData.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
			errorOut = "blob too large for hash calculation: " + path;
			return false;
		}
		md5.Update(blobData.data(), static_cast<int>(blobData.size()));
		md5.Final();
		std::array<unsigned char, 16> md5Bytes = {};
		for (size_t i = 0; i < md5Bytes.size(); ++i) {
			md5Bytes[i] = md5.get(i);
		}

		const std::string md5Hex = md5.toString();
		const std::string poolPath = fileSystem->getPoolFilename(md5Hex);
		const uint32_t size = static_cast<uint32_t>(blobData.size());

		if (!VerifyPoolFile(poolPath, md5Bytes, size)) {
			fileSystem->removeFile(poolPath);
			if (!WriteGzipFile(poolPath, blobData)) {
				errorOut = "failed to write pool file: " + poolPath;
				return false;
			}
		}

		const uLong crc = ::crc32(0L, reinterpret_cast<const Bytef*>(blobData.data()),
					  static_cast<uInt>(blobData.size()));
		SdpEntry entry;
		entry.name = path;
		entry.md5 = md5Bytes;
		entry.crc32 = static_cast<uint32_t>(crc);
		entry.size = size;
		entries.push_back(entry);
	}

	if (entries.empty()) {
		errorOut = "no valid entries produced from git tree";
		return false;
	}

	std::sort(entries.begin(), entries.end(),
		  [](const SdpEntry& a, const SdpEntry& b) { return a.name < b.name; });

	const std::string tmpPath = BuildTmpSdpPath(version.tag, version.commit);
	fileSystem->removeFile(tmpPath);

	gzFile sdp = gzopen(tmpPath.c_str(), "wb");
	if (sdp == nullptr) {
		errorOut = "failed to create temporary sdp file: " + tmpPath;
		return false;
	}

	for (const SdpEntry& entry : entries) {
		if (entry.name.size() > 255) {
			gzclose(sdp);
			fileSystem->removeFile(tmpPath);
			errorOut = "sdp path too long (>255): " + entry.name;
			return false;
		}

		const unsigned char len = static_cast<unsigned char>(entry.name.size());
		if (gzwrite(sdp, &len, 1) != 1) {
			gzclose(sdp);
			fileSystem->removeFile(tmpPath);
			errorOut = "failed writing sdp entry length";
			return false;
		}
		if (gzwrite(sdp, entry.name.data(), entry.name.size()) !=
		    static_cast<int>(entry.name.size())) {
			gzclose(sdp);
			fileSystem->removeFile(tmpPath);
			errorOut = "failed writing sdp entry name";
			return false;
		}
		if (gzwrite(sdp, entry.md5.data(), entry.md5.size()) !=
		    static_cast<int>(entry.md5.size())) {
			gzclose(sdp);
			fileSystem->removeFile(tmpPath);
			errorOut = "failed writing sdp entry md5";
			return false;
		}
		const std::string crcBytes = PackLE32(entry.crc32);
		const std::string sizeBytes = PackLE32(entry.size);
		if (gzwrite(sdp, crcBytes.data(), crcBytes.size()) !=
		    static_cast<int>(crcBytes.size()) ||
		    gzwrite(sdp, sizeBytes.data(), sizeBytes.size()) !=
			static_cast<int>(sizeBytes.size())) {
			gzclose(sdp);
			fileSystem->removeFile(tmpPath);
			errorOut = "failed writing sdp entry metadata";
			return false;
		}
	}

	if (gzclose(sdp) != Z_OK) {
		fileSystem->removeFile(tmpPath);
		errorOut = "failed closing temporary sdp file";
		return false;
	}

	HashMD5 sdpMd5;
	sdpMd5.Init();
	for (const SdpEntry& entry : entries) {
		HashMD5 nameMd5;
		nameMd5.Init();
		nameMd5.Update(entry.name.data(), entry.name.size());
		nameMd5.Final();
		sdpMd5.Update(reinterpret_cast<const char*>(nameMd5.Data()),
			      nameMd5.getSize());
		sdpMd5.Update(reinterpret_cast<const char*>(entry.md5.data()),
			      entry.md5.size());
	}
	sdpMd5.Final();

	resultOut.sdpMd5 = sdpMd5.toString();
	const std::string finalPath = fileSystem->getSpringDir() + PATH_DELIMITER +
				      "packages" + PATH_DELIMITER +
				      resultOut.sdpMd5 + ".sdp";
	if (!fileSystem->fileExists(finalPath)) {
		if (!fileSystem->Rename(tmpPath, finalPath)) {
			fileSystem->removeFile(tmpPath);
			errorOut = "failed to move temporary sdp into packages: " + finalPath;
			return false;
		}
	} else {
		fileSystem->removeFile(tmpPath);
	}

	std::list<FileData> parsed;
	if (!fileSystem->parseSdp(finalPath, parsed)) {
		errorOut = "generated sdp failed validation: " + finalPath;
		return false;
	}
	return true;
}
