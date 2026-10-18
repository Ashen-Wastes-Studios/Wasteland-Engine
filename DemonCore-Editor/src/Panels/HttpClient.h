#pragma once

// <atomic> for the download progress/cancel flags, <filesystem> for destPath,
// <functional> for the streaming callback, <utility> for the pair in HttpHeaders.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Wasteland
{

	// A vector of pairs rather than a map: HTTP permits the same header name more
	// than once (Set-Cookie being the usual case), and insertion order is part of the
	// wire format. A map would silently drop duplicates and reorder.
	using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

	// A value type returned by every non-streaming call, so a caller gets status,
	// body and error together rather than through out-parameters or an exception.
	//
	// DEFECT (ambiguous failure): Succeeded and StatusCode are independent. Checking
	// only StatusCode treats a transport failure (0) differently from an HTTP error;
	// checking only Succeeded discards a 404 body that says why. Both have to be
	// read, which the struct does not enforce. Not fixed; comments only.
	struct HttpResult
	{
		bool Succeeded = false;
		// The engine is a Windows target and this client is built on WinHTTP, a system
		// library -- so adding HTTPS costs no new dependency. That is the whole reason
		// this exists rather than a library.
		long StatusCode = 0;
		std::string Body;
		std::string Error;
	};

	// Minimal HTTPS client.
	// Windows: WinHTTP (system library, no new dependencies).
	// Other platforms: stub that reports unsupported (engine targets Windows).
	// Four static methods and no instance state, no constructor, no destructor: the
	// class exists only as a namespace. It could have been a namespace; a class lets
	// all four share one header and one translation unit.
	class HttpClient
	{
	public:
		// The two blocking calls. Both DEFAULT headers to an empty vector, so the
		// common case needs no temporary at the call site -- and the default is a value
		// rather than a reference, so there is no lifetime trap.
		static HttpResult Get(const std::string &url, const HttpHeaders &headers = {});
		static HttpResult PostJson(const std::string &url, const std::string &jsonBody, const HttpHeaders &headers = {});

		// Streaming for server-sent-event style endpoints: a chat completion with
		// "stream":true returns a sequence of partial results, and a caller that waited
		// for the whole body would show nothing until generation finished.
		//
		// onChunk is std::function rather than a raw function pointer, so a caller can
		// capture state -- the panel it is updating, an abort flag -- without a global.
		// The cost is a heap allocation per call site and an indirect call per chunk.
		// Streaming POST for SSE-style endpoints (chat completions with
		// "stream":true). Calls onChunk per received block until the server
		// closes the stream. Returns "" on success, else an error message.
		// statusOut receives the HTTP status (0 if no response). No redirect
		// handling — chat endpoints don't redirect.
		// DEFECT (no cancel): PostStream takes no cancellation flag, unlike
		// DownloadFile below which polls one per chunk. A chat stream the server never
		// closes therefore blocks the calling thread indefinitely, with no way to abort.
		// For an editor panel that is a hung UI rather than a crash, but it is an
		// asymmetry worth knowing. Not fixed; comments only.
		static std::string PostStream(const std::string &url, const std::string &jsonBody, const HttpHeaders &headers,
			std::function<void(const char *data, size_t len)> onChunk, long &statusOut);

		// The download path writes to a temporary and renames on success, so a partial
		// file never appears at destPath. That matters for a model download measured in
		// gigabytes: an interrupted transfer leaves a .part file rather than a truncated
		// model that looks complete.
		// Blocking download. Streams to destPath (.part then rename).
		// Updates downloaded/total bytes, polls cancel per chunk, sets finished on exit.
		// errorOut is empty on success.
		// Progress and cancel are passed as std::atomic& and updated BY THE CALLEE.
		// That is why they are atomics rather than callbacks: the UI thread polls while
		// the download thread writes, and no locking or marshalling is needed.
		//
		// finished is set on exit including on failure, so a progress bar bound to it
		// stops spinning either way; errorOut is the discriminator.
		static void DownloadFile(const std::string &url, const std::filesystem::path &destPath, const HttpHeaders &headers,
			std::atomic<uint64_t> &downloaded, std::atomic<uint64_t> &total,
			std::atomic<bool> &cancel, std::atomic<bool> &finished, std::string &errorOut);

		// URL encoding is public rather than internal because callers assemble query
		// strings themselves -- a HuggingFace model path with a slash in it has to be
		// encoded before it goes into a URL.
		static std::string UrlEncode(const std::string &value);
	};

}
