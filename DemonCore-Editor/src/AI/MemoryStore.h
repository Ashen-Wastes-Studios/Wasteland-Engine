#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace Wasteland
{
    // A single durable memory entry written by the agent or the user.
    struct MemoryEntry
    {
        std::string Key;         // unique id (timestamp + random suffix)
        std::string Category;    // "lesson", "preference", "correction", "snippet", "tool_telemetry"
        std::string Text;        // human-readable content
        int64_t Timestamp = 0;   // unix seconds
        float Weight = 1.0f;     // higher = more important; decays over time
        std::vector<std::string> Tags;
        bool UserAuthored = false;
    };

    // Summary stats returned by memory_read tools.
    struct MemoryStats
    {
        size_t TotalEntries = 0;
        size_t Lessons = 0;
        size_t Preferences = 0;
        size_t Corrections = 0;
        size_t Snippets = 0;
        size_t ToolTelemetry = 0;
        size_t UserEntries = 0;
    };

    // Self-improvement layer: the agent's long-term memory across sessions.
    // Files live under the active project's `ai_memory/` directory:
    //   project_memory.json, tool_telemetry.json, skill_history.json
    // A global store also lives in %APPDATA%\Wasteland\ai_memory.json.
    class MemoryStore
    {
    public:
        // Lazily initializes the store path. Safe to call multiple times.
        static void Initialize();

        // Returns true if the store directory exists and is writable.
        static bool IsAvailable();

        // Project-layer accessors. Thread-safe.
        static MemoryStats ProjectStats();
        static std::vector<MemoryEntry> ProjectRead(const std::string &category);
        static void ProjectAppend(const MemoryEntry &entry);
        static void ProjectAppend(const std::string &text, const std::string &category, const std::string &key);
        static void ProjectSearch(const std::string &query, std::vector<MemoryEntry> &out);
        static void ProjectForget(const std::string &key);

        // Global-layer accessors. Thread-safe.
        static MemoryStats GlobalStats();
        static std::vector<MemoryEntry> GlobalRead(const std::string &category);
        static void GlobalAppend(const MemoryEntry &entry);
        static void GlobalForget(const std::string &key);

        // Build a system-prompt snippet from the most important memory entries,
        // sorted by weight and capped to the supplied character limit.
        static std::string BuildPromptMemory(size_t maxChars = 4096);

    private:
        static std::vector<MemoryEntry> ReadTextFile(const std::filesystem::path &file);
        static void WriteTextFile(const std::filesystem::path &file, const std::vector<MemoryEntry> &entries);
        static MemoryStats ComputeStats(const std::vector<MemoryEntry> &entries);
        static std::string GenerateKey();

        static std::filesystem::path s_ProjectMemoryDir;
        static std::filesystem::path s_GlobalMemoryPath;
        static std::mutex s_ProjectMutex;
        static std::mutex s_GlobalMutex;
        static bool s_Initialized;
    };

}
