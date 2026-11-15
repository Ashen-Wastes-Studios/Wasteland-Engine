#include "wlpch.h"
#include "MemoryStore.h"
#include "Wasteland/Project/Project.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <shlobj.h>

namespace Wasteland
{
    std::filesystem::path MemoryStore::s_ProjectMemoryDir;
    std::filesystem::path MemoryStore::s_GlobalMemoryPath;
    std::mutex MemoryStore::s_ProjectMutex;
    std::mutex MemoryStore::s_GlobalMutex;
    bool MemoryStore::s_Initialized = false;

    void MemoryStore::Initialize()
    {
        if (s_Initialized)
            return;

        if (Project::Get().IsOpen())
        {
            s_ProjectMemoryDir = Project::Get().GetAssetPath("ai_memory");
            std::error_code ec;
            std::filesystem::create_directories(s_ProjectMemoryDir, ec);
        }

        wchar_t appData[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData)))
        {
            std::filesystem::path globalDir = std::filesystem::path(appData) / "Wasteland" / "ai_memory";
            std::error_code ec;
            std::filesystem::create_directories(globalDir, ec);
            s_GlobalMemoryPath = globalDir / "ai_memory.txt";
        }

        s_Initialized = true;
    }

    bool MemoryStore::IsAvailable()
    {
        return s_Initialized && !s_ProjectMemoryDir.empty();
    }

    MemoryStats MemoryStore::ProjectStats()
    {
        MemoryStats stats;
        if (!IsAvailable())
            return stats;

        std::lock_guard<std::mutex> lock(s_ProjectMutex);
        auto entries = ReadTextFile(s_ProjectMemoryDir / "project_memory.txt");
        stats = ComputeStats(entries);
        return stats;
    }

    std::vector<MemoryEntry> MemoryStore::ProjectRead(const std::string &category)
    {
        std::vector<MemoryEntry> out;
        if (!IsAvailable())
            return out;

        std::lock_guard<std::mutex> lock(s_ProjectMutex);
        auto entries = ReadTextFile(s_ProjectMemoryDir / "project_memory.txt");
        for (const auto &entry : entries)
        {
            if (category.empty() || entry.Category == category)
                out.push_back(entry);
        }
        return out;
    }

    void MemoryStore::ProjectAppend(const MemoryEntry &entry)
    {
        if (!IsAvailable())
            return;

        std::lock_guard<std::mutex> lock(s_ProjectMutex);
        auto path = s_ProjectMemoryDir / "project_memory.txt";
        auto entries = ReadTextFile(path);
        entries.push_back(entry);
        WriteTextFile(path, entries);
    }

    void MemoryStore::ProjectAppend(const std::string &text, const std::string &category, const std::string &key)
    {
        MemoryEntry entry;
        entry.Key = key.empty() ? GenerateKey() : key;
        entry.Text = text;
        entry.Category = category.empty() ? "lesson" : category;
        entry.Timestamp = (int64_t)std::time(nullptr);
        ProjectAppend(entry);
    }

    void MemoryStore::ProjectSearch(const std::string &query, std::vector<MemoryEntry> &out)
    {
        out.clear();
        if (!IsAvailable() || query.empty())
            return;

        std::lock_guard<std::mutex> lock(s_ProjectMutex);
        auto entries = ReadTextFile(s_ProjectMemoryDir / "project_memory.txt");

        std::string lowerQuery = query;
        std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

        for (const auto &entry : entries)
        {
            std::string lowerText = entry.Text;
            std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), ::tolower);
            if (lowerText.find(lowerQuery) != std::string::npos)
                out.push_back(entry);
        }
    }

    void MemoryStore::ProjectForget(const std::string &key)
    {
        if (!IsAvailable() || key.empty())
            return;

        std::lock_guard<std::mutex> lock(s_ProjectMutex);
        auto path = s_ProjectMemoryDir / "project_memory.txt";
        auto entries = ReadTextFile(path);
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&key](const MemoryEntry &e)
                                     { return e.Key == key; }),
                      entries.end());
        WriteTextFile(path, entries);
    }

    MemoryStats MemoryStore::GlobalStats()
    {
        MemoryStats stats;
        if (!s_Initialized || s_GlobalMemoryPath.empty())
            return stats;

        std::lock_guard<std::mutex> lock(s_GlobalMutex);
        auto entries = ReadTextFile(s_GlobalMemoryPath);
        stats = ComputeStats(entries);
        return stats;
    }

    std::vector<MemoryEntry> MemoryStore::GlobalRead(const std::string &category)
    {
        std::vector<MemoryEntry> out;
        if (!s_Initialized || s_GlobalMemoryPath.empty())
            return out;

        std::lock_guard<std::mutex> lock(s_GlobalMutex);
        auto entries = ReadTextFile(s_GlobalMemoryPath);
        for (const auto &entry : entries)
        {
            if (category.empty() || entry.Category == category)
                out.push_back(entry);
        }
        return out;
    }

    void MemoryStore::GlobalAppend(const MemoryEntry &entry)
    {
        if (!s_Initialized || s_GlobalMemoryPath.empty())
            return;

        std::lock_guard<std::mutex> lock(s_GlobalMutex);
        auto entries = ReadTextFile(s_GlobalMemoryPath);
        entries.push_back(entry);
        WriteTextFile(s_GlobalMemoryPath, entries);
    }

    void MemoryStore::GlobalForget(const std::string &key)
    {
        if (!s_Initialized || s_GlobalMemoryPath.empty() || key.empty())
            return;

        std::lock_guard<std::mutex> lock(s_GlobalMutex);
        auto entries = ReadTextFile(s_GlobalMemoryPath);
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&key](const MemoryEntry &e)
                                     { return e.Key == key; }),
                      entries.end());
        WriteTextFile(s_GlobalMemoryPath, entries);
    }

    std::string MemoryStore::BuildPromptMemory(size_t maxChars)
    {
        if (!IsAvailable())
            return {};

        std::lock_guard<std::mutex> lock(s_ProjectMutex);
        auto entries = ReadTextFile(s_ProjectMemoryDir / "project_memory.txt");

        if (entries.empty())
            return {};

        // Sort by weight descending; highest-weight lessons surface first.
        std::sort(entries.begin(), entries.end(),
                  [](const MemoryEntry &a, const MemoryEntry &b)
                  {
                      return a.Weight > b.Weight;
                  });

        std::string out = "## Persistent Memory\n\n";
        for (const auto &entry : entries)
        {
            char timeBuf[64] = {};
            tm timeInfo = {};
            localtime_s(&timeInfo, &entry.Timestamp);
            std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &timeInfo);
            std::string line = "- [" + entry.Category + "] " + entry.Text + "\n";
            if (out.size() + line.size() > maxChars)
                break;
            out += line;
        }

        return out;
    }

    std::vector<MemoryEntry> MemoryStore::ReadTextFile(const std::filesystem::path &file)
    {
        std::vector<MemoryEntry> entries;
        if (!std::filesystem::exists(file))
            return entries;

        std::ifstream in(file);
        if (!in.is_open())
            return entries;

        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty())
                continue;
            // Format: KEY|CATEGORY|TIMESTAMP|WEIGHT|USER|TEXT
            std::vector<std::string> parts;
            size_t start = 0;
            for (int sep = 0; sep < 5; ++sep)
            {
                size_t pos = line.find('|', start);
                if (pos == std::string::npos)
                    break;
                parts.push_back(line.substr(start, pos - start));
                start = pos + 1;
            }
            parts.push_back(line.substr(start));

            if (parts.size() < 6)
                continue;

            MemoryEntry entry;
            entry.Key = parts[0];
            entry.Category = parts[1];
            entry.Timestamp = std::atoll(parts[2].c_str());
            entry.Weight = (float)std::atof(parts[3].c_str());
            entry.UserAuthored = (parts[4] == "1");
            // Unescape pipe characters.
            entry.Text = parts[5];
            size_t pos = 0;
            while ((pos = entry.Text.find("\\|", pos)) != std::string::npos)
            {
                entry.Text.replace(pos, 2, "|");
                ++pos;
            }
            entries.push_back(entry);
        }
        return entries;
    }

    void MemoryStore::WriteTextFile(const std::filesystem::path &file, const std::vector<MemoryEntry> &entries)
    {
        std::ofstream out(file, std::ios::trunc);
        if (!out.is_open())
            return;

        for (const auto &entry : entries)
        {
            // Escape pipe characters in text to avoid splitting.
            std::string escapedText = entry.Text;
            size_t pos = 0;
            while ((pos = escapedText.find('|', pos)) != std::string::npos)
            {
                escapedText.replace(pos, 1, "\\|");
                pos += 2;
            }
            out << entry.Key << "|"
                << entry.Category << "|"
                << entry.Timestamp << "|"
                << std::fixed << std::setprecision(2) << entry.Weight << "|"
                << (entry.UserAuthored ? "1" : "0") << "|"
                << escapedText << "\n";
        }
    }

    MemoryStats MemoryStore::ComputeStats(const std::vector<MemoryEntry> &entries)
    {
        MemoryStats stats;
        stats.TotalEntries = entries.size();
        for (const auto &entry : entries)
        {
            if (entry.Category == "lesson")
                ++stats.Lessons;
            else if (entry.Category == "preference")
                ++stats.Preferences;
            else if (entry.Category == "correction")
                ++stats.Corrections;
            else if (entry.Category == "snippet")
                ++stats.Snippets;
            else if (entry.Category == "tool_telemetry")
                ++stats.ToolTelemetry;
            if (entry.UserAuthored)
                ++stats.UserEntries;
        }
        return stats;
    }

    std::string MemoryStore::GenerateKey()
    {
        static std::atomic<uint64_t> s_Counter{0};
        auto now = (int64_t)std::time(nullptr);
        auto counter = s_Counter.fetch_add(1);
        return std::to_string(now) + "_" + std::to_string(counter);
    }

}
