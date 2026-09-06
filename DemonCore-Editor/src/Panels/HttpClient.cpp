#include "wlpch.h"
#include "HttpClient.h"

#ifdef WL_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

#include <cctype>
#include <cstdio>
#include <fstream>

namespace Wasteland
{

	std::string HttpClient::UrlEncode(const std::string &value)
	{
		static const char *hex = "0123456789ABCDEF";
		std::string out;
		out.reserve(value.size());
		for (unsigned char c : value)
		{
			if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
				out += (char)c;
			else if (c == ' ')
				out += "%20";
			else
			{
				out += '%';
				out += hex[c >> 4];
				out += hex[c & 0xF];
			}
		}
		return out;
	}

#ifdef WL_PLATFORM_WINDOWS

	namespace
	{
		std::wstring ToWide(const std::string &s)
		{
			if (s.empty())
				return L"";
			int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
			std::wstring w(n, 0);
			MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
			return w;
		}

		std::string LastErrorString(const char *what, DWORD code)
		{
			char buf[128];
			snprintf(buf, sizeof(buf), "%s failed (code %lu)", what, (unsigned long)code);
			return std::string(buf);
		}

		struct WinHttpHandles
		{
			HINTERNET Session = nullptr, Connect = nullptr, Request = nullptr;
			~WinHttpHandles()
			{
				if (Request)
					WinHttpCloseHandle(Request);
				if (Connect)
					WinHttpCloseHandle(Connect);
				if (Session)
					WinHttpCloseHandle(Session);
			}
		};

		// Splits "https://host:port/path?query" into components for WinHTTP.
		bool CrackUrl(const std::string &url, std::wstring &host, INTERNET_PORT &port, std::wstring &path, bool &secure)
		{
			URL_COMPONENTSW comp = {};
			comp.dwStructSize = sizeof(comp);
			comp.dwHostNameLength = (DWORD)-1;
			comp.dwUrlPathLength = (DWORD)-1;
			comp.dwExtraInfoLength = (DWORD)-1;
			comp.dwSchemeLength = (DWORD)-1;
			std::wstring wurl = ToWide(url);
			if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &comp))
				return false;
			host.assign(comp.lpszHostName, comp.dwHostNameLength);
			port = comp.nPort;
			secure = (comp.nScheme == INTERNET_SCHEME_HTTPS);
			std::wstring p;
			if (comp.dwUrlPathLength > 0)
				p.assign(comp.lpszUrlPath, comp.dwUrlPathLength);
			else
				p = L"/";
			if (comp.dwExtraInfoLength > 0)
				p.append(comp.lpszExtraInfo, comp.dwExtraInfoLength);
			path = p;
			return true;
		}

		bool ReadStatus(HINTERNET hRequest, long &status)
		{
			DWORD code = 0, size = sizeof(code);
			if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX))
				return false;
			status = (long)code;
			return true;
		}

		bool ReadHeader(HINTERNET hRequest, DWORD query, std::wstring &out)
		{
			DWORD size = 0;
			WinHttpQueryHeaders(hRequest, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
				return false;
			std::wstring buf(size / sizeof(wchar_t), 0);
			if (!WinHttpQueryHeaders(hRequest, query, WINHTTP_HEADER_NAME_BY_INDEX, &buf[0], &size, WINHTTP_NO_HEADER_INDEX))
				return false;
			// size includes null terminator
			if (!buf.empty() && buf.back() == L'\0')
				buf.pop_back();
			out = buf;
			return true;
		}

		std::string WideToUtf8(const std::wstring &w)
		{
			if (w.empty())
				return "";
			int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
			std::string s(n, 0);
			WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
			return s;
		}

		bool IsRedirect(long status)
		{
			return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
		}

		struct RequestResult
		{
			bool TransportOk = false;
			std::string TransportError;
			long Status = 0;
			bool GotResponse = false; // false when caller streams download and consumed body itself
		};

		// Core request with manual redirect following (max 5 hops).
		// If downloadPath is non-empty, the body streams to that file and Body stays empty.
		HttpResult Perform(const std::string &method, const std::string &startUrl, const HttpHeaders &headers,
			const std::string &body, const std::filesystem::path &downloadPath,
			std::atomic<uint64_t> *downloaded, std::atomic<uint64_t> *total, std::atomic<bool> *cancel)
		{
			HttpResult result;
			std::string url = startUrl;
			std::string currentMethod = method;
			std::string currentBody = body;
			HttpHeaders currentHeaders = headers;

			for (int hop = 0; hop < 6; hop++)
			{
				std::wstring host, path;
				INTERNET_PORT port = 0;
				bool secure = false;
				if (!CrackUrl(url, host, port, path, secure))
				{
					result.Error = "Invalid URL";
					return result;
				}

				WinHttpHandles h;
				h.Session = WinHttpOpen(L"Wasteland-Editor/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
					WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
				if (!h.Session)
				{
					result.Error = LastErrorString("WinHttpOpen", GetLastError());
					return result;
				}
				DWORD timeouts[4] = {15000, 15000, 30000, 60000};
				WinHttpSetTimeouts(h.Session, timeouts[0], timeouts[1], timeouts[2], timeouts[3]);

				h.Connect = WinHttpConnect(h.Session, host.c_str(), port, 0);
				if (!h.Connect)
				{
					result.Error = LastErrorString("WinHttpConnect", GetLastError());
					return result;
				}

				DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
				h.Request = WinHttpOpenRequest(h.Connect, ToWide(currentMethod).c_str(), path.c_str(),
					nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
				if (!h.Request)
				{
					result.Error = LastErrorString("WinHttpOpenRequest", GetLastError());
					return result;
				}

				std::wstring headerBlock;
				for (auto &kv : currentHeaders)
				{
					headerBlock += ToWide(kv.first);
					headerBlock += L": ";
					headerBlock += ToWide(kv.second);
					headerBlock += L"\r\n";
				}
				if (currentMethod == "POST" && currentBody.size() > 0)
				{
					bool hasContentType = false;
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

				const void *bodyPtr = currentBody.empty() ? WINHTTP_NO_REQUEST_DATA : (const void *)currentBody.data();
				DWORD bodyLen = currentBody.empty() ? 0 : (DWORD)currentBody.size();
				if (!WinHttpSendRequest(h.Request, headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
						headerBlock.empty() ? 0 : (DWORD)-1, (LPVOID)bodyPtr, bodyLen, bodyLen, 0))
				{
					result.Error = LastErrorString("WinHttpSendRequest", GetLastError());
					return result;
				}
				if (!WinHttpReceiveResponse(h.Request, nullptr))
				{
					result.Error = LastErrorString("WinHttpReceiveResponse", GetLastError());
					return result;
				}

				long status = 0;
				if (!ReadStatus(h.Request, status))
				{
					result.Error = LastErrorString("WinHttpQueryHeaders(status)", GetLastError());
					return result;
				}

				if (IsRedirect(status))
				{
					std::wstring location;
					if (!ReadHeader(h.Request, WINHTTP_QUERY_LOCATION, location))
					{
						result.Error = "Redirect without Location header";
						return result;
					}
					std::string next = WideToUtf8(location);
					if (next.rfind("http://", 0) != 0 && next.rfind("https://", 0) != 0)
					{
						// Relative redirect: keep scheme+host.
						size_t schemeEnd = url.find("://");
						size_t hostEnd = url.find('/', schemeEnd == std::string::npos ? 0 : schemeEnd + 3);
						std::string base = (hostEnd == std::string::npos) ? url : url.substr(0, hostEnd);
						next = (next.empty() || next[0] != '/') ? base + "/" + next : base + next;
					}
					// Drop Authorization when the host changes (e.g. HF -> CDN).
					std::wstring oldHost, newHost, dummy;
					INTERNET_PORT dp = 0;
					bool ds = false;
					std::wstring nh, np;
					if (CrackUrl(url, oldHost, port, dummy, ds) && CrackUrl(next, nh, dp, np, ds) && oldHost != nh)
					{
						HttpHeaders filtered;
						for (auto &kv : currentHeaders)
						{
							std::string k = kv.first;
							for (char &c : k)
								c = (char)tolower((unsigned char)c);
							if (k != "authorization")
								filtered.push_back(kv);
						}
						currentHeaders = filtered;
					}
					if (status == 303)
					{
						currentMethod = "GET";
						currentBody.clear();
					}
					url = next;
					continue;
				}

				result.StatusCode = status;
				if (!downloadPath.empty())
				{
					// Stream body to file.
					std::wstring lenStr;
					uint64_t contentLen = 0;
					if (ReadHeader(h.Request, WINHTTP_QUERY_CONTENT_LENGTH, lenStr))
					{
						try
						{
							contentLen = std::stoull(WideToUtf8(lenStr));
						}
						catch (...)
						{
							contentLen = 0;
						}
					}
					if (total)
						total->store(contentLen);

					std::filesystem::path tmpPath = downloadPath;
					tmpPath += ".part";
					std::error_code ec;
					std::filesystem::create_directories(downloadPath.parent_path(), ec);

					std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
					if (!file.is_open())
					{
						result.Error = "Cannot open file for writing: " + tmpPath.string();
						return result;
					}
					uint64_t received = 0;
					bool failed = false;
					for (;;)
					{
						if (cancel && cancel->load())
						{
							file.close();
							std::error_code ec2;
							std::filesystem::remove(tmpPath, ec2);
							result.Error = "Cancelled";
							return result;
						}
						DWORD available = 0;
						if (!WinHttpQueryDataAvailable(h.Request, &available))
						{
							result.Error = LastErrorString("WinHttpQueryDataAvailable", GetLastError());
							failed = true;
							break;
						}
						if (available == 0)
							break;
						std::vector<char> chunk(available);
						DWORD read = 0;
						if (!WinHttpReadData(h.Request, chunk.data(), available, &read) || read == 0)
						{
							result.Error = LastErrorString("WinHttpReadData", GetLastError());
							failed = true;
							break;
						}
						file.write(chunk.data(), read);
						received += read;
						if (downloaded)
							downloaded->store(received);
					}
					file.close();
					if (failed)
					{
						std::error_code ec2;
						std::filesystem::remove(tmpPath, ec2);
						return result;
					}
					if (!(status >= 200 && status < 300))
					{
						std::error_code ec2;
						std::filesystem::remove(tmpPath, ec2);
						result.Error = "HTTP " + std::to_string(status);
						return result;
					}
					std::error_code ec3;
					std::filesystem::remove(downloadPath, ec3);
					std::filesystem::rename(tmpPath, downloadPath, ec3);
					if (ec3)
					{
						result.Error = "Cannot finalize file: " + ec3.message();
						return result;
					}
					result.Succeeded = true;
					return result;
				}

				// Read body into memory.
				std::string out;
				for (;;)
				{
					DWORD available = 0;
					if (!WinHttpQueryDataAvailable(h.Request, &available))
					{
						result.Error = LastErrorString("WinHttpQueryDataAvailable", GetLastError());
						return result;
					}
					if (available == 0)
						break;
					size_t base = out.size();
					out.resize(base + available);
					DWORD read = 0;
					if (!WinHttpReadData(h.Request, &out[base], available, &read))
					{
						result.Error = LastErrorString("WinHttpReadData", GetLastError());
						return result;
					}
					out.resize(base + read);
					if (read == 0)
						break;
				}
				result.Body = std::move(out);
				result.Succeeded = (status >= 200 && status < 300);
				if (!result.Succeeded)
					result.Error = "HTTP " + std::to_string(status);
				return result;
			}

			result.Error = "Too many redirects";
			return result;
		}
	}

	HttpResult HttpClient::Get(const std::string &url, const HttpHeaders &headers)
	{
		HttpHeaders h = headers;
		h.emplace_back("User-Agent", "Wasteland-Editor/1.0");
		h.emplace_back("Accept", "application/json");
		return Perform("GET", url, h, "", {}, nullptr, nullptr, nullptr);
	}

	HttpResult HttpClient::PostJson(const std::string &url, const std::string &jsonBody, const HttpHeaders &headers)
	{
		HttpHeaders h = headers;
		h.emplace_back("User-Agent", "Wasteland-Editor/1.0");
		h.emplace_back("Accept", "application/json");
		return Perform("POST", url, h, jsonBody, {}, nullptr, nullptr, nullptr);
	}

	void HttpClient::DownloadFile(const std::string &url, const std::filesystem::path &destPath, const HttpHeaders &headers,
		std::atomic<uint64_t> &downloaded, std::atomic<uint64_t> &total,
		std::atomic<bool> &cancel, std::atomic<bool> &finished, std::string &errorOut)
	{
		downloaded.store(0);
		total.store(0);
		HttpHeaders h = headers;
		h.emplace_back("User-Agent", "Wasteland-Editor/1.0");
		HttpResult r = Perform("GET", url, h, "", destPath, &downloaded, &total, &cancel);
		errorOut = r.Succeeded ? "" : r.Error;
		finished.store(true);
	}

#else

	HttpResult HttpClient::Get(const std::string &, const HttpHeaders &)
	{
		return HttpResult{false, 0, "", "HTTPS client requires Windows (WinHTTP)"};
	}

	HttpResult HttpClient::PostJson(const std::string &, const std::string &, const HttpHeaders &)
	{
		return HttpResult{false, 0, "", "HTTPS client requires Windows (WinHTTP)"};
	}

	void HttpClient::DownloadFile(const std::string &, const std::filesystem::path &, const HttpHeaders &,
		std::atomic<uint64_t> &downloaded, std::atomic<uint64_t> &total,
		std::atomic<bool> &, std::atomic<bool> &finished, std::string &errorOut)
	{
		downloaded.store(0);
		total.store(0);
		errorOut = "HTTPS client requires Windows (WinHTTP)";
		finished.store(true);
	}

#endif

}
