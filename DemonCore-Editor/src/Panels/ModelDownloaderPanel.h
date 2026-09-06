#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Wasteland
{

	struct HFModelInfo
	{
		std::string Id;
		int64_t Downloads = 0;
		int64_t Likes = 0;
		bool Gated = false;
	};

	struct HFFileEntry
	{
		std::string Name;
		bool Selected = false;
	};

	// "Model Downloader" page: search Hugging Face for GGUF models,
	// list their files, and download .gguf weights into assets/models.
	// Optional HF token via ApiKeyStore for gated repos / higher limits.
	class ModelDownloaderPanel
	{
	public:
		ModelDownloaderPanel();
		~ModelDownloaderPanel();

		void OnImGuiRender();

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }

	private:
		void LaunchWorker(std::thread &&t);
		void Search();
		void ListFilesFor(int index);
		void DownloadSelected();
		void RefreshTokenBuffer();

	private:
		bool m_Visible = true;

		char m_SearchBuf[256] = {0};
		char m_TokenBuf[512] = {0};
		bool m_ShowToken = false;
		bool m_GgufOnly = true;
		char m_DestDirBuf[256] = {"assets/models"};

		std::vector<HFModelInfo> m_Results;
		int m_SelectedModel = -1;
		std::vector<HFFileEntry> m_Files;

		std::mutex m_Mutex;
		std::thread m_Worker;
		std::atomic<bool> m_Searching{false};
		std::atomic<bool> m_Listing{false};
		std::atomic<bool> m_Downloading{false};
		std::atomic<bool> m_CancelDownload{false};
		std::atomic<uint64_t> m_ProgressCurrent{0};
		std::atomic<uint64_t> m_ProgressTotal{0};
		std::string m_Status = "Search for a model, pick .gguf files, download.";
		std::string m_CurrentFile;
	};

}
