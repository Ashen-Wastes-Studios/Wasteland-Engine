// SubagentSystem.h -- automatic parallel subagents for the AI chat panel.
//
// Mirrors Hermes Agent's `delegate_task`: the main agent emits a <delegate>
// block, the system decomposes it into independent tasks, runs each on its
// own thread with its own provider/transcript slice, and merges the results
// back into the main agent's context.
//
// Design constraints carried over from the plan (docs/BUILT_IN_AI_CHAT_PLAN.md
// section 6.5):
//   - Subagents do NOT share the main agent's message list. Each gets a
//     scoped copy of the relevant context only.
//   - Subagents report back as structured results, not raw chat messages.
//   - A subagent cannot spawn further subagents (no infinite fan-out).
//   - Concurrency is capped by ApiKeyStore::MaxSubagents.
#pragma once

#include <string>
#include <vector>

namespace Wasteland
{
    // One unit of delegated work. Parsed from a <task .../> element inside a
    // <delegate> block.
    struct SubagentTask
    {
        std::string Id;                          // task id from the tag
        std::string Goal;                        // what the subagent must accomplish
        std::string Context;                     // relevant project/engine snippets
        std::vector<std::string> AllowedTools;   // tool whitelist (empty = all)
        int TimeoutSeconds = 120;                // wall-clock cap (informational)
    };

    // The outcome of one subagent run.
    struct SubagentResult
    {
        std::string TaskId;
        std::string Summary;      // concise result handed to the main agent
        std::string FullOutput;   // complete transcript, shown in the UI on expand
        bool Success = false;
        std::string Error;
    };

    // Everything the parallel runner needs to build a subagent's world.
    // Passed by value into each worker thread; nothing here is shared mutable
    // state.
    struct SubagentRunContext
    {
        std::string ProviderName;
        std::string Model;
        float Temperature = 0.7f;
        std::string SystemPrompt;
        std::string ToolDefinitionsJson;
        int MaxIterations = 6;
        int MaxConcurrent = 4;
    };

    class SubagentSystem
    {
    public:
        // Parses a <delegate> ... </delegate> block out of model output.
        // Returns false when no well-formed block is present. Malformed or
        // empty task lists also return false, so the caller treats "nothing
        // parsed" as "nothing to run".
        static bool ParseDelegateTag(const std::string &text, std::vector<SubagentTask> &outTasks);

        // Runs every task in parallel on its own std::thread, capped at
        // ctx.MaxConcurrent simultaneous workers. Blocks until all tasks
        // finish. Results are returned in the same order as the input tasks,
        // so result[i] corresponds to tasks[i].
        static std::vector<SubagentResult> RunParallel(const std::vector<SubagentTask> &tasks,
                                                       const SubagentRunContext &ctx);

        // Merges results into one block suitable for appending to the main
        // agent's context. Failed tasks are reported as such rather than
        // dropped, so the main agent knows a subtask did not complete.
        static std::string Aggregate(const std::vector<SubagentResult> &results);

        // Convenience: parse + run + aggregate in one call. Returns an empty
        // string when no delegate block was found.
        static std::string ExecuteDelegation(const std::string &text, const SubagentRunContext &ctx);

        // The tool-call parser. Duplicated from AIChatPanel::ExtractToolCall
        // rather than exposed from the panel: the panel's copy is private and
        // static, and pulling it out would couple this file to the panel's
        // header for a 40-line scanner. The two must stay byte-identical in
        // behaviour.
        static std::string ExtractToolCall(const std::string &text, std::string &outToolName, std::string &outArgs);
    };
}
