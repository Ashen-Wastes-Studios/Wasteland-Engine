#pragma once

// For the atomic flags and counters in the worker/render split. This panel
// is the reason this header needs thread-safe primitives at all: HTTP work
// happens on a worker thread while ImGui reads state on the UI thread.
#include <atomic>
// Guards the non-atomic members below -- the vectors and the std::string
// status text -- which cannot be made atomic and are shared across the
// thread boundary.
#include <mutex>
#include <string>
// For the worker thread handle held in m_Worker.
#include <thread>
#include <vector>

namespace Wasteland
{

	// One search result: a Hugging Face GGUF model repository.
	//
	// Plain data with defaults, not a class -- it is a value in a vector that
	// the UI renders directly.
	struct HFModelInfo
	{
		// The repository id, e.g. "user/model". This is the handle every later
		// API call uses, so it is the one field that must always be populated.
		std::string Id;
		// Download count as shown in the results list. 64-bit because HF counts
		// large enough to exceed a 32-bit int for popular repositories.
		//
		// Purely informational -- sorted on at most, never used in a decision.
		int64_t Downloads = 0;
		// Like count, the other half of the sort key. Like Downloads, defaulted so
		// a partially parsed JSON entry cannot leave it indeterminate.
		int64_t Likes = 0;
		// Whether the repo needs an accepted licence. Gates the download button
		// unless a token is present -- the whole reason RefreshTokenBuffer exists.
		bool Gated = false;
	};

	// One file inside a repository, with the author's selection state.
	struct HFFileEntry
	{
		// The file name as HF reports it, e.g. "Q4_K_M.gguf". The full path is NOT
		// stored -- HF file entries are repo-relative, and the panel builds the
		// download URL from the model id plus this name.
		std::string Name;
		// Whether the user ticked this file. Defaults false so nothing is
		// downloaded by accident -- a whole GGUF repo can be tens of gigabytes, so
		// requiring an explicit selection is the safe default.
		bool Selected = false;
	};

	// "Model Downloader" page: search Hugging Face for GGUF models,
	// list their files, and download .gguf weights into assets/models.
	// Optional HF token via ApiKeyStore for gated repos / higher limits.
	// The panel owns a worker thread and a set of HTTP operations. Its whole
	// design is split by that: OnImGuiRender runs on the UI thread and must
	// never block, so every operation runs on a worker and reports back through
	// the atomics and the mutex.
	class ModelDownloaderPanel
	{
	public:
		// Default-constructed. No arguments: the destination directory is read from
		// the active project at use time rather than fixed at construction.
		ModelDownloaderPanel();
		// Must outlive the worker: it has to request cancellation, join m_Worker,
		// and only then release the members the worker touches. A destructor that
		// merely let the thread die would leave it reading freed memory.
		~ModelDownloaderPanel();

		// Draws the panel when visible and drives the buttons. Called every frame
		// from the editor layer, so it must be cheap: the actual work is behind the
		// worker.
		void OnImGuiRender();

		// Inline visibility query. Trivial getter rather than a real method, so
		// there is no way for the editor layer to learn that the panel is mid-search
		// -- it has to read m_Searching itself for that.
		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }

	private:
		// Takes ownership of a thread and starts the operation.
		//
		// An rvalue reference, so the caller cannot pass an lvalue -- which forces
		// the thread to be handed over rather than silently abandoned. The panel
		// takes ownership of the thread rather than spawning detached ones, because
		// a detached thread would outlive the panel and the members below.
		void LaunchWorker(std::thread &&t);
		// Kicks off a HF model search using m_SearchBuf. Sets m_Searching so the
		// UI can disable the button instead of launching a second search.
		void Search();
		// Fetches the file list for the selected result.
		//
		// @param index  Index into m_Results, NOT an HF index -- the caller's
		//                selection. An out-of-range value must be handled by the
		//                implementation rather than the caller.
		void ListFilesFor(int index);
		// Downloads every HFFileEntry with Selected set. The only writer of
		// m_ProgressCurrent/m_ProgressTotal and m_CancelDownload.
		void DownloadSelected();
		// Copies the token from ApiKeyStore into m_TokenBuf. Separate because the
		// store can change from elsewhere and the raw buffer must not be re-read
		// from disk per request.
		void RefreshTokenBuffer();

	// A second consecutive private: label at line 46, directly after the
	// method group above. Redundant but harmless.
	private:
		// Panel visibility, initialised TRUE -- the panel shows on first open
		// rather than waiting to be summoned from a menu.
		bool m_Visible = true;

		// A fixed C array, not a std::string: it is passed straight to ImGui's
		// InputText, which wants char* plus a size. The {0} initialiser matters --
		// ImGui treats this buffer as a NUL-terminated string, so an uninitialised
		// array would scan past its end on the first frame.
		char m_SearchBuf[256] = {0};
		// The HF API token, same ImGui-buffer pattern and same zero-init
		// requirement, sized larger than the search box because a token is a longer
		// credential than a search term.
		//
		// DEFECT: a secret held in a plain char array that is not cleared on close
		// -- the panel's visibility toggle and the destructor both leave it in
		// memory. It is also plain text with no scrubbing, so it can appear in a
		// core dump or a screenshot of the panel. A std::string that is wiped on
		// destruction would be the minimum improvement.
		char m_TokenBuf[512] = {0};
		// Whether the token field renders as password dots. False by default, so
		// the credential is masked until the user deliberately reveals it.
		bool m_ShowToken = false;
		// Filter to .gguf files. Default ON, since the panel's stated purpose is
		// GGUF weights and most HF repos are safetensors or full-precision
		// checkpoints that would be useless here.
		bool m_GgufOnly = true;
		// Empty by default, which means "use the active project's
		// assets/models" -- a hardcoded default here would write downloads
		// outside whichever project is open.
		// Destination directory override. Empty by default, which means 'use the
		// active project's assets/models' -- a hardcoded default here would write
		// downloads outside whichever project happens to be open.
		char m_DestDirBuf[256] = {0};

		// Search results. NOT atomic and NOT trivially copyable, so it is one of
		// the members m_Mutex actually protects: the worker fills it and the UI
		// thread iterates it.
		std::vector<HFModelInfo> m_Results;
		// Index into m_Results, or -1 for none.
		//
		// DEFECT: a plain int with no synchronisation, while m_Results is mutex
		// protected. The worker can resize or clear m_Results -- for instance when a
		// new search replaces the old results -- at the same moment the UI thread
		// reads m_SelectedModel to index into it. That is a data race on a
		// non-atomic, and worse, the index can silently refer to a different
		// repository than the one the user clicked.
		int m_SelectedModel = -1;
		// Files of the currently selected model. Mutex-protected, same as
		// m_Results -- the worker replaces this vector wholesale when
		// ListFilesFor completes.
		std::vector<HFFileEntry> m_Files;

		// Guards the two vectors and m_Status/m_CurrentFile. Deliberately NOT a
		// recursive mutex, and every lock site must therefore avoid re-entering.
		//
		// The atomic members below deliberately do not take this lock: they are
		// polled by ImGui every frame, and an uncontended lock per poll would cost
		// more than the atomics themselves.
		std::mutex m_Mutex;
		// The worker thread handle, owned by the panel. DEFECT: no in-class
		// initialiser, and it is only ever assigned by LaunchWorker -- so a panel
		// that is destroyed without ever launching an operation holds a
		// non-joinable default thread, which is safe only because the destructor
		// does the right thing with both cases. Worth an explicit comment there
		// rather than relying on it.
		std::thread m_Worker;
		// A search is in flight. Read by ImGui to disable the search button, so
		// the user cannot launch a second search that would race the first for
		// m_Results.
		std::atomic<bool> m_Searching{false};
		// A file listing is in flight -- separate from m_Searching because a user
		// can legitimately start a listing while a search finishes, and conflating
		// them would block one behind the other.
		std::atomic<bool> m_Listing{false};
		// A download is in flight. Gates the progress bar and the cancel button.
		std::atomic<bool> m_Downloading{false};
		// Cooperative cancellation flag, set by the UI thread and polled by the
		// worker between chunks. An atomic bool rather than a condition variable
		// because the worker checks it at chunk boundaries, not on every byte.
		//
		// DEFECT: it is never reset to false anywhere in this header. If the
		// cancellation check lives in the .cpp and does not clear it on entry, the
		// next download would be cancelled immediately by a stale flag. The .cpp
		// has to reset it, and this header does not say so.
		std::atomic<bool> m_CancelDownload{false};
		// Bytes written so far. uint64_t rather than size_t because file sizes
		// here are the 64-bit values HF reports, and a 32-bit build would otherwise
		// wrap on a large model.
		std::atomic<uint64_t> m_ProgressCurrent{0};
		// Total bytes expected, from the HTTP content length.
		//
		// DEFECT: when the server sends no content length this stays 0, and a
		// progress computation dividing current by total would divide by zero. The
		// .cpp has to treat 0 as unknown rather than assuming it is set whenever
		// a download starts.
		std::atomic<uint64_t> m_ProgressTotal{0};
		// Status line shown in the panel, initialised to an instruction rather than
		// an empty string, so the panel explains itself before the first search.
		//
		// Mutex-protected, because the worker writes status text like "Downloading
		// Q4_K_M.gguf..." while the UI thread reads it every frame.
		std::string m_Status = "Search for a model, pick .gguf files, download.";
		// Name of the file currently downloading, for the status line. Empty when
		// idle. Mutex-protected like m_Status.
		std::string m_CurrentFile;
	};

}
