// SkillLoader.cpp -- scans assets/ai/skills/*.md and parses Hermes SKILL.md
// YAML frontmatter.
#include "wlpch.h"
#include "SkillLoader.h"
#include "Wasteland/Project/Project.h"

#include <fstream>
#include <sstream>
#include <algorithm>

namespace Wasteland
{
    std::vector<SkillInfo> SkillLoader::s_Catalogue;

    std::filesystem::path SkillLoader::GetSkillsDirectory()
    {
        if (Project::Get().IsOpen())
            return Project::Get().GetAssetPath("ai/skills");
        return {};
    }

    void SkillLoader::Refresh()
    {
        s_Catalogue.clear();
        std::filesystem::path dir = GetSkillsDirectory();
        if (dir.empty() || !std::filesystem::is_directory(dir))
            return;

        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dir);
             it != std::filesystem::directory_iterator(); ++it)
        {
            ec.clear();
            if (it->is_regular_file(ec) && it->path().extension() == ".md")
            {
                SkillInfo info = ParseFile(it->path());
                if (!info.Name.empty())
                    s_Catalogue.push_back(std::move(info));
            }
        }
    }

    std::vector<SkillInfo> SkillLoader::GetAll()
    {
        return s_Catalogue;
    }

    const SkillInfo *SkillLoader::FindByPath(const std::string &path)
    {
        for (const auto &info : s_Catalogue)
        {
            if (info.FilePath == path)
                return &info;
        }
        return nullptr;
    }

    const SkillInfo *SkillLoader::FindByName(const std::string &name)
    {
        for (const auto &info : s_Catalogue)
        {
            if (info.Name == name)
                return &info;
        }
        return nullptr;
    }

    std::string SkillLoader::GetDescriptionsMarkdown()
    {
        if (s_Catalogue.empty())
            return {};

        std::string out = "## Available Skills\n\n";
        for (const auto &info : s_Catalogue)
        {
            out += "- **" + info.Name + "**: " + info.Description + "\n";
            if (!info.Tags.empty())
            {
                out += "  Tags: ";
                for (size_t i = 0; i < info.Tags.size(); ++i)
                {
                    if (i)
                        out += ", ";
                    out += info.Tags[i];
                }
                out += "\n";
            }
        }
        return out;
    }

    SkillInfo SkillLoader::ParseFile(const std::filesystem::path &path)
    {
        SkillInfo info;
        info.FilePath = path.filename().string();

        std::ifstream file(path);
        if (!file.is_open())
            return info;

        std::stringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();

        // Find the frontmatter block between the first two --- lines.
        size_t start = content.find("---");
        if (start == std::string::npos)
            return info;
        start += 3;
        size_t end = content.find("---", start);
        if (end == std::string::npos)
            return info;

        std::string yaml = content.substr(start, end - start);
        // Everything after the second --- is the markdown body.
        if (end + 3 < content.size())
            info.Body = content.substr(end + 3);

        // Trim leading/trailing whitespace from body.
        size_t b = info.Body.find_first_not_of(" \t\r\n");
        if (b != std::string::npos)
            info.Body = info.Body.substr(b);

        info.Name = ParseYamlField(yaml, "name");
        info.Description = ParseYamlField(yaml, "description");
        info.Version = ParseYamlField(yaml, "version");
        info.Author = ParseYamlField(yaml, "author");
        info.License = ParseYamlField(yaml, "license");

        std::string platforms = ParseYamlField(yaml, "platforms");
        if (!platforms.empty())
            info.Platforms = ParseYamlList(platforms);

        std::string tags = ParseYamlField(yaml, "tags");
        if (!tags.empty())
            info.Tags = ParseYamlList(tags);

        std::string related = ParseYamlField(yaml, "related_skills");
        if (!related.empty())
            info.RelatedSkills = ParseYamlList(related);

        // Fallback: if no name, use filename stem.
        if (info.Name.empty())
            info.Name = path.stem().string();

        return info;
    }

    std::string SkillLoader::ParseYamlField(const std::string &yaml, const std::string &key)
    {
        std::string search = key + ":";
        size_t pos = yaml.find(search);
        if (pos == std::string::npos)
            return {};

        pos += search.size();
        // Skip whitespace.
        while (pos < yaml.size() && (yaml[pos] == ' ' || yaml[pos] == '\t'))
            ++pos;

        std::string value;
        if (pos < yaml.size() && yaml[pos] == '"')
        {
            // Quoted string.
            ++pos;
            while (pos < yaml.size() && yaml[pos] != '"')
                value += yaml[pos++];
            return value;
        }
        else if (pos < yaml.size() && yaml[pos] == '[')
        {
            // List -- return the raw bracketed text; ParseYamlList handles it.
            size_t close = yaml.find(']', pos);
            if (close != std::string::npos)
                return yaml.substr(pos, close - pos + 1);
            return yaml.substr(pos);
        }
        else
        {
            // Unquoted scalar.
            while (pos < yaml.size() && yaml[pos] != '\n')
                value += yaml[pos++];
            size_t b = value.find_first_not_of(" \t");
            if (b != std::string::npos)
                value = value.substr(b);
            size_t e = value.find_last_not_of(" \t\r\n");
            if (e != std::string::npos)
                value = value.substr(0, e + 1);
        }

        return value;
    }

    std::vector<std::string> SkillLoader::ParseYamlList(const std::string &value)
    {
        std::vector<std::string> items;
        size_t open = value.find('[');
        size_t close = value.find(']');
        if (open == std::string::npos || close == std::string::npos || close <= open)
            return items;

        std::string inner = value.substr(open + 1, close - open - 1);
        size_t start = 0;
        while (start < inner.size())
        {
            size_t comma = inner.find(',', start);
            std::string token = inner.substr(start, comma - start);
            size_t b = token.find_first_not_of(" \t");
            size_t e = token.find_last_not_of(" \t\r\n");
            if (b != std::string::npos && e != std::string::npos)
            {
                std::string item = token.substr(b, e - b + 1);
                // Strip quotes.
                if (item.size() >= 2 && item.front() == '"' && item.back() == '"')
                    item = item.substr(1, item.size() - 2);
                if (!item.empty())
                    items.push_back(item);
            }
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return items;
    }

}
