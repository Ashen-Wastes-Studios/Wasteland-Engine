#include "wlpch.h"
#include "ModelDownloaderPanel.h"
#include "ApiKeyStore.h"
#include "HttpClient.h"
#include "JsonMini.h"

#include <imgui/imgui.h>

#ifdef WL_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace Wasteland
{

	static bool EndsWith(const std::string &s, const std::string &suffix)
	{
		if (suffix.size() > s.size())
			return false;
		return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	static bool EndsWithNoCase(const std::string &s, const std::string &suffix)
	{
		if (suffix.size() > s.size())
			return false;
		size_t off = s.size() - suffix.size();
		for (size_t i = 0; i < suffix.size(); i++)
		{
			if (tolower((unsigned char)s[off + i]) != tolower((unsigned char)suffix[i]))
				return false;
		}
		return true;
	}

	ModelDownloaderPanel::ModelDownloaderPanel()
	{
		strncpy_s(m_SearchBuf, sizeof(m_SearchBuf), "gguf", _TRUNCATE);
	}

	ModelDownloaderPanel::~ModelDownloaderPanel()
	{
		m_CancelDownload.store(true);
		if (m_Worker.joinable())
			m_Worker.join();
	}

	void ModelDownloaderPanel::LaunchWorker(std::thread &&t)
	{
		if (m_Worker.joinable())
			m_Worker.join();
		m_Worker = std::move(t);
	}

	static HttpHeaders HFHeaders(const std::string &token)
	{
		HttpHeaders h;
		if (!token.empty())
			h.emplace_back("Authorization", "Bearer " + token);
		return h;
	}

	void ModelDownloaderPanel::Search()
	{
		if (m_Searching.load() || m_Listing.load() || m_Downloading.load())
			return;
		std::string query = m_SearchBuf;
		std::string token = ApiKeyStore::Instance().HuggingFaceToken;
		m_Searching.store(true);
		m_Status = "Searching Hugging Face...";
		LaunchWorker(std::thread([this, query, token]() {
			std::string url = "https://huggingface.co/api/models?search=" + HttpClient::UrlEncode(query) +
				"&filter=gguf&sort=downloads&direction=-1&limit=25";
			HttpResult r = HttpClient::Get(url, HFHeaders(token));
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (r.Succeeded)
			{
				m_Results.clear();
				for (auto &obj : JsonMini::SplitArrayObjects(r.Body))
				{
					HFModelInfo info;
					if (!JsonMini::ExtractString(obj, "id", info.Id))
						continue;
					JsonMini::ExtractInt64(obj, "downloads", info.Downloads);
					JsonMini::ExtractInt64(obj, "likes", info.Likes);
					std::string gated;
					if (JsonMini::ExtractRawToken(obj, "gated", gated))
						info.Gated = (gated != "false");
					m_Results.push_back(info);
				}
				m_SelectedModel = -1;
				m_Files.clear();
				m_Status = std::to_string(m_Results.size()) + " models found.";
			}
			else
			{
				m_Status = "Search failed: " + r.Error;
			}
			m_Searching.store(false);
		}));
	}

	void ModelDownloaderPanel::ListFilesFor(int index)
	{
		if (m_Searching.load() || m_Listing.load() || m_Downloading.load())
			return;
		std::vector<HFModelInfo> results;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			results = m_Results;
		}
		if (index < 0 || index >= (int)results.size())
			return;
		std::string id = results[index].Id;
		bool ggufOnly = m_GgufOnly;
		std::string token = ApiKeyStore::Instance().HuggingFaceToken;
		m_Listing.store(true);
		m_Status = "Listing files for " + id + "...";
		LaunchWorker(std::thread([this, id, index, ggufOnly, token]() {
			std::string url = "https://huggingface.co/api/models/" + HttpClient::UrlEncode(id);
			// UrlEncode leaves '/' unencoded? No — it encodes '/'. Repo ids need the slash kept.
			// Fix: restore encoded slashes.
			std::string fixed;
			for (size_t i = 0; i < url.size(); i++)
			{
				if (i + 2 < url.size() && url[i] == '%' && url[i + 1] == '2' && url[i + 2] == 'F')
				{
					fixed += '/';
					i += 2;
				}
				else
					fixed += url[i];
			}
			HttpResult r = HttpClient::Get(fixed, HFHeaders(token));
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (r.Succeeded)
			{
				m_Files.clear();
				size_t sib = r.Body.find("\"siblings\"");
				std::string sub = (sib == std::string::npos) ? "" : r.Body.substr(sib);
				size_t pos = 0;
				while ((pos = sub.find("\"rfilename\"", pos)) != std::string::npos)
				{
					std::string name;
					std::string piece = sub.substr(pos);
					if (JsonMini::ExtractString(piece, "rfilename", name) && !name.empty())
					{
						if (!ggufOnly || EndsWithNoCase(name, ".gguf"))
						{
							HFFileEntry e;
							e.Name = name;
							e.Selected = EndsWithNoCase(name, ".gguf");
							m_Files.push_back(e);
						}
						pos += 11;
					}
					else
						pos += 11;
				}
				m_SelectedModel = index;
				m_Status = std::to_string(m_Files.size()) + " files listed for " + id + ".";
			}
			else
			{
				m_Status = "File list failed: " + r.Error;
			}
			m_Listing.store(false);
		}));
	}

	void ModelDownloaderPanel::DownloadSelected()
	{
		if (m_Searching.load() || m_Listing.load() || m_Downloading.load())
			return;
		std::vector<HFModelInfo> results;
		std::vector<std::string> files;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			results = m_Results;
			for (auto &f : m_Files)
			{
				if (f.Selected)
					files.push_back(f.Name);
			}
		}
		if (m_SelectedModel < 0 || m_SelectedModel >= (int)results.size() || files.empty())
		{
			m_Status = "Select a model and at least one file first.";
			return;
		}
		std::string id = results[m_SelectedModel].Id;
		std::string destDir = m_DestDirBuf[0] ? m_DestDirBuf : "assets/models";
		std::string token = ApiKeyStore::Instance().HuggingFaceToken;
		m_CancelDownload.store(false);
		m_Downloading.store(true);
		LaunchWorker(std::thread([this, id, files, destDir, token]() {
			// Subfolder per repo: "org/name" -> "org_name".
			std::string sub = id;
			for (char &c : sub)
			{
				if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|')
					c = '_';
			}
			bool allOk = true;
			for (size_t i = 0; i < files.size(); i++)
			{
				if (m_CancelDownload.load())
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					m_Status = "Download cancelled.";
					allOk = false;
					break;
				}
				const std::string &name = files[i];
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					m_CurrentFile = name;
					m_Status = "Downloading " + name + " (" + std::to_string(i + 1) + "/" + std::to_string(files.size()) + ")...";
				}
				std::string url = "https://huggingface.co/" + id + "/resolve/main/" + name;
				std::filesystem::path dest = std::filesystem::path(destDir) / sub / name;
				std::string error;
				std::atomic<bool> finished{false};
				m_ProgressCurrent.store(0);
				m_ProgressTotal.store(0);
				HttpClient::DownloadFile(url, dest, HFHeaders(token), m_ProgressCurrent, m_ProgressTotal,
					m_CancelDownload, finished, error);
				if (!error.empty())
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					m_Status = "Failed " + name + ": " + error;
					allOk = false;
					break;
				}
			}
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (allOk)
				{
					m_Status = "Downloaded " + std::to_string(files.size()) + " file(s) to " +
						(std::filesystem::path(destDir) / sub).string() + ".";
					m_CurrentFile.clear();
				}
			}
			m_Downloading.store(false);
		}));
	}

	void ModelDownloaderPanel::RefreshTokenBuffer()
	{
		strncpy_s(m_TokenBuf, sizeof(m_TokenBuf), ApiKeyStore::Instance().HuggingFaceToken.c_str(), _TRUNCATE);
	}

	void ModelDownloaderPanel::OnImGuiRender()
	{
		if (!m_Visible)
			return;

		ImGui::Begin("Model Downloader", &m_Visible);

		if (ImGui::CollapsingHeader("Hugging Face token (optional)"))
		{
			static bool tokenInit = false;
			if (!tokenInit)
			{
				RefreshTokenBuffer();
				tokenInit = true;
			}
			ImGuiInputTextFlags flags = m_ShowToken ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password;
			ImGui::InputText("HF token", m_TokenBuf, sizeof(m_TokenBuf), flags);
			ImGui::SameLine();
			ImGui::Checkbox("Show", &m_ShowToken);
			ImGui::TextWrapped("Needed for gated repos and higher rate limits. Get one at huggingface.co/settings/tokens. Stored in %%APPDATA%%\\Wasteland\\ai_keys.cfg (never committed).");
			if (ImGui::Button("Save token"))
			{
				ApiKeyStore::Instance().HuggingFaceToken = m_TokenBuf;
				ApiKeyStore::Instance().Save();
				m_Status = "Token saved.";
			}
			ImGui::SameLine();
			if (ImGui::Button("Clear token"))
			{
				m_TokenBuf[0] = '\0';
				ApiKeyStore::Instance().HuggingFaceToken.clear();
				ApiKeyStore::Instance().Save();
				m_Status = "Token cleared.";
			}
		}

		ImGui::Separator();
		ImGui::InputText("Search", m_SearchBuf, sizeof(m_SearchBuf));
		ImGui::SameLine();
		bool busy = m_Searching.load() || m_Listing.load() || m_Downloading.load();
		ImGui::BeginDisabled(busy);
		if (ImGui::Button("Search##hf_search_btn"))
			Search();
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Checkbox("GGUF only", &m_GgufOnly);

		// Results.
		std::vector<HFModelInfo> results;
		std::vector<HFFileEntry> files;
		std::string status;
		int selected = -1;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			results = m_Results;
			files = m_Files;
			status = m_Status;
			selected = m_SelectedModel;
		}
		ImGui::BeginChild("##hf_results", ImVec2(0, 160), true);
		for (int i = 0; i < (int)results.size(); i++)
		{
			char label[512];
			snprintf(label, sizeof(label), "%s  (dl: %lld, likes: %lld%s)", results[i].Id.c_str(),
				(long long)results[i].Downloads, (long long)results[i].Likes,
				results[i].Gated ? ", gated" : "");
			if (ImGui::Selectable(label, selected == i))
			{
				selected = i;
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					m_SelectedModel = i;
				}
				ListFilesFor(i);
			}
		}
		ImGui::EndChild();

		if (!results.empty() && selected >= 0 && selected < (int)results.size())
		{
			ImGui::TextWrapped("Files for %s:", results[selected].Id.c_str());
#ifdef WL_PLATFORM_WINDOWS
			ImGui::SameLine();
			if (ImGui::SmallButton("Open on huggingface.co"))
			{
				std::string page = "https://huggingface.co/" + results[selected].Id;
				ShellExecuteA(nullptr, "open", page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
#endif
		}

		ImGui::BeginChild("##hf_files", ImVec2(0, 160), true);
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			for (size_t i = 0; i < m_Files.size(); i++)
			{
				ImGui::PushID((int)i);
				ImGui::Checkbox(m_Files[i].Name.c_str(), &m_Files[i].Selected);
				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		ImGui::InputText("Download to", m_DestDirBuf, sizeof(m_DestDirBuf));

		bool downloading = m_Downloading.load();
		ImGui::BeginDisabled(busy);
		if (ImGui::Button("Download selected"))
			DownloadSelected();
		ImGui::EndDisabled();
		if (downloading)
		{
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				m_CancelDownload.store(true);
			uint64_t cur = m_ProgressCurrent.load();
			uint64_t tot = m_ProgressTotal.load();
			std::string file;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				file = m_CurrentFile;
			}
			if (tot > 0)
			{
				char overlay[128];
				snprintf(overlay, sizeof(overlay), "%s: %llu / %llu MB", file.c_str(),
					(unsigned long long)(cur / (1024 * 1024)), (unsigned long long)(tot / (1024 * 1024)));
				ImGui::ProgressBar((float)((double)cur / (double)tot), ImVec2(-FLT_MIN, 0), overlay);
			}
			else
			{
				char overlay[128];
				snprintf(overlay, sizeof(overlay), "%s: %llu MB...", file.c_str(),
					(unsigned long long)(cur / (1024 * 1024)));
				ImGui::ProgressBar(0.0f, ImVec2(-FLT_MIN, 0), overlay);
			}
		}

		ImGui::TextWrapped("Status: %s", status.c_str());

		ImGui::End();
	}

}
