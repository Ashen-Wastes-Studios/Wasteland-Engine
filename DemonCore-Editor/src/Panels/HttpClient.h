#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Wasteland
{

	using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

	struct HttpResult
	{
		bool Succeeded = false;
		long StatusCode = 0;
		std::string Body;
		std::string Error;
	};

	// Minimal HTTPS client.
	// Windows: WinHTTP (system library, no new dependencies).
	// Other platforms: stub that reports unsupported (engine targets Windows).
	class HttpClient
	{
	public:
		static HttpResult Get(const std::string &url, const HttpHeaders &headers = {});
		static HttpResult PostJson(const std::string &url, const std::string &jsonBody, const HttpHeaders &headers = {});

		// Blocking download. Streams to destPath (.part then rename).
		// Updates downloaded/total bytes, polls cancel per chunk, sets finished on exit.
		// errorOut is empty on success.
		static void DownloadFile(const std::string &url, const std::filesystem::path &destPath, const HttpHeaders &headers,
			std::atomic<uint64_t> &downloaded, std::atomic<uint64_t> &total,
			std::atomic<bool> &cancel, std::atomic<bool> &finished, std::string &errorOut);

		static std::string UrlEncode(const std::string &value);
	};

}
