#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Wasteland
{
    // Parsed Hermes-format SKILL.md frontmatter.
    struct SkillInfo
    {
        std::string FilePath;
        std::string Name;
        std::string Description;
        std::string Version;
        std::string Author;
        std::string License;
        std::vector<std::string> Platforms;
        std::vector<std::string> Tags;
        std::vector<std::string> RelatedSkills;
        std::string Body; // markdown body after frontmatter
    };

    // Scans assets/ai/skills/*.md, parses Hermes YAML frontmatter, and
    // exposes the catalogue to the AI chat panel.
    class SkillLoader
    {
    public:
        // Scan the skills directory. Safe to call multiple times; the
        // catalogue is rebuilt each time so the panel sees new files.
        static void Refresh();

        // All parsed skills.
        static std::vector<SkillInfo> GetAll();

        // One skill by filename stem or skill name.
        static const SkillInfo *FindByPath(const std::string &path);
        static const SkillInfo *FindByName(const std::string &name);

        // Human-readable list of skill descriptions for the system prompt.
        static std::string GetDescriptionsMarkdown();

        // Path to the skills directory relative to the active project.
        static std::filesystem::path GetSkillsDirectory();

    private:
        // Parse one SKILL.md file. Returns empty SkillInfo on failure.
        static SkillInfo ParseFile(const std::filesystem::path &path);

        // Parse YAML frontmatter block between two --- lines.
        static std::string ParseYamlField(const std::string &yaml, const std::string &key);

        // Split a YAML list like [windows, linux, macos].
        static std::vector<std::string> ParseYamlList(const std::string &value);

        // Current catalogue. Cleared and rebuilt by Refresh().
        static std::vector<SkillInfo> s_Catalogue;
    };

}
