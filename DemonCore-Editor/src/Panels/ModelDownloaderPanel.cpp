// PCH first: it supplies <string>, <vector>, <thread>, <mutex> and <atomic>, plus
// the filesystem and ImGui prerequisites this panel needs.
#include "wlpch.h"
// Its own header: the buffers (m_SearchBuf, m_TokenBuf, m_DestDirBuf), the three
// atomic busy flags, and the single m_Worker thread.
#include "ModelDownloaderPanel.h"
// The persisted Hugging Face token. Read on the UI thread and COPIED into each
// worker's lambda, so the worker never touches shared API-key state.
#include "ApiKeyStore.h"
// WinHTTP-backed Get() and DownloadFile(), plus UrlEncode(). All blocking: every
// call below is made from a worker thread, never from OnImGuiRender.
#include "HttpClient.h"
// Hand-rolled field extraction. SplitArrayObjects/ExtractString are tolerant
// partial parsers, which is why the code below checks each return value.
#include "JsonMini.h"
// Project::Get().GetAssetPath("models") is the default download destination.
// Resolving it per click rather than caching is what keeps it correct when the
// user switches projects.
#include "Wasteland/Project/Project.h"

// The whole panel is drawn with immediate-mode ImGui. There is no retained
// widget state beyond the member buffers below.
#include <imgui/imgui.h>

// ShellExecuteA needs Windows headers. Everything below the guard is the only
// platform-specific code in the file.
#ifdef WL_PLATFORM_WINDOWS
#ifndef NOMINMAX
// Prevents <Windows.h> defining min/max macros, which would break every
// std::max/std::min in this file -- including the ones in the header.
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
// Keeps <Windows.h> from dragging in Winsock and the rest of the Win32 surface.
// Both defines are idempotent and guarded, so a PCH that already set them is
// unaffected.
#define WIN32_LEAN_AND_MEAN
#endif
// ShellExecuteA, declared here. The alternative -- _wfopen or a browser API --
// would be more code for no benefit on a Windows-only editor.
#include <Windows.h>
// ShellExecuteA's declaration; the include order above matters.
#include <shellapi.h>
// ShellExecuteA lives in shell32. Without this the link fails at build time, not
// at the click.
#pragma comment(lib, "shell32.lib")
#endif

// The panel lives in the engine namespace alongside the other editor panels,
// so its helpers can see HFModelInfo and HFFileEntry unqualified.
namespace Wasteland
{

	// Case-SENSITIVE suffix test, written by hand because C++17 has no
	// std::string::ends_with and C++20's is not available in this toolchain.
	static bool EndsWith(const std::string &s, const std::string &suffix)
	{
		// Guard against unsigned underflow in the subtraction below. Without it, an
		// empty string tested against a non-empty suffix would wrap to a huge offset.
		if (suffix.size() > s.size())
			return false;
		// compare() with an explicit length avoids allocating a substring just to test.
		return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	// Case-INSENSITIVE variant. This is the one actually used, twice, for .gguf.
	static bool EndsWithNoCase(const std::string &s, const std::string &suffix)
	{
		if (suffix.size() > s.size())
			return false;
		// Computed once and reused in the loop, rather than per character.
		size_t off = s.size() - suffix.size();
		// Compares byte by byte. Correct for ASCII extensions, which is all a filename
		// suffix needs to be here.
		for (size_t i = 0; i < suffix.size(); i++)
		{
			// The (unsigned char) casts matter: tolower on a plain char is undefined for
			// values above 127, and HF filenames are UTF-8.
			if (tolower((unsigned char)s[off + i]) != tolower((unsigned char)suffix[i]))
				return false;
		}
		return true;
	}

	// Only initialises the search box. Every other member has a default member
	// initialiser in the header, which is why this constructor is nearly empty.
	ModelDownloaderPanel::ModelDownloaderPanel()
	{
		// DEFECT: strncpy_s is the MSVC-secure-CRT form and is not standard C++. This is
		// the ONLY non-portable call outside the WL_PLATFORM_WINDOWS guard above, so on
		// a non-MSVC toolchain this file would not compile even with the Win32 calls
		// excluded. _TRUNCATE means an over-long input is cut, never rejected.
		strncpy_s(m_SearchBuf, sizeof(m_SearchBuf), "gguf", _TRUNCATE);
	}

	// Signals cancellation and WAITS for the worker. This is what stops a detached
	// download from writing into a destroyed panel.
	ModelDownloaderPanel::~ModelDownloaderPanel()
	{
		// Set before the join, so a download in progress sees it on its next chunk poll
		// and unwinds. The worker also polls this flag between files.
		m_CancelDownload.store(true);
		// joinable() rather than assuming: after a successful join the thread object is
		// no longer joinable, and joining twice is undefined.
		if (m_Worker.joinable())
			// BLOCKS until the worker returns. On the UI thread this is a visible hitch, but
			// it is the price of not having the worker touch freed memory. A panel that is
			// closed mid-download will stall here until the current chunk completes.
			m_Worker.join();
	}

	// Installs a new worker, joining any previous one first. Centralised so the
	// join-before-start rule is enforced in one place rather than at three call sites.
	void ModelDownloaderPanel::LaunchWorker(std::thread &&t)
	{
		// The previous worker must be finished before its captured `this` can be
		// considered dead. Joining here also means a second click cannot overlap the
		// first -- though the busy guards already prevent that.
		if (m_Worker.joinable())
			m_Worker.join();
		// Move-assign, so the thread handle is transferred rather than copied.
		m_Worker = std::move(t);
	}

	// Builds the auth header set. A free function rather than a member because it
	// touches no panel state.
	static HttpHeaders HFHeaders(const std::string &token)
	{
		// Returned by value. HttpHeaders is a vector of pairs, so copy elision applies.
		HttpHeaders h;
		// Anonymous requests are legitimate -- public repos work without a token -- so
		// no header is added rather than an empty Bearer value being sent.
		if (!token.empty())
			// The standard Hugging Face scheme. The token is copied into a new string, so
			// the header does not alias the caller's buffer.
			h.emplace_back("Authorization", "Bearer " + token);
		return h;
	}

	// Kicks off a Hugging Face model search on a worker thread. Returns immediately;
	// results appear when the worker fills m_Results under the mutex.
	void ModelDownloaderPanel::Search()
	{
		// One busy flag blocks all three operations. They are mutually exclusive because
		// they share m_Results, m_Files and m_SelectedModel, and interleaving two would
		// produce a file list for the wrong model.
		if (m_Searching.load() || m_Listing.load() || m_Downloading.load())
			return;
		// Copied off the ImGui buffer before the thread starts, so the worker never
		// reads a buffer the UI thread may be editing.
		std::string query = m_SearchBuf;
		// Also copied by value into the lambda. Reading ApiKeyStore from the worker
		// would race with the Save button in OnImGuiRender.
		std::string token = ApiKeyStore::Instance().HuggingFaceToken;
		// Set BEFORE launching, so a second click on the same frame is rejected. The
		// clear happens at the end of the worker lambda.
		m_Searching.store(true);
		// DEFECT: written WITHOUT holding m_Mutex, while the worker writes the same
		// std::string under the lock and OnImGuiRender reads it under the lock. This is
		// a genuine data race on a non-atomic string. The same unlocked write appears
		// in ListFilesFor and DownloadSelected.
		m_Status = "Searching Hugging Face...";
		// Every captured value is by value; only `this` is captured by reference (as a
		// pointer). The lifetime guarantee is the destructor's join.
		LaunchWorker(std::thread([this, query, token]() {
			// The public models API, no auth required for the search itself.
			std::string url = "https://huggingface.co/api/models?search=" + HttpClient::UrlEncode(query) +
				// filter=gguf restricts to quantised models; sort by downloads descending is what
				// makes the most-used models appear first. DEFECT: limit=25 is hard-coded and
				// there is no paging, so results past 25 are unreachable.
				"&filter=gguf&sort=downloads&direction=-1&limit=25";
			// BLOCKING network call on the worker thread -- which is exactly why this runs
			// off the UI thread. A few-second DNS or TLS stall would freeze the editor here.
			HttpResult r = HttpClient::Get(url, HFHeaders(token));
			// Taken before touching m_Results, m_Files, m_SelectedModel or m_Status, and
			// held to the end of the lambda. Deliberately coarse: the whole result swap is
			// atomic from the UI thread's point of view.
			std::lock_guard<std::mutex> lock(m_Mutex);
			// HttpResult carries the status code, body and error string. Succeeded is the
			// only field consulted here -- the status code is never surfaced to the user.
			if (r.Succeeded)
			{
				// Cleared first so a failed parse cannot leave the previous search's results on
				// screen next to a partial new list.
				m_Results.clear();
				// The mini-parser splits the JSON array into per-object substrings. Each object
				// is then queried field by field, with no schema validation.
				for (auto &obj : JsonMini::SplitArrayObjects(r.Body))
				{
					// Default-constructed, so Downloads/Likes start at 0 and Gated at false. The
					// extraction calls below overwrite only what the API actually returned.
					HFModelInfo info;
					// The id is REQUIRED -- it is the only key used to build the file-list and
					// download URLs -- so an object without one is skipped entirely.
					if (!JsonMini::ExtractString(obj, "id", info.Id))
						continue;
					// int64 because HF counts exceed 2^31 for popular models. The return value is
					// ignored: a missing field leaves the default 0, which is close enough for a
					// sort key the user is only reading.
					JsonMini::ExtractInt64(obj, "downloads", info.Downloads);
					JsonMini::ExtractInt64(obj, "likes", info.Likes);
					// The gated field is a BOOLEAN in the API, not a string -- ExtractRawToken grabs
					// the raw token so `"gated": false` can be distinguished from `"gated": true`.
					std::string gated;
					if (JsonMini::ExtractRawToken(obj, "gated", gated))
						// Any token other than the literal "false" counts as gated. That misclassifies
						// a missing field as gated=true, and would also treat "False" as gated.
						info.Gated = (gated != "false");
					m_Results.push_back(info);
				}
				m_SelectedModel = -1;
				m_Files.clear();
				// The count is the user-visible feedback. Includes only the objects that had an
				// id, so it can be lower than the API's own result count.
				m_Status = std::to_string(m_Results.size()) + " models found.";
			}
			else
			{
				// The worker's error string, propagated verbatim. Nothing is logged elsewhere,
				// so this status line is the ONLY record of a failed search.
				m_Status = "Search failed: " + r.Error;
			}
			// Cleared last, INSIDE the mutex scope. That re-enables the buttons via the
			// atomic flag; it does not need the lock, but clearing it early would allow a
			// click that raced the still-running tail of this lambda.
			m_Searching.store(false);
		}));
	}

	// Lists a repo's files on a worker thread. `index` is into the CURRENT m_Results,
	// which is why the worker re-captures it and the UI re-validates it.
	void ModelDownloaderPanel::ListFilesFor(int index)
	{
		if (m_Searching.load() || m_Listing.load() || m_Downloading.load())
			return;
		// A COPY of m_Results, made under the lock and used after the lock is released,
		// so the bounds check below does not hold the mutex during a network call.
		std::vector<HFModelInfo> results;
		{
			// Scoped to the copy only -- an inner block, unlike Search(). The reason is that
			// this function must not block the UI thread while a request is in flight.
			std::lock_guard<std::mutex> lock(m_Mutex);
			// Copied because the worker outlives this scope and m_Results can change.
			results = m_Results;
		}
		// Bounds check against the SNAPSHOT. The UI thread could have changed
		// m_SelectedModel while the copy was being taken.
		if (index < 0 || index >= (int)results.size())
			return;
		// The repo id, e.g. "org/model". Captured by value for the worker.
		std::string id = results[index].Id;
		// The checkbox state, captured so the worker does not read a UI-owned bool.
		bool ggufOnly = m_GgufOnly;
		std::string token = ApiKeyStore::Instance().HuggingFaceToken;
		m_Listing.store(true);
		// Same unlocked m_Status write as in Search -- see the DEFECT note there.
		m_Status = "Listing files for " + id + "...";
		LaunchWorker(std::thread([this, id, index, ggufOnly, token]() {
			// The repo-info endpoint, which lists sibling files in its response.
			std::string url = "https://huggingface.co/api/models/" + HttpClient::UrlEncode(id);
			// The comment records a real decision. HttpClient::UrlEncode percent-encodes
			// everything outside [A-Za-z0-9-_.~], so the '/' in "org/model" becomes %2F.
			// UrlEncode leaves '/' unencoded? No — it encodes '/'. Repo ids need the slash kept.
			// The fix below decodes ONLY %2F back to '/' and leaves every other escape
			// intact -- correct for this URL shape.
			// Fix: restore encoded slashes.
			// Rebuilt by hand rather than using a decode function, because decoding
			// everything would also undo escapes the API needs.
			std::string fixed;
			// Linear scan over the whole URL. Cheap at this length.
			for (size_t i = 0; i < url.size(); i++)
			{
				// Matches the uppercase %2F UrlEncode produces. DEFECT: it is case-sensitive --
				// if UrlEncode ever emitted lowercase %2f this would miss it and the request
				// would 404.
				if (i + 2 < url.size() && url[i] == '%' && url[i + 1] == '2' && url[i + 2] == 'F')
				{
					// The restored separator.
					fixed += '/';
					// Skips the "2F" so the loop does not re-examine them. The loop's own i++ then
					// moves past the whole escape.
					i += 2;
				}
				else
					fixed += url[i];
			}
			// The repaired URL. Auth is the same as for search.
			HttpResult r = HttpClient::Get(fixed, HFHeaders(token));
			// Held across the whole parse below, which is pure CPU work on an already
			// received body.
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (r.Succeeded)
			{
				m_Files.clear();
				// Substring search for the siblings array rather than a full JSON walk.
				size_t sib = r.Body.find("\"siblings\"");
				// Takes everything from "siblings" to the end of the body. DEFECT: any
				// "rfilename" appearing AFTER the siblings array ends -- in a cardData or
				// config block -- would also be picked up as a file.
				std::string sub = (sib == std::string::npos) ? "" : r.Body.substr(sib);
				size_t pos = 0;
				// Scans for the key rather than parsing an array. Robust to the API reordering
				// fields, and tolerant of syntax this parser does not understand.
				while ((pos = sub.find("\"rfilename\"", pos)) != std::string::npos)
				{
					std::string name;
					// A copy of the REMAINDER from this key onward, not just the object. That is
					// what lets ExtractString search forward for the value.
					std::string piece = sub.substr(pos);
					// The value follows the key in the same object. The empty check rejects a key
					// with a missing value.
					if (JsonMini::ExtractString(piece, "rfilename", name) && !name.empty())
					{
						// The GGUF-only filter, using the case-insensitive helper. With the checkbox off,
						// every file in the repo is listed -- including README.md and configs.
						if (!ggufOnly || EndsWithNoCase(name, ".gguf"))
						{
							HFFileEntry e;
							e.Name = name;
							// DEFECT: every .gguf is pre-ticked, regardless of the ggufOnly checkbox. With
							// ggufOnly OFF, the .gguf files are selected and the rest are not -- which is
							// arguably right, but it means "download selected" defaults to every quant in
							// the repo, potentially tens of gigabytes.
							e.Selected = EndsWithNoCase(name, ".gguf");
							m_Files.push_back(e);
						}
						// 11 == strlen("\"rfilename\""), including both quotes. A magic number tied
						// to the key spelling: rename the key in the parser and this skips too far.
						pos += 11;
					}
					else
						pos += 11;
				}
				// Set here as well as from the UI thread. This makes the worker the authority,
				// so the selection survives even if the UI thread's write is lost to a race.
				m_SelectedModel = index;
				// Reports the FILTERED count, so a user with ggufOnly on sees fewer files than
				// the repo actually has.
				m_Status = std::to_string(m_Files.size()) + " files listed for " + id + ".";
			}
			else
			{
				// Same error-surfacing pattern as Search.
				m_Status = "File list failed: " + r.Error;
			}
			// Re-enables the UI. Set last, mirroring m_Searching in Search().
			m_Listing.store(false);
		}));
	}

	// Downloads every ticked file sequentially on a worker thread. Sequential, not
	// parallel: HF rate-limits aggressively and parallel downloads get 429s.
	void ModelDownloaderPanel::DownloadSelected()
	{
		if (m_Searching.load() || m_Listing.load() || m_Downloading.load())
			return;
		// Another snapshot copy, for the same reason as in ListFilesFor.
		std::vector<HFModelInfo> results;
		// The ticked filenames, copied under the lock so the worker does not depend on
		// m_Files staying put.
		std::vector<std::string> files;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			results = m_Results;
			// Reads the Selected flags from the copy... which is then discarded, because the
			// worker only uses `files`. The model-list copy is used, this one is not.
			for (auto &f : m_Files)
			{
				if (f.Selected)
					// Names only -- the worker re-derives the destination from destDir + sub + name.
					files.push_back(f.Name);
			}
		}
		// Three preconditions in one guard: a model must be selected, the index must be
		// valid against the snapshot, and at least one file must be ticked.
		if (m_SelectedModel < 0 || m_SelectedModel >= (int)results.size() || files.empty())
		{
			// The rejection message. Reached only from the UI thread, so the unlocked write
			// races less here -- but it is still the same unlocked m_Status pattern.
			m_Status = "Select a model and at least one file first.";
			return;
		}
		// Re-validated against the snapshot above, so this index is safe.
		std::string id = results[m_SelectedModel].Id;
		// Downloads land in the active project's models directory unless the
		// user typed an override.
		// An empty buffer means "use the project default". Resolved to an absolute path
		// HERE, on the UI thread, so the worker does not depend on Project::Get().
		std::string destDir = m_DestDirBuf[0]
								  ? std::string(m_DestDirBuf)
								  : Project::Get().GetAssetPath("models").string();
		// Copied by value for the same reason as in the other two operations.
		std::string token = ApiKeyStore::Instance().HuggingFaceToken;
		// CRITICAL: clears any stale cancel left over from a previous download. Without
		// it, a download cancelled once would refuse to start again.
		m_CancelDownload.store(false);
		m_Downloading.store(true);
		// Everything needed is captured by value, including the token and destDir.
		LaunchWorker(std::thread([this, id, files, destDir, token]() {
			// Subfolder per repo: "org/name" -> "org_name".
			// "org/model" becomes "org_model", so each repo gets its own subfolder.
			std::string sub = id;
			// In-place sanitisation of the characters Windows forbids in a filename.
			for (char &c : sub)
			{
				// The full Windows-illegal set, plus the path separator. DEFECT: control
				// characters, and names ending in a dot or a space, are also illegal on Windows
				// and are not filtered.
				if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|')
					// Underscore rather than removal, so "a/b" and "a_b" collide -- and "org/model"
					// collides with the real repo "org_model".
					c = '_';
			}
			// Accumulated across the loop so the final status can distinguish a complete
			// download from a partial one.
			bool allOk = true;
			// Sequential, one file per iteration.
			for (size_t i = 0; i < files.size(); i++)
			{
				// Polled BETWEEN files, not during one. A cancel during a 4 GB download therefore
				// takes effect only after that file finishes -- see the note on finished below.
				if (m_CancelDownload.load())
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					// Under the lock, unlike the pre-launch status writes. The cancel path is the
					// one place in this lambda that takes the mutex for a status-only update.
					m_Status = "Download cancelled.";
					allOk = false;
					// Stops the loop. The remaining files are simply not attempted.
					break;
				}
				// A reference into the captured vector, which outlives the loop.
				const std::string &name = files[i];
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					// Drives the progress overlay's label in OnImGuiRender.
					m_CurrentFile = name;
					// "(2/5)" style progress. Readable, and it makes a stalled multi-file download
					// obvious -- though it does not say WHICH bytes are done.
					m_Status = "Downloading " + name + " (" + std::to_string(i + 1) + "/" + std::to_string(files.size()) + ")...";
				}
				// The resolve endpoint streams file content. NOTE: unlike the search and list
				// URLs, this one is NOT passed through UrlEncode, because the path components
				// must stay literal. A filename with a space or a '#' would break the URL.
				std::string url = "https://huggingface.co/" + id + "/resolve/main/" + name;
				// destDir/sub/name. HttpClient::DownloadFile creates the parent directory, so
				// this does not need an explicit create_directories.
				std::filesystem::path dest = std::filesystem::path(destDir) / sub / name;
				// Out-param filled by DownloadFile. Empty on success.
				std::string error;
				// Passed to DownloadFile, which sets it true on exit. DEFECT: the worker never
				// READS it -- the loop relies on DownloadFile returning instead -- so this flag
				// is write-only and its only purpose is to satisfy the signature.
				std::atomic<bool> finished{false};
				// Reset per file, so the progress bar does not carry the previous file's total.
				m_ProgressCurrent.store(0);
				// Zero total is what makes the UI show the indeterminate branch rather than a
				// division by zero in the bar's fraction.
				m_ProgressTotal.store(0);
				// Streams to destPath + ".part", then renames -- so a cancelled or failed
				// download never leaves a half-written .gguf that the loader would try to read.
				HttpClient::DownloadFile(url, dest, HFHeaders(token), m_ProgressCurrent, m_ProgressTotal,
					// The panel's own atomic is passed BY REFERENCE, so the worker's writes are
					// visible to the UI thread's Cancel button and progress reads.
					m_CancelDownload, finished, error);
				// Any failure stops the whole batch. A user who ticked 5 files and had the 3rd
				// fail gets 1, 2 downloaded and 4, 5 skipped.
				if (!error.empty())
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					// Names the file that failed, which is the useful part in a multi-file batch.
					m_Status = "Failed " + name + ": " + error;
					allOk = false;
					break;
				}
			}
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				// Only reached if every file succeeded -- a cancel or error breaks before this.
				if (allOk)
				{
					// The success message, including the resolved destination so the user can see
					// where a default-path download actually landed.
					m_Status = "Downloaded " + std::to_string(files.size()) + " file(s) to " +
						(std::filesystem::path(destDir) / sub).string() + ".";
					// Clears the overlay label. DEFECT: this only happens on the allOk path, so after
					// a failed or cancelled download the progress overlay keeps showing the last
					// file name indefinitely.
					m_CurrentFile.clear();
				}
			}
			// Re-enables the UI. Last statement, and outside the lock scope -- which is fine
			// because it is an atomic.
			m_Downloading.store(false);
		}));
	}

	// Copies the stored token into the ImGui text buffer. Called once, lazily.
	void ModelDownloaderPanel::RefreshTokenBuffer()
	{
		// Same MSVC-only strncpy_s as the constructor, and the same _TRUNCATE semantics.
		// A token longer than 511 chars is silently cut rather than rejected.
		strncpy_s(m_TokenBuf, sizeof(m_TokenBuf), ApiKeyStore::Instance().HuggingFaceToken.c_str(), _TRUNCATE);
	}

	// Draws the panel. Called every frame while visible; all the real work happens
	// on the worker, so this function must never block.
	void ModelDownloaderPanel::OnImGuiRender()
	{
		// The panel is skipped entirely when hidden, so its cost is one bool test.
		if (!m_Visible)
			return;

		// Passing &m_Visible gives ImGui a close button, which sets the flag directly.
		// The bool test above then skips the body next frame.
		ImGui::Begin("Model Downloader", &m_Visible);

		// Collapsed by default, so the token is not exposed on screen unless asked for.
		if (ImGui::CollapsingHeader("Hugging Face token (optional)"))
		{
			// DEFECT: a function-local static, so the flag survives this panel's destruction
			// and applies to every ModelDownloaderPanel instance for the rest of the
			// process. A second panel would never populate its token buffer. A member
			// variable would scope it correctly.
			static bool tokenInit = false;
			// One-shot initialisation, so the buffer is not overwritten every frame while
			// the user is typing into it.
			if (!tokenInit)
			{
				// Seeds m_TokenBuf from the store exactly once per process (see the static above).
				RefreshTokenBuffer();
				// Set even if the store was empty, so an empty buffer is not re-seeded each frame.
				tokenInit = true;
			}
			// The Password flag masks the characters; Show toggles it off for copy/paste.
			ImGuiInputTextFlags flags = m_ShowToken ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password;
			// Binds directly to the member buffer, which is why the panel copies the store
			// into it rather than using an ImGui-owned string.
			ImGui::InputText("HF token", m_TokenBuf, sizeof(m_TokenBuf), flags);
			ImGui::SameLine();
			// Same line as the field, so the eye toggle sits next to the text.
			ImGui::Checkbox("Show", &m_ShowToken);
			// Help text. Names the two reasons a user would bother, and where to get a token.
			ImGui::TextWrapped("Needed for gated repos and higher rate limits. Get one at huggingface.co/settings/tokens. Stored in %%APPDATA%%\\Wasteland\\ai_keys.cfg (never committed).");
			// Writes the buffer into the singleton and flushes it to disk immediately.
			if (ImGui::Button("Save token"))
			{
				// Copies out of the ImGui buffer, so later typing does not mutate the store.
				ApiKeyStore::Instance().HuggingFaceToken = m_TokenBuf;
				// Synchronous file write on the UI thread. Small, but it is disk I/O in the
				// frame loop.
				ApiKeyStore::Instance().Save();
				m_Status = "Token saved.";
			}
			ImGui::SameLine();
			// Clears both the buffer and the store, then saves -- so the file on disk is
			// scrubbed too rather than only the in-memory copy.
			if (ImGui::Button("Clear token"))
			{
				// First byte only: the buffer is a char array, so this is an empty C string.
				m_TokenBuf[0] = '\0';
				// Cleared from the singleton as well, so a later RefreshTokenBuffer cannot
				// resurrect the old value.
				ApiKeyStore::Instance().HuggingFaceToken.clear();
				ApiKeyStore::Instance().Save();
				m_Status = "Token cleared.";
			}
		}

		// Visual break between the token block and the search controls.
		ImGui::Separator();
		// The query box. Its contents are copied into the worker lambda on submit.
		ImGui::InputText("Search", m_SearchBuf, sizeof(m_SearchBuf));
		ImGui::SameLine();
		// The same three-flag test the worker-launching functions use. Read once here
		// and reused for both button groups.
		bool busy = m_Searching.load() || m_Listing.load() || m_Downloading.load();
		// greys out the Search button rather than hiding it, so the layout does not jump
		// when a search starts.
		ImGui::BeginDisabled(busy);
		if (ImGui::Button("Search##hf_search_btn"))
			Search();
		// Must be paired with BeginDisabled, or every later widget inherits the state.
		ImGui::EndDisabled();
		ImGui::SameLine();
		// Applied at LIST time, not search time -- changing it does not re-list until
		// the user clicks the model again.
		ImGui::Checkbox("GGUF only", &m_GgufOnly);

		// Section marker. The snapshot below exists so the draw loop does not hold the
		// mutex while ImGui runs -- ImGui calls can be slow and must not block a worker.
		// Results.
		std::vector<HFModelInfo> results;
		std::vector<HFFileEntry> files;
		std::string status;
		// Local copy of the selection, used to decide which row is highlighted.
		int selected = -1;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// Full copies of both vectors and the status string, taken under the lock and
			// then used lock-free for the rest of the frame.
			results = m_Results;
			files = m_Files;
			// Copied so the final TextWrapped does not read m_Status without the lock --
			// which is what makes the unlocked writes in Search/ListFilesFor racy.
			status = m_Status;
			selected = m_SelectedModel;
		}
		// A 160px-tall scrollable child. ImVec2(0, h) means "full available width".
		// The '##' prefix hides the label.
		ImGui::BeginChild("##hf_results", ImVec2(0, 160), true);
		// Iterates the SNAPSHOT, not m_Results, so a worker completing mid-frame cannot
		// invalidate the iterator.
		for (int i = 0; i < (int)results.size(); i++)
		{
			// Fixed buffer for the row text. The id alone can be long, so 512 is generous.
			char label[512];
			// Bounded formatting -- snprintf, so a pathologically long id truncates instead
			// of overflowing. %lld matches the int64_t fields.
			snprintf(label, sizeof(label), "%s  (dl: %lld, likes: %lld%s)", results[i].Id.c_str(),
				// Explicit casts because %lld expects long long and the fields are int64_t.
				(long long)results[i].Downloads, (long long)results[i].Likes,
				// Gated models need a token, which is why this marker is on the row itself.
				results[i].Gated ? ", gated" : "");
			// Returns true on the frame the row is clicked. Comparison against the local
			// `selected` gives the highlight.
			if (ImGui::Selectable(label, selected == i))
			{
				// Updated locally first so the highlight applies on this very frame.
				selected = i;
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					// Then published under the lock, for the worker and for DownloadSelected.
					m_SelectedModel = i;
				}
				// Kicks off the file listing from inside the draw loop. It joins any previous
				// worker via LaunchWorker, so this can BLOCK the frame -- the same hitch risk
				// the destructor has.
				ListFilesFor(i);
			}
		}
		// Closes the results scroll region. Unbalanced Begin/End here would break every
		// subsequent ImGui call.
		ImGui::EndChild();

		// Three guards before indexing `results[selected]` -- each one independently
		// prevents an out-of-bounds read.
		if (!results.empty() && selected >= 0 && selected < (int)results.size())
		{
			// Names the repo whose files are listed below.
			ImGui::TextWrapped("Files for %s:", results[selected].Id.c_str());
// The open-in-browser button is Windows-only, matching the ShellExecuteA include
// at the top of the file.
#ifdef WL_PLATFORM_WINDOWS
			// Places the button beside the label rather than on its own row.
			ImGui::SameLine();
			// Opens the repo page in the default browser. Read-only convenience -- it does
			// not touch any panel state.
			if (ImGui::SmallButton("Open on huggingface.co"))
			{
				// The raw id is NOT url-encoded here, but a repo id is already URL-safe, so that
				// is correct.
				std::string page = "https://huggingface.co/" + results[selected].Id;
				// Verb "open" hands off to the shell's default handler. SW_SHOWNORMAL avoids
				// a minimised or hidden window.
				ShellExecuteA(nullptr, "open", page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
#endif
		}

		// A second scroll region for the per-file checkboxes, same sizing as results.
		ImGui::BeginChild("##hf_files", ImVec2(0, 160), true);
		{
			// DEFECT: the mutex is HELD for the entire loop below, including all the ImGui
			// checkbox drawing. This is the opposite of the snapshot pattern used above, and
			// it means a worker thread completing a download blocks on this lock while the
			// UI draws -- and vice versa.
			std::lock_guard<std::mutex> lock(m_Mutex);
			// Reads m_FILES directly rather than the `files` copy taken earlier, because the
			// checkboxes are two-way: the UI must write the flags back.
			for (size_t i = 0; i < m_Files.size(); i++)
			{
				// Scopes the widget id to this row. Without it, two files with the same name in
				// different repos would collide in ImGui's id space.
				ImGui::PushID((int)i);
				// The filename is the label, so the user sees what they are ticking. The bool
				// pointer writes straight into m_Files under the lock.
				ImGui::Checkbox(m_Files[i].Name.c_str(), &m_Files[i].Selected);
				// Must balance the PushID above, or every subsequent widget gets a shifted id.
				ImGui::PopID();
			}
		}
		// Closes the file list region.
		ImGui::EndChild();

		// Empty buffer means "the active project's models directory"; show
		// what that resolves to so the destination is never a mystery.
		// Empty means "project default". The two branches below show the field or the
		// resolved path, but neither lets the user CLEAR a typed override back to empty
		// once set -- there is no way to return to the default from the UI.
		if (m_DestDirBuf[0])
			// A free-text path with no browse button and no validation.
			ImGui::InputText("Download to", m_DestDirBuf, sizeof(m_DestDirBuf));
		else
			// Shows the resolved default read-only. The TextDisabled variant greys it, which
			// is how the user can tell it is not editable.
			ImGui::TextDisabled("Download to: %s",
								// Called on the UI thread. This is the SAME expression DownloadSelected uses,
								// so the two cannot disagree about the destination.
								Project::Get().GetAssetPath("models").string().c_str());

		// Sampled once: the Cancel button and progress block below must agree about
		// whether a download is running for the whole frame.
		bool downloading = m_Downloading.load();
		// Same guard as the Search button, using the same `busy` value.
		ImGui::BeginDisabled(busy);
		// Enabled only when a model is selected and at least one file is ticked.
		if (ImGui::Button("Download selected"))
			DownloadSelected();
		// Paired with the BeginDisabled above.
		ImGui::EndDisabled();
		// The cancel-and-progress region. Rendered only while a download is active, so
		// the layout does not reserve space for it when idle.
		if (downloading)
		{
			ImGui::SameLine();
			// Sets the flag the worker polls -- it does not join or interrupt the worker.
			if (ImGui::Button("Cancel"))
				// Cooperative cancellation only. A single in-flight chunk still completes before
				// the worker notices, so a large file does not stop immediately.
				m_CancelDownload.store(true);
			uint64_t cur = m_ProgressCurrent.load();
			uint64_t tot = m_ProgressTotal.load();
			// A copy of the current filename for the overlay text.
			std::string file;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				// Under the lock, unlike m_Status which is read from the earlier snapshot.
				file = m_CurrentFile;
			}
			// With a known total, a determinate bar; without one, the indeterminate branch
			// below. The zero case is what stops the fraction dividing by zero.
			if (tot > 0)
			{
				// Fixed buffer, 128 bytes. Enough for a long filename plus two byte counts --
				// snprintf truncates rather than overflowing if a repo name is pathological.
				char overlay[128];
				// Progress in whole MB, so a 900 KB file shows "0 MB" for its entire download.
				snprintf(overlay, sizeof(overlay), "%s: %llu / %llu MB", file.c_str(),
					// Integer division by 1024*1024, matching the %llu format. The casts are
					// required because the values are uint64_t.
					(unsigned long long)(cur / (1024 * 1024)), (unsigned long long)(tot / (1024 * 1024)));
				// The double casts avoid 32-bit float overflow on a large total. ImVec2(-FLT_MIN,
				// 0) is ImGui's idiom for "full width" -- a hairless width would be clipped.
				ImGui::ProgressBar((float)((double)cur / (double)tot), ImVec2(-FLT_MIN, 0), overlay);
			}
			// Total still unknown: HF sends no Content-Length for a chunked response, or the
			// download has not received headers yet.
			else
			{
				char overlay[128];
				// Shows downloaded-so-far with an ellipsis, signalling that the total is unknown.
				snprintf(overlay, sizeof(overlay), "%s: %llu MB...", file.c_str(),
					(unsigned long long)(cur / (1024 * 1024)));
				// A fraction of 0 with an explicit overlay: ImGui would otherwise draw its own
				// default "0.0" text over the filename.
				ImGui::ProgressBar(0.0f, ImVec2(-FLT_MIN, 0), overlay);
			}
		}

		// The status line, from the snapshot taken at the top of the frame. Always
		// rendered last, so it is the last thing visible when something goes wrong.
		ImGui::TextWrapped("Status: %s", status.c_str());

		// Closes the panel. Must be called on every path that called Begin, or ImGui's
		// internal stack desynchronises.
		ImGui::End();
	}

}
