// SubagentSystem.cpp -- parallel subagent execution for the AI chat panel.
#include "wlpch.h"
#include "SubagentSystem.h"
#include "IAIProvider.h"
#include "ProviderFactory.h"
#include "../Panels/AIChatPanel.h"
#include "../Panels/ToolServer.h"

#include <algorithm>
#include <atomic>
#include <thread>

namespace Wasteland
{
    // -------------------------------------------------------------------------
    // Tool-call parsing. Mirrors AIChatPanel::ExtractToolCall exactly.
    // -------------------------------------------------------------------------
    std::string SubagentSystem::ExtractToolCall(const std::string &text, std::string &outToolName, std::string &outArgs)
    {
        size_t start = text.find("<tool_call");
        if (start == std::string::npos)
            return "";

        size_t nameStart = text.find("name=\"", start);
        if (nameStart == std::string::npos)
            return "";
        nameStart += 6;

        size_t nameEnd = text.find("\"", nameStart);
        if (nameEnd == std::string::npos)
            return "";
        outToolName = text.substr(nameStart, nameEnd - nameStart);

        size_t tagEnd = text.find(">", nameEnd);
        if (tagEnd == std::string::npos)
            return "";

        size_t argsStart = text.find("<args>", tagEnd);
        if (argsStart != std::string::npos)
        {
            argsStart += 6;
            size_t argsEnd = text.find("</args>", argsStart);
            if (argsEnd != std::string::npos)
                outArgs = text.substr(argsStart, argsEnd - argsStart);
        }

        size_t callEnd = text.find("</tool_call>", tagEnd);
        if (callEnd == std::string::npos)
            return "";
        return text.substr(start, callEnd + 12 - start);
    }

    // -------------------------------------------------------------------------
    // <delegate> tag parsing.
    //
    // Expected shape (emitted by the model, described in the system prompt):
    //   <delegate>
    //     <task id="1" goal="..." tools="search_files,read_file" />
    //     <task id="2" goal="..." tools="search_files,read_file" />
    //   </delegate>
    //
    // Parsing is deliberately forgiving: a missing attribute is an empty
    // string rather than a parse failure, because a model that emits a
    // half-formed goal is still better served by running the task than by
    // discarding it.
    // -------------------------------------------------------------------------
    namespace
    {
        // Reads `attr="value"` out of a tag body. Returns an empty string when
        // the attribute is absent or unterminated.
        std::string ExtractAttribute(const std::string &tag, const std::string &attr)
        {
            size_t pos = tag.find(attr + "=\"");
            if (pos == std::string::npos)
                return "";
            pos += attr.size() + 2; // skip attr=" (the '=' and the quote)
            size_t end = tag.find("\"", pos);
            if (end == std::string::npos)
                return "";
            return tag.substr(pos, end - pos);
        }

        // Splits a comma-separated tool list. Empty entries are dropped so a
        // trailing comma does not produce a phantom tool name.
        std::vector<std::string> SplitToolList(const std::string &tools)
        {
            std::vector<std::string> out;
            size_t start = 0;
            while (start <= tools.size())
            {
                size_t comma = tools.find(',', start);
                std::string piece = (comma == std::string::npos)
                                        ? tools.substr(start)
                                        : tools.substr(start, comma - start);
                // Trim whitespace so "read_file, search_files" parses the same
                // as "read_file,search_files".
                size_t b = piece.find_first_not_of(" \t");
                size_t e = piece.find_last_not_of(" \t");
                if (b != std::string::npos)
                    out.push_back(piece.substr(b, e - b + 1));
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
            return out;
        }

        bool IsToolAllowed(const std::vector<std::string> &allowed, const std::string &tool)
        {
            if (allowed.empty())
                return true;
            return std::find(allowed.begin(), allowed.end(), tool) != allowed.end();
        }
    }

    bool SubagentSystem::ParseDelegateTag(const std::string &text, std::vector<SubagentTask> &outTasks)
    {
        outTasks.clear();

        size_t blockStart = text.find("<delegate>");
        if (blockStart == std::string::npos)
            return false;
        size_t blockEnd = text.find("</delegate>", blockStart);
        if (blockEnd == std::string::npos)
            return false;

        // The inner text, so the task scan cannot wander past the block.
        std::string block = text.substr(blockStart + 10, blockEnd - (blockStart + 10));

        // Every <task ... /> element in the block.
        size_t pos = 0;
        while (true)
        {
            size_t taskStart = block.find("<task", pos);
            if (taskStart == std::string::npos)
                break;
            // The element ends at its own '/>' -- a '>' alone would also end a
            // non-self-closing tag, which this format does not use.
            size_t taskEnd = block.find("/>", taskStart);
            if (taskEnd == std::string::npos)
                break;

            std::string tag = block.substr(taskStart, taskEnd + 2 - taskStart);

            SubagentTask task;
            task.Id = ExtractAttribute(tag, "id");
            task.Goal = ExtractAttribute(tag, "goal");
            task.Context = ExtractAttribute(tag, "context");
            std::string tools = ExtractAttribute(tag, "tools");
            if (!tools.empty())
                task.AllowedTools = SplitToolList(tools);

            // A task with no goal is useless; skip it rather than running an
            // empty objective.
            if (!task.Goal.empty())
            {
                // Fall back to a positional id when the model omitted one, so
                // the result mapping stays unambiguous.
                if (task.Id.empty())
                    task.Id = std::to_string(outTasks.size() + 1);
                outTasks.push_back(task);
            }

            pos = taskEnd + 2;
        }

        return !outTasks.empty();
    }

    // -------------------------------------------------------------------------
    // One subagent run.
    //
    // Self-contained: builds its own provider, its own transcript, and its own
    // tool loop. It touches no shared state except the ToolServer singleton
    // (which the plan designates as the shared, thread-safe tool surface).
    // -------------------------------------------------------------------------
    namespace
    {
        SubagentResult RunOneTask(const SubagentTask &task, const SubagentRunContext &ctx)
        {
            SubagentResult result;
            result.TaskId = task.Id;

            IAIProvider *provider = ProviderFactory_Create(ctx.ProviderName);
            if (!provider)
            {
                result.Error = "Provider unavailable: " + ctx.ProviderName;
                return result;
            }

            // The subagent's system prompt: the main prompt plus a scoped
            // instruction that makes it report rather than chat.
            std::string sys = ctx.SystemPrompt;
            sys += "\n\n## Subagent Mode\n";
            sys += "You are a subagent spawned by the main agent to complete ONE specific goal. ";
            sys += "Work autonomously: use the available tools, then report a concise summary of what you found or did. ";
            sys += "Do not ask the user questions. Do not spawn further subagents. ";
            sys += "Your final message is returned to the main agent as your result.\n";

            // History starts with the goal, and any context the main agent
            // attached. One user turn, so the subagent's transcript stays
            // small regardless of how many tool rounds it runs.
            std::string userText = "Goal: " + task.Goal;
            if (!task.Context.empty())
                userText += "\n\nContext:\n" + task.Context;

            std::vector<ChatMessage> history;
            history.push_back({"user", userText});

            std::string fullOutput;
            std::string lastAssistant;

            for (int i = 0; i < ctx.MaxIterations; ++i)
            {
                ProviderRequest request;
                request.Model = ctx.Model;
                request.Temperature = ctx.Temperature;
                request.SystemPrompt = sys;
                request.History = history;
                request.MaxHistory = 10;
                request.Stream = true;
                request.ToolDefinitionsJson = ctx.ToolDefinitionsJson;

                std::string accumulated;
                auto onChunk = [&accumulated](const std::string &delta) -> bool
                {
                    accumulated += delta;
                    return true;
                };

                ProviderResult pr = provider->Chat(request, onChunk);
                if (!pr.Succeeded)
                {
                    result.Error = pr.Error.empty() ? "Provider call failed." : pr.Error;
                    break;
                }

                lastAssistant = accumulated;
                fullOutput += accumulated;

                std::string toolName, args;
                if (SubagentSystem::ExtractToolCall(accumulated, toolName, args).empty())
                {
                    // No tool call means the subagent considers the goal done.
                    break;
                }

                // Whitelist enforcement. A disallowed tool is reported back as
                // the tool result, so the subagent can correct course instead
                // of silently losing the call.
                std::string toolResult;
                if (!IsToolAllowed(task.AllowedTools, toolName))
                {
                    toolResult = "Error: tool '" + toolName + "' is not permitted for this task. Allowed tools: ";
                    if (task.AllowedTools.empty())
                        toolResult += "(all)";
                    else
                    {
                        for (size_t j = 0; j < task.AllowedTools.size(); ++j)
                        {
                            if (j) toolResult += ", ";
                            toolResult += task.AllowedTools[j];
                        }
                    }
                }
                else
                {
                    toolResult = ToolServer::Instance().ExecuteTool(toolName, args);
                }

                // Both turns are appended so the next request has the
                // assistant's tool call followed by its result -- the shape
                // the tool-calling convention expects.
                history.push_back({"assistant", accumulated});
                history.push_back({"tool", toolResult, toolName});
            }

            if (result.Error.empty())
            {
                result.Success = true;
                // Prefer the last assistant text as the summary; fall back to
                // the accumulated output if the final turn was a tool call.
                result.Summary = lastAssistant.empty() ? fullOutput : lastAssistant;
                result.FullOutput = fullOutput;
            }

            delete provider;
            return result;
        }
    }

    std::vector<SubagentResult> SubagentSystem::RunParallel(const std::vector<SubagentTask> &tasks,
                                                            const SubagentRunContext &ctx)
    {
        std::vector<SubagentResult> results(tasks.size());
        if (tasks.empty())
            return results;

        // Cap the worker count at both the configured maximum and the task
        // count -- spawning more threads than tasks is pure overhead.
        size_t workerCount = std::min<size_t>(std::max(1, ctx.MaxConcurrent), tasks.size());

        std::atomic<size_t> nextIndex{0};
        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (size_t w = 0; w < workerCount; ++w)
        {
            workers.emplace_back([&]()
                                 {
                                     size_t i;
                                     while ((i = nextIndex.fetch_add(1)) < tasks.size())
                                     {
                                         results[i] = RunOneTask(tasks[i], ctx);
                                     }
                                 });
        }

        for (auto &t : workers)
            t.join();

        return results;
    }

    std::string SubagentSystem::Aggregate(const std::vector<SubagentResult> &results)
    {
        std::string out = "## Subagent Results\n\n";
        for (const auto &r : results)
        {
            out += "### Task " + r.TaskId + "\n";
            if (!r.Success)
            {
                out += "Failed: " + (r.Error.empty() ? "unknown error" : r.Error) + "\n\n";
                continue;
            }
            out += r.Summary.empty() ? "(no result)" : r.Summary;
            out += "\n\n";
        }
        return out;
    }

    std::string SubagentSystem::ExecuteDelegation(const std::string &text, const SubagentRunContext &ctx)
    {
        std::vector<SubagentTask> tasks;
        if (!ParseDelegateTag(text, tasks))
            return "";
        auto results = RunParallel(tasks, ctx);
        return Aggregate(results);
    }

}
