// PCH first, before Windows.h: this is where the project's default
// include order and any compile-wide defines come from.
#include "wlpch.h"
// HttpResult, HttpHeaders and the four public entry points. Note that
// UrlEncode is declared OUTSIDE the platform guard below, so it is
// available on every platform even though the client is not.
#include "HttpClient.h"

// The entire client is WinHTTP and therefore Windows-only. The #else
// branch near the end of this file supplies stubs that report
// unsupported rather than failing to link.
#ifdef WL_PLATFORM_WINDOWS
// NOMINMAX stops <windows.h> defining min/max macros, which would
// otherwise break every std::max/std::min in the editor.
#ifndef NOMINMAX
// Guards against a second definition if the PCH (or another header)
// already set it -- both of these are legitimately set by both.
#define NOMINMAX
#endif
// WIN32_LEAN_AND_MEAN excludes the Winsock, cryptography and other
// headers this file has no use for -- a build-time saving.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// HINTERNET handles and the WinHTTP API surface.
#include <Windows.h>
#include <winhttp.h>
// Links winhttp.lib from this object file, so nothing has to be added
// to the .vcxproj for the client to resolve.
#pragma comment(lib, "winhttp.lib")
#endif

// std::isalnum in UrlEncode's unreserved-character test. The cast to
// unsigned char at the call site is what keeps that predicate defined
// for high bytes.
#include <cctype>
// snprintf in LastErrorString. <cstdio> rather than <stdio.h> so the
// names are in namespace std (the code calls the unqualified name,
// which MSVC also provides).
#include <cstdio>
// std::ofstream for streaming a download to a .part file.
#include <fstream>

// Namespace Wasteland, matching the editor's other panels.
namespace Wasteland
{

	// Percent-encodes a string for use in a URL query component.
	std::string HttpClient::UrlEncode(const std::string &value)
	{
		// Uppercase hex digits, as both RFC 3986 and every real server accept.
		static const char *hex = "0123456789ABCDEF";
		// Accumulator, returned by value below. Built by appending rather
		// than by a fixed-size buffer because the encoded length is unknown
		// up front.
		std::string out;
		// reserve up front: the encoded form is never shorter than the input,
		// so this is a lower bound and usually avoids reallocating entirely.
		out.reserve(value.size());
		// Iterating as unsigned char is deliberate -- see the isalnum note
		// below.
		for (unsigned char c : value)
		{
			// RFC 3986 unreserved set: alphanumerics plus -._~. Anything outside
			// it gets percent-encoded, which is the conservative choice -- it
			// cannot produce a malformed URL, at the cost of over-encoding.
			if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
				out += (char)c;
			// Space becomes %20 rather than '+'. '+' only means space in the
			// application/x-www-form-urlencoded body encoding, not in a path or
			// query component, so %20 is the correct choice here.
			else if (c == ' ')
				out += "%20";
			else
			{
				out += '%';
				// High nibble then low nibble, each a separate index into the same
				// 16-character table. No bounds concern: c is at most 0xFF.
				out += hex[c >> 4];
				out += hex[c & 0xF];
			}
		}
		// Value return: the caller gets the encoded string and the local is
		// freed. No move is possible here because `out` was returned
		// directly, not copied.
		return out;
	}

#ifdef WL_PLATFORM_WINDOWS

	// Internal linkage -- none of these helpers are part of HttpClient's
	// API, and their names should not collide across the editor.
	namespace
	{
		// UTF-8 to UTF-16. WinHTTP's wide API takes wchar_t*, and the editor
		// stores everything as UTF-8 std::string.
		std::wstring ToWide(const std::string &s)
		{
			// Empty in, empty out -- MultiByteToWideChar with a zero length is
			// legal but the std::wstring(0, 0) below would then be indexed at [0].
			if (s.empty())
				return L"";
			// Two-call idiom: ask for the required size (passing a null output
			// buffer), then allocate exactly that many wchar_t. The return value
			// is not checked for failure -- on a malformed sequence it is 0, the
			// string below is empty, and the second call is simply a no-op.
			int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
			// Exactly n wide chars, pre-sized so the second call below writes
			// into an already-correct buffer rather than growing one.
			std::wstring w(n, 0);
			// Second pass fills the buffer. Its return value is deliberately
			// ignored -- n came from the identical first call, so a failure
			// here would mean the same call just succeeded with a different
			// size.
			MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
			return w;
		}

		// Turns a WinHTTP error code into a message a user can act on.
		std::string LastErrorString(const char *what, DWORD code)
		{
			// No hint for unrecognized codes, which leaves the message as the
			// bare "<what> failed (code N)" form.
			const char *hint = "";
			// The three codes that actually show up when talking to a local model
			// server, with the fix spelled out. These are WINHTTP_ERROR_* values
			// (12002 = ERROR_WINHTTP_TIMEOUT, 12029 = ERROR_WINHTTP_CANNOT_CONNECT,
			// 12007 = ERROR_WINHTTP_NAME_NOT_RESOLVED).
			//
			// Hardcoded decimal literals rather than the named constants:
			// winhttp.h defines these only if WINHTTP_ENABLE_ERROR_CODE_TEXT is
			// not suppressing them, and a literal cannot fail to compile.
			if (code == 12002)
				hint = " — timed out. Local server? Make sure LM Studio is running, a model is loaded, and its server is Started; verify the base URL/port";
			else if (code == 12029)
				hint = " — cannot connect. Is the server running? Check the base URL/port";
			else if (code == 12007)
				hint = " — host name not resolved. Check the base URL";
			// snprintf rather than sprintf: the hint strings are long and
			// user-visible, and 320 bytes is a real limit here.
			char buf[320];
			// Cast to unsigned long because DWORD is unsigned and %lu expects a
			// signed long -- without the cast this is a format/arg type mismatch
			// that MSVC flags at compile time and other toolchains miss.
			snprintf(buf, sizeof(buf), "%s failed (code %lu)%s", what, (unsigned long)code, hint);
			// Materialized from the stack buffer; the caller gets a std::string
			// and buf goes out of scope.
			return std::string(buf);
		}

		// RAII for the three HINTERNET handles. This is the main reason the
		// early `return result;` paths below are safe at all -- every one of
		// them would otherwise leak a session, a connect and a request.
		struct WinHttpHandles
		{
			// Session (one per Perform call), connection, then request. Closed in
			// the reverse order in the destructor.
			HINTERNET Session = nullptr, Connect = nullptr, Request = nullptr;
			// The destructor is the whole point of this struct. Every WinHTTP
			// handle in this file is stored in one, so no early return can leak
			// one.
			~WinHttpHandles()
			{
				// Reverse of acquisition order. WinHTTP requires the child handle be
				// closed before its parent, and closing the session last would strand
				// the other two.
				if (Request)
					// Null-checked rather than unconditional: WinHttpCloseHandle on a
					// null handle is undefined, and a partially-built chain has nulls.
					WinHttpCloseHandle(Request);
				if (Connect)
					WinHttpCloseHandle(Connect);
				if (Session)
					WinHttpCloseHandle(Session);
			}
		};

		// Splits "https://host:port/path?query" into components for WinHTTP.
		// Splits "https://host:port/path?query" into components for WinHTTP.
		bool CrackUrl(const std::string &url, std::wstring &host, INTERNET_PORT &port, std::wstring &path, bool &secure)
		{
			// All four length fields set to (DWORD)-1 means "fill these in", so
			// WinHttpCrackUrl allocates nothing and the caller frees nothing.
			URL_COMPONENTSW comp = {};
			// dwStructSize is mandatory: WinHttpCrackUrl checks it against the
			// running binary's layout and fails on a mismatch. This is the
			// documented versioning mechanism for the struct.
			comp.dwStructSize = sizeof(comp);
			comp.dwHostNameLength = (DWORD)-1;
			comp.dwUrlPathLength = (DWORD)-1;
			comp.dwExtraInfoLength = (DWORD)-1;
			comp.dwSchemeLength = (DWORD)-1;
			// WinHttpCrackUrl is a macro that dispatches to A or W by UNICODE
			// definition; this is the wide one.
			std::wstring wurl = ToWide(url);
			// A parse failure here means the caller passed something that is not
			// a URL at all -- an empty string, or a base URL missing its scheme.
			if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &comp))
				return false;
			// The length is a count of CHARACTERS for the wide variants, so this
			// assign copies exactly the hostname and nothing more.
			host.assign(comp.lpszHostName, comp.dwHostNameLength);
			port = comp.nPort;
			// secure is derived from the scheme, so http:// silently downgrades to
			// a plaintext request with no error -- worth knowing, since the base
			// URL is user-editable in the AI panel.
			secure = (comp.nScheme == INTERNET_SCHEME_HTTPS);
			// The path is rebuilt from the url-path and extra-info pieces so the
			// query string survives.
			std::wstring p;
			// A URL like "http://host" has no path component; WinHTTP needs
			// "/" rather than an empty string.
			if (comp.dwUrlPathLength > 0)
				p.assign(comp.lpszUrlPath, comp.dwUrlPathLength);
			// A URL with no path component still needs "/" -- WinHttpOpenRequest
			// with an empty path is not well-formed.
			else
				p = L"/";
			if (comp.dwExtraInfoLength > 0)
				p.append(comp.lpszExtraInfo, comp.dwExtraInfoLength);
			path = p;
			return true;
		}

		// Reads the numeric status code. WINHTTP_QUERY_FLAG_NUMBER is what
		// makes this return an integer rather than a string.
		bool ReadStatus(HINTERNET hRequest, long &status)
		{
			// size must be initialized to the buffer size going IN, which is how
			// WinHTTP knows it may write a DWORD.
			DWORD code = 0, size = sizeof(code);
			// WINHTTP_HEADER_NAME_BY_INDEX with WINHTTP_QUERY_STATUS_CODE: index
			// 0 of the status pseudo-headers. The alternative form takes no index
			// argument.
			if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX))
				return false;
			status = (long)code;
			return true;
		}

		bool ReadHeader(HINTERNET hRequest, DWORD query, std::wstring &out)
		{
			// size in, buffer pointer out -- the WinHTTP two-call header idiom.
			DWORD size = 0;
			// Two-call idiom again: first pass with a null buffer to learn the
			// required byte count. The RETURN VALUE IS IGNORED here -- the
			// function relies on GetLastError() == ERROR_INSUFFICIENT_BUFFER to
			// distinguish "ask for the size" from "no such header". Correct for
			// WinHTTP, but it is the pattern to be careful with.
			WinHttpQueryHeaders(hRequest, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
				return false;
			// size is in BYTES; divide by sizeof(wchar_t) for the character count.
			// The +1 for the null terminator is WinHTTP's convention, handled by
			// the pop_back below.
			std::wstring buf(size / sizeof(wchar_t), 0);
			if (!WinHttpQueryHeaders(hRequest, query, WINHTTP_HEADER_NAME_BY_INDEX, &buf[0], &size, WINHTTP_NO_HEADER_INDEX))
				return false;
			// size includes null terminator
			// Drop the trailing null. WinHTTP counts it in the size, so the
			// buffer is one character longer than the value.
			if (!buf.empty() && buf.back() == L'\0')
				buf.pop_back();
			// Assigned only on the success path; on failure the caller's
			// out-parameter is left untouched.
			out = buf;
			return true;
		}

		// UTF-16 back to UTF-8, for handing a header value to the caller.
		std::string WideToUtf8(const std::wstring &w)
		{
			if (w.empty())
				return "";
			// Same two-call sizing pattern as ToWide. The return value is again
			// unchecked; a conversion failure yields n == 0 and an empty string.
			int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
			// Mirror of ToWide's buffer, sized in bytes this time.
			std::string s(n, 0);
			WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
			return s;
		}

		// 303 is included because it is in practice the status a model server
		// uses to bounce /v1/chat/completions to a trailing-slash form.
		// 307/308 preserve method and body, which is correct for them.
		bool IsRedirect(long status)
		{
			return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
		}

		// DEFECT: this struct is declared and never used anywhere in the file
		// -- not by Perform, not by any caller. All four members (including the
		// GotResponse flag commented as meaningful) are dead. It looks like
		// the remains of a transport-vs-HTTP distinction that was later
		// expressed directly on HttpResult.
		struct RequestResult
		{
			// These four fields encode the transport-vs-HTTP distinction the
			// caller actually wanted -- and none of them is ever read. The
			// struct is dead; see the note at its declaration.
			bool TransportOk = false;
			std::string TransportError;
			long Status = 0;
			bool GotResponse = false; // false when caller streams download and consumed body itself
		};

		// Core request with manual redirect following.
		// If downloadPath is non-empty the body streams to that file and Body
		// stays empty; otherwise the whole body is buffered into Body.
		// Core request with manual redirect following (max 5 hops).
		// If downloadPath is non-empty, the body streams to that file and Body stays empty.
		HttpResult Perform(const std::string &method, const std::string &startUrl, const HttpHeaders &headers,
			const std::string &body, const std::filesystem::path &downloadPath,
			std::atomic<uint64_t> *downloaded, std::atomic<uint64_t> *total, std::atomic<bool> *cancel)
		{
			// HttpResult starts Succeeded=false, so every early `return result`
			// below is a failure with only Error set.
			HttpResult result;
			// url/method/body/headers are the CURRENT values, reassigned on each
			// redirect. Copying them into locals is what lets the loop carry
			// redirect state forward without mutating the caller's arguments.
			std::string url = startUrl;
			// Copied, not referenced: a redirect rewrites these, and the
			// caller's arguments must come back unchanged.
			std::string currentMethod = method;
			std::string currentBody = body;
			HttpHeaders currentHeaders = headers;

			// Six attempts: the original request plus up to five redirects. The
			// comment above says "max 5 hops", and 303-downgrade plus host
			// change can each consume one.
			for (int hop = 0; hop < 6; hop++)
			{
				// Per-hop, not per-call: a redirect can change host, port, path and
				// scheme, so all four are re-derived every iteration.
				std::wstring host, path;
				INTERNET_PORT port = 0;
				bool secure = false;
				// A URL that cannot be parsed is a caller bug (a malformed base URL
				// from the settings box), not a transport failure, so it gets a
				// distinct message.
				if (!CrackUrl(url, host, port, path, secure))
				{
					result.Error = "Invalid URL";
					return result;
				}

				// Handles are created per attempt inside the loop, so a redirect to a
				// different host gets a fresh connection and the old ones are closed
				// by the destructor when this iteration's scope ends.
				WinHttpHandles h;
				// Loopback must not go through the system proxy: auto-detect/WPAD
				// hangs or times out local servers (12002).
				// Loopback must NOT go through the system proxy: auto-detect and WPAD
				// hang or time out against a local server, producing error 12002.
				// The 127. prefix test catches the whole 127.0.0.0/8 range, not just
				// 127.0.0.1.
				bool loopback = (host == L"localhost" || host == L"::1" || host.rfind(L"127.", 0) == 0);
				// The two proxy modes. NO_PROXY for loopback, DEFAULT_PROXY
				// otherwise -- deliberately all-or-nothing, with no WPAD bypass
				// list.
				DWORD access = loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
				// Agent string doubles as the default User-Agent; the explicit header
				// in Get()/PostJson() is what actually reaches the server.
				h.Session = WinHttpOpen(L"Wasteland-Editor/1.0", access,
					WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
				if (!h.Session)
				{
					result.Error = LastErrorString("WinHttpOpen", GetLastError());
					return result;
				}
				// Four timeouts: resolve, connect, send, receive. The 60s receive is
				// generous because a local model can take a long time to produce its
				// FIRST token. PostStream below uses an even longer receive timeout.
				DWORD timeouts[4] = {15000, 15000, 30000, 60000};
				// The array is unpacked into the four positional parameters. A local
				// array rather than four named constants so the order is stated
				// once, visibly.
				WinHttpSetTimeouts(h.Session, timeouts[0], timeouts[1], timeouts[2], timeouts[3]);

				h.Connect = WinHttpConnect(h.Session, host.c_str(), port, 0);
				if (!h.Connect)
				{
					result.Error = LastErrorString("WinHttpConnect", GetLastError());
					return result;
				}

				// WINHTTP_FLAG_SECURE is what turns on TLS; without it the same
				// code path sends plaintext even for an https:// URL.
				DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
				h.Request = WinHttpOpenRequest(h.Connect, ToWide(currentMethod).c_str(), path.c_str(),
					nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
				if (!h.Request)
				{
					result.Error = LastErrorString("WinHttpOpenRequest", GetLastError());
					return result;
				}

				// Caller headers are serialized into ONE block rather than passed as
				// a WINHTTP_HEADERS structure. Simple, and fine for the handful of
				// headers this client sends; it also means a header VALUE containing
				// CRLF could inject an extra header (nothing here produces one).
				std::wstring headerBlock;
				// Copied by non-const reference so the lowercasing loop below can
				// modify the temporary key copy.
				for (auto &kv : currentHeaders)
				{
					// "Name: value\r\n" per header. CRLF because that is what HTTP
					// requires, and because WinHTTP splits the block on it.
					headerBlock += ToWide(kv.first);
					headerBlock += L": ";
					headerBlock += ToWide(kv.second);
					headerBlock += L"\r\n";
				}
				// A default Content-Type is added only for a POST with a body -- a
				// GET has nothing to describe.
				if (currentMethod == "POST" && currentBody.size() > 0)
				{
					// Case-insensitive check, because a caller supplying "Content-Type"
					// -- as most do -- must not be duplicated.
					bool hasContentType = false;
					// Runs only for a POST with a non-empty body; a GET needs no
					// Content-Type and adding one would be wrong.
					for (auto &kv : currentHeaders)
					{
						std::string k = kv.first;
						for (char &c : k)
							c = (char)tolower((unsigned char)c);
						if (k == "content-type")
						{
							hasContentType = true;
							break;
						}
					}
					if (!hasContentType)
						headerBlock += L"Content-Type: application/json\r\n";
				}

				// WINHTTP_NO_REQUEST_DATA for an empty body is the documented way to
				// send a request with no payload; passing a valid pointer with length
				// 0 is not equivalent.
				const void *bodyPtr = currentBody.empty() ? WINHTTP_NO_REQUEST_DATA : (const void *)currentBody.data();
				// DWORD length. DEFECT: currentBody.size() is a size_t truncated to
				// 32 bits with no check, so a body over 4 GiB silently sends a
				// wrong length. Not reachable for a chat JSON payload, but it is
				// the kind of thing that bites if this is reused for uploads.
				DWORD bodyLen = currentBody.empty() ? 0 : (DWORD)currentBody.size();
				// (DWORD)-1 for the header length tells WinHTTP the header block is
				// null-terminated, so it does not need a length.
				if (!WinHttpSendRequest(h.Request, headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
						headerBlock.empty() ? 0 : (DWORD)-1, (LPVOID)bodyPtr, bodyLen, bodyLen, 0))
				{
					result.Error = LastErrorString("WinHttpSendRequest", GetLastError());
					return result;
				}
				// Separated from the send so a transport failure and a server
				// failure produce different messages.
				if (!WinHttpReceiveResponse(h.Request, nullptr))
				{
					result.Error = LastErrorString("WinHttpReceiveResponse", GetLastError());
					return result;
				}

				// Status is read before any body handling, because a redirect must be
				// acted on without downloading the body first.
				long status = 0;
				if (!ReadStatus(h.Request, status))
				{
					result.Error = LastErrorString("WinHttpQueryHeaders(status)", GetLastError());
					return result;
				}

				// Redirect handling, done by hand rather than via
				// WINHTTP_OPTION_REDIRECT_POLICY, because the Authorization-stripping
				// rule below is not something the automatic policy offers.
				if (IsRedirect(status))
				{
					// The target of the redirect, as a wide string.
					std::wstring location;
					// A redirect with no Location cannot be followed. Reported rather
					// than treated as a terminal response.
					if (!ReadHeader(h.Request, WINHTTP_QUERY_LOCATION, location))
					{
						result.Error = "Redirect without Location header";
						return result;
					}
					// Converted back to UTF-8 so it can be compared against the string
					// form of the current URL.
					std::string next = WideToUtf8(location);
					// A Location that is not absolute is relative to the current URL.
					if (next.rfind("http://", 0) != 0 && next.rfind("https://", 0) != 0)
					{
						// Relative redirect: keep scheme+host.
						// Base = scheme://host, taken by finding the path's leading slash.
						// This drops the old path, which is correct for the common
						// "redirect to another endpoint" case but would be wrong for a
						// relative redirect that is meant to be resolved against the old
						// directory (RFC 3986 says resolve against the base URI, and this
						// approximation only matches it when the old path has no trailing
						// directory component).
						size_t schemeEnd = url.find("://");
						// Search for the path's first slash, which is what terminates the
						// scheme://host prefix. Falls back to offset 0 when there is no
						// "://" at all, so a malformed URL degrades rather than throwing.
						size_t hostEnd = url.find('/', schemeEnd == std::string::npos ? 0 : schemeEnd + 3);
						std::string base = (hostEnd == std::string::npos) ? url : url.substr(0, hostEnd);
						next = (next.empty() || next[0] != '/') ? base + "/" + next : base + next;
					}
					// Drop Authorization when the host changes (e.g. a HuggingFace
					// redirect to a CDN). DEFECT: the comparison is on host ONLY -- a
					// redirect that changes only the PORT, or that downgrades https to
					// http on the same host, keeps the Authorization header. Port is
					// re-cracked into `port`/`dp` but never compared.
					// Drop Authorization when the host changes (e.g. HF -> CDN).
					std::wstring oldHost, newHost, dummy;
					// dp/ds are throwaway outputs for the second CrackUrl. DEFECT: `port`
					// and `ds` are the LOOP's own variables being reused as scratch, so
					// they are clobbered here and only happen to be safe because the
					// top of the next iteration reassigns them.
					INTERNET_PORT dp = 0;
					bool ds = false;
					std::wstring nh, np;
					// `port` and `ds` are reused as scratch outputs here, which overwrites
					// the loop's own port and secure flag with the NEW url's values. That
					// happens to be harmless because both are recomputed at the top of
					// the next iteration -- but it is fragile: `ds` is written by both
					// CrackUrl calls and read by neither.
					if (CrackUrl(url, oldHost, port, dummy, ds) && CrackUrl(next, nh, dp, np, ds) && oldHost != nh)
					{
						// Rebuilt rather than erased in place, because currentHeaders is
						// const-shaped from the caller's perspective.
						HttpHeaders filtered;
						for (auto &kv : currentHeaders)
						{
							std::string k = kv.first;
							for (char &c : k)
								c = (char)tolower((unsigned char)c);
							// Compared against a lowercased copy, so "Authorization" is matched
							// without mutating the header that gets copied through.
							if (k != "authorization")
								filtered.push_back(kv);
						}
						// Replaces the whole vector only after the filtered copy is complete,
						// so a partial filter can never be observed.
						currentHeaders = filtered;
					}
					// Only 303 downgrades to GET. 301/302 are followed with the method and
					// body intact, which is a real divergence from what browsers and most
					// HTTP clients do for those two codes.
					if (status == 303)
					{
						// The 303 downgrade: method and body both become GET-shaped, since a
						// GET with a leftover JSON body is not a thing servers accept.
						currentMethod = "GET";
						currentBody.clear();
					}
					// Loop control: `continue` re-enters at the top with the new URL,
					// which is what makes this a redirect LOOP rather than recursion.
					url = next;
					continue;
				}

				// Status recorded before the download/body branches, so a caller
				// always sees the code even when the body is streamed to a file.
				result.StatusCode = status;
				// Download mode: the body goes to disk and HttpResult::Body is left
				// empty. This is the branch that cannot buffer a multi-gigabyte
				// model file.
				if (!downloadPath.empty())
				{
					// Stream body to file.
					std::wstring lenStr;
					// Zero means "unknown", which the progress bar treats as no total.
					uint64_t contentLen = 0;
					// Content-Length is optional -- chunked responses have none -- so a
					// failure here leaves contentLen at 0 and the progress bar simply
					// has no total.
					if (ReadHeader(h.Request, WINHTTP_QUERY_CONTENT_LENGTH, lenStr))
					{
						try
						{
							// Wrapped in try/catch because std::stoull throws std::invalid_argument
							// on a malformed header, and a bad Content-Length must not abort the
							// download.
							contentLen = std::stoull(WideToUtf8(lenStr));
						}
						// catch(...) rather than catch(const std::exception&):
						// std::stoull throws invalid_argument/out_of_range, and being
						// specific here would risk letting something else escape.
						catch (...)
						{
							contentLen = 0;
						}
					}
					// Published to the UI thread through the atomic, so it needs no
					// lock.
					if (total)
						total->store(contentLen);

					// Download to a .part sibling and rename at the end, so an
					// interrupted download never leaves a truncated file at the real
					// path. The suffix goes on the full path so both are on the same
					// volume, which is what makes the final rename a move rather than a
					// copy.
					std::filesystem::path tmpPath = downloadPath;
					// Operator+= on a path appends a filename component, producing
					// "<dest>.part" rather than replacing the extension.
					tmpPath += ".part";
					std::error_code ec;
					// Creates the destination directory if needed -- a model download into
					// a fresh cache folder should not require the caller to pre-create it.
					std::filesystem::create_directories(downloadPath.parent_path(), ec);

					// binary: model files are not text and a text-mode write would
					// corrupt them on Windows. trunc: a stale .part from a previous
					// failed run is discarded.
					std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
					if (!file.is_open())
					{
						result.Error = "Cannot open file for writing: " + tmpPath.string();
						return result;
					}
					// Progress counters, updated per chunk below. `downloaded` is polled by
					// the UI thread, hence the atomics in the signature.
					uint64_t received = 0;
					// `failed` separates "the loop ended badly" from "the loop ended
					// because the body was complete" -- the ofstream close and the
					// status check below both depend on telling those apart.
					bool failed = false;
					for (;;)
					{
						// Cancellation is polled between chunks, so it is only observed once
						// the current WinHttpReadData returns. A stalled read blocks cancel
						// for up to the receive timeout.
						if (cancel && cancel->load())
						{
							// Close before removing: on Windows an open file cannot be deleted.
							// Removing the .part is what makes a cancel leave no debris.
							file.close();
							std::error_code ec2;
							std::filesystem::remove(tmpPath, ec2);
							// Distinct from a transport failure so the caller can tell a user
							// cancellation from a network error.
							result.Error = "Cancelled";
							return result;
						}
						// Query-then-read, the standard WinHTTP streaming pair.
						DWORD available = 0;
						if (!WinHttpQueryDataAvailable(h.Request, &available))
						{
							result.Error = LastErrorString("WinHttpQueryDataAvailable", GetLastError());
							failed = true;
							break;
						}
						// available == 0 is the documented end-of-body signal.
						if (available == 0)
							break;
						// Chunk buffer sized to exactly what WinHTTP said is available, so no
						// read ever exceeds it.
						std::vector<char> chunk(available);
						// Reset per iteration: WinHttpReadData is told how many bytes it
						// may write, and `read` reports how many it actually did.
						DWORD read = 0;
						// A zero-byte read alongside a successful WinHttpReadData is treated
						// as a failure here, whereas the in-memory branch below treats it as
						// a clean end of stream. The two disagree.
						if (!WinHttpReadData(h.Request, chunk.data(), available, &read) || read == 0)
						{
							result.Error = LastErrorString("WinHttpReadData", GetLastError());
							failed = true;
							break;
						}
						// Written straight through; `received` is the running total for the
						// progress display.
						file.write(chunk.data(), read);
						// Accumulated across chunks so the atomic reports a running total
						// rather than the last chunk's size.
						received += read;
						if (downloaded)
							downloaded->store(received);
					}
					// Explicit close so flush errors surface here rather than in the
					// destructor after `result` has already been returned.
					file.close();
					if (failed)
					{
						// A distinct error_code so it cannot be confused with the `ec` from
						// create_directories above.
						std::error_code ec2;
						// On any failure the .part is deleted, so a retry starts clean.
						std::filesystem::remove(tmpPath, ec2);
						return result;
					}
					// The HTTP status is validated only AFTER the body has been fully
					// written to disk. A 404 page is therefore downloaded in full and
					// then discarded.
					if (!(status >= 200 && status < 300))
					{
						std::error_code ec2;
						std::filesystem::remove(tmpPath, ec2);
						result.Error = "HTTP " + std::to_string(status);
						return result;
					}
					// remove-then-rename rather than a plain rename, because
					// std::filesystem::rename does not overwrite an existing destination
					// on Windows. The window between the two is why the destination is
					// removed first: a reader can briefly see neither file.
					std::error_code ec3;
					// Two steps rather than a single overwrite-rename, because
					// std::filesystem::rename does not replace an existing destination
					// on Windows. There is a window where neither file exists.
					std::filesystem::remove(downloadPath, ec3);
					std::filesystem::rename(tmpPath, downloadPath, ec3);
					if (ec3)
					{
						result.Error = "Cannot finalize file: " + ec3.message();
						return result;
					}
					// The only place a download is marked successful.
					result.Succeeded = true;
					return result;
				}

				// Buffered mode: the whole body goes into memory.
				// Read body into memory.
				std::string out;
				for (;;)
				{
					// Same query/read pair as the download branch, but writing into
					// memory.
					DWORD available = 0;
					if (!WinHttpQueryDataAvailable(h.Request, &available))
					{
						result.Error = LastErrorString("WinHttpQueryDataAvailable", GetLastError());
						return result;
					}
					if (available == 0)
						break;
					// resize-then-read into the tail: this avoids a second buffer and a
					// copy that a read-into-a-temporary-then-append approach would need.
					size_t base = out.size();
					// Grow first, read into the tail. The buffer may be reallocated by
					// this resize, which is why `base` (an index, not a pointer) is
					// captured before it.
					out.resize(base + available);
					DWORD read = 0;
					if (!WinHttpReadData(h.Request, &out[base], available, &read))
					{
						result.Error = LastErrorString("WinHttpReadData", GetLastError());
						return result;
					}
					out.resize(base + read);
					// A zero-byte read ends the loop -- the normal end-of-body case, in
					// contrast to the download branch above.
					if (read == 0)
						break;
				}
				// move rather than copy: the body can be large and out is dead after
				// this line.
				result.Body = std::move(out);
				// Success is the 2xx range only. A 3xx that survived the redirect
				// loop, and any 4xx/5xx, are all failures.
				result.Succeeded = (status >= 200 && status < 300);
				if (!result.Succeeded)
					result.Error = "HTTP " + std::to_string(status);
				return result;
			}

			// Loop exhausted: every one of the six attempts was a redirect.
			result.Error = "Too many redirects";
			return result;
		}
	}

	// Plain GET, body buffered. The two added headers are appended rather
	// than merged, so a caller that already supplied Accept or User-Agent
	// ends up with the header twice -- harmless for these two in practice,
	// but it is emplace_back, not a replace.
	HttpResult HttpClient::Get(const std::string &url, const HttpHeaders &headers)
	{
		HttpHeaders h = headers;
		// DEFECT: emplace_back APPENDS, it does not replace. A caller that
		// passed its own User-Agent or Accept gets both headers on the wire.
		h.emplace_back("User-Agent", "Wasteland-Editor/1.0");
		h.emplace_back("Accept", "application/json");
		// Empty downloadPath and null progress/cancel pointers select the
		// buffered branch inside Perform and disable the progress plumbing.
		return Perform("GET", url, h, "", {}, nullptr, nullptr, nullptr);
	}

	// POST a JSON body. No Content-Type here -- Perform adds one by
	// default when the caller has not specified it.
	HttpResult HttpClient::PostJson(const std::string &url, const std::string &jsonBody, const HttpHeaders &headers)
	{
		HttpHeaders h = headers;
		h.emplace_back("User-Agent", "Wasteland-Editor/1.0");
		h.emplace_back("Accept", "application/json");
		// The trailing {} is an empty downloadPath and the three nullptrs
		// disable progress and cancellation, selecting the buffered branch.
		return Perform("POST", url, h, jsonBody, {}, nullptr, nullptr, nullptr);
	}

	// Streaming POST for SSE-style endpoints. Returns "" on success and
	// an error message otherwise, so the caller distinguishes the two by
	// emptiness rather than by a status flag.
	//
	// Deliberately does NOT share Perform's machinery: no redirect
	// handling, no download-to-file mode, no cancellation. Chat endpoints
	// redirect rarely, and the streaming callback has to be invoked
	// inline as data arrives.
	std::string HttpClient::PostStream(const std::string &url, const std::string &jsonBody, const HttpHeaders &headers,
		std::function<void(const char *data, size_t len)> onChunk, long &statusOut)
	{
		// Zeroed up front so a caller that gets an early error still reads a
		// defined 0 rather than an uninitialized value.
		statusOut = 0;
		// Parsed on the worker side of the call, so a malformed URL returns
		// "Invalid URL" immediately rather than opening a session.
		std::wstring host, path;
		INTERNET_PORT port = 0;
		bool secure = false;
		if (!CrackUrl(url, host, port, path, secure))
			return "Invalid URL";

		// Same RAII wrapper as Perform -- the only shared piece of machinery
		// between the two functions.
		WinHttpHandles h;
		// Same loopback proxy bypass as Perform, duplicated rather than shared
		// -- this function deliberately has its own WinHTTP setup.
		bool loopback = (host == L"localhost" || host == L"::1" || host.rfind(L"127.", 0) == 0);
		DWORD access = loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
		h.Session = WinHttpOpen(L"Wasteland-Editor/1.0", access,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!h.Session)
			return LastErrorString("WinHttpOpen", GetLastError());
		// Generous receive timeout: it only applies to gaps BETWEEN packets,
		// so a slow first token (a model loading) does not abort the request.
		// 300s here versus 60s in Perform, because a streamed completion can
		// -- legitimately pause mid-answer.
		// Generous receive timeout: it only applies to gaps between packets,
		// so slow first tokens (model load) don't abort the request.
		WinHttpSetTimeouts(h.Session, 15000, 15000, 30000, 300000);

		h.Connect = WinHttpConnect(h.Session, host.c_str(), port, 0);
		if (!h.Connect)
			return LastErrorString("WinHttpConnect", GetLastError());

		DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
		// Method is hardcoded POST -- this entry point only exists for chat
		// completions and uploads.
		h.Request = WinHttpOpenRequest(h.Connect, L"POST", path.c_str(),
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
		if (!h.Request)
			return LastErrorString("WinHttpOpenRequest", GetLastError());

		// Same single-header-block approach as Perform.
		std::wstring headerBlock;
		for (auto &kv : headers)
		{
			headerBlock += ToWide(kv.first);
			headerBlock += L": ";
			headerBlock += ToWide(kv.second);
			headerBlock += L"\r\n";
		}
		// Unconditional Content-Type. DEFECT: unlike Perform, this does not
		// check whether the caller already supplied one, so passing a
		// Content-Type through HttpHeaders produces a duplicate header here.
		headerBlock += L"Content-Type: application/json\r\n";
		// Asks for an event stream explicitly. This is what makes the server
		// emit SSE framing instead of a single JSON body.
		headerBlock += L"Accept: text/event-stream\r\n";

		// jsonBody is empty-safe: a null pointer with length 0 is what
		// WinHTTP expects for an empty body.
		DWORD bodyLen = (DWORD)jsonBody.size();
		if (!WinHttpSendRequest(h.Request, headerBlock.c_str(), (DWORD)-1,
				(LPVOID)(bodyLen ? jsonBody.data() : nullptr), bodyLen, bodyLen, 0))
			return LastErrorString("WinHttpSendRequest", GetLastError());
		if (!WinHttpReceiveResponse(h.Request, nullptr))
			return LastErrorString("WinHttpReceiveResponse", GetLastError());

		long status = 0;
		if (!ReadStatus(h.Request, status))
			return LastErrorString("WinHttpQueryHeaders(status)", GetLastError());
		// Status is always reported to the caller, even on failure, so the UI
		// can distinguish "401 unauthorized" from "no response".
		statusOut = status;
		// Non-2xx is handled specially: the body is drained for a useful
		// error message rather than fed to onChunk.
		if (status < 200 || status >= 300)
		{
			// Reused `err` name from Perform's scope? No -- this is a separate
			// function with its own local, so the two do not interact.
			std::string err;
			// On an error status, drain the body to build a useful message.
			// This deliberately caps how much is read (see below) -- an error
			// page from a misconfigured proxy can be arbitrarily large.
			for (;;)
			{
				DWORD available = 0;
				// A query failure and end-of-body are treated identically here
				// (break), unlike the streaming loop below which distinguishes them.
				if (!WinHttpQueryDataAvailable(h.Request, &available) || available == 0)
					break;
				size_t base = err.size();
				err.resize(base + available);
				DWORD read = 0;
				if (!WinHttpReadData(h.Request, &err[base], available, &read))
					break;
				err.resize(base + read);
				// The cap is checked AFTER the append, so `err` can exceed 8192 by up
				// -- to one chunk before the loop stops.
				if (read == 0 || err.size() > 8192)
					break;
			}
			// "HTTP <code>" is the fallback prefix; the server's own text, when
			// present, is appended after a colon.
			std::string detail = "HTTP " + std::to_string(status);
			if (!err.empty())
				// Truncated to 300 characters for display. The server's own error
				// text is the useful part; the rest is usually HTML.
				detail += ": " + err.substr(0, 300);
			return detail;
		}

		// The streaming loop. Every chunk is handed straight to onChunk as it
		// arrives -- that is the whole point of this entry point.
		for (;;)
		{
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(h.Request, &available))
				return LastErrorString("WinHttpQueryDataAvailable", GetLastError());
			if (available == 0)
				// Clean end of stream: success, reported as an empty error string.
				return ""; // Clean end of stream.
			// Sized per chunk like the download path, so a large SSE burst does
			// not accumulate in one buffer.
			std::vector<char> chunk(available);
			DWORD read = 0;
			if (!WinHttpReadData(h.Request, chunk.data(), available, &read) || read == 0)
				return LastErrorString("WinHttpReadData", GetLastError());
			// The callback receives a borrowed pointer valid only for this call,
			// so a consumer that keeps the data must copy it. AIChatPanel
			// appends into a std::string, which does.
			onChunk(chunk.data(), read);
		}
	}

	// Blocking download for the model-download panel. Everything is
	// blocking and out-parameter based because it runs on a worker thread
	// that the UI polls through the atomics.
	void HttpClient::DownloadFile(const std::string &url, const std::filesystem::path &destPath, const HttpHeaders &headers,
		std::atomic<uint64_t> &downloaded, std::atomic<uint64_t> &total,
		std::atomic<bool> &cancel, std::atomic<bool> &finished, std::string &errorOut)
	{
		// Zeroed up front so a progress bar never shows stale bytes from a
		// previous download before the first chunk arrives.
		downloaded.store(0);
		// download counter is NOT reset here, only total -- the caller
		// reads downloaded to drive the progress bar and it is zeroed in
		// the ofstream path below.
		total.store(0);
		HttpHeaders h = headers;
		// No Accept header, unlike Get/PostJson: the response is a binary
		// -- weight file, not JSON.
		h.emplace_back("User-Agent", "Wasteland-Editor/1.0");
		// The caller's headers plus a default User-Agent. No Accept header
		// here, unlike Get()/PostJson(), because the response is a binary
		// -- not JSON.
		HttpResult r = Perform("GET", url, h, "", destPath, &downloaded, &total, &cancel);
		// errorOut is derived from the result rather than passed through, so
		// the caller gets the same "empty means success" convention the
		// other entry points use.
		errorOut = r.Succeeded ? "" : r.Error;
		// Set on EVERY exit path including failure, so the UI's worker thread
		// always stops polling. A caller that forgets to check `finished`
		// would otherwise spin forever on a failed download.
		finished.store(true);
	}

// -------------------------------
// #else
// Non-Windows: the client is unavailable and says so.
// -------------------------------
#else

	// Each stub returns the same fixed message. They report failure
	// through HttpResult rather than throwing, so a caller needs no
	// platform #ifdef of its own.
	HttpResult HttpClient::Get(const std::string &, const HttpHeaders &)
	{
		// Aggregate-initialized in HttpResult's declaration order
		// -- (Succeeded, StatusCode, Body, Error). Reordering those members
		// would silently break this and every other stub.
		return HttpResult{false, 0, "", "HTTPS client requires Windows (WinHTTP)"};
	}

	HttpResult HttpClient::PostJson(const std::string &, const std::string &, const HttpHeaders &)
	{
		return HttpResult{false, 0, "", "HTTPS client requires Windows (WinHTTP)"};
	}

	// PostStream's stub sets statusOut before returning, matching the
	// real implementation's contract that the out-param is always
	// written.
	std::string HttpClient::PostStream(const std::string &, const std::string &, const HttpHeaders &,
		std::function<void(const char *, size_t)>, long &statusOut)
	{
		// statusOut written even on the unsupported path, so a caller that
		// reads it unconditionally sees a defined 0.
		statusOut = 0;
		return "HTTPS client requires Windows (WinHTTP)";
	}

	// The download stub still zeroes and sets the progress atomics and
	// `finished`, so the UI thread's polling loop terminates on a
	// non-Windows build instead of hanging.
	void HttpClient::DownloadFile(const std::string &, const std::filesystem::path &, const HttpHeaders &,
		std::atomic<uint64_t> &downloaded, std::atomic<uint64_t> &total,
		std::atomic<bool> &, std::atomic<bool> &finished, std::string &errorOut)
	{
		// Mirrors the real DownloadFile's contract exactly: progress zeroed
		// and finished set on every exit, so the UI's polling loop
		// terminates identically.
		downloaded.store(0);
		total.store(0);
		// errorOut is set BEFORE finished, so a UI thread that wakes on
		// `finished` always finds the error message already written.
		errorOut = "HTTPS client requires Windows (WinHTTP)";
		finished.store(true);
	}

#endif

}
