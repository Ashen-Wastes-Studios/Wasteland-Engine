// PCH first, by house rule. This panel lives in the editor module but
// shares the engine's precompiled header, so the standard-library and
// logging facilities come from wlpch rather than from local includes.
#include "wlpch.h"
#include "ContentBrowserPanel.h"

// The only project header this translation unit needs: Project owns the
// asset root, the containment test, and name sanitization. Everything
// else is STL or ImGui.
#include "Wasteland/Project/Project.h"

// std::sort for the entry listing below. The sort is not cosmetic --
// an unsorted directory_iterator reshuffles on every refresh.
#include <algorithm>
// strlen, for the "did the user type anything" check on the new-folder
// buffer. C-style on purpose: the buffer is a char[] because that is
// what ImGui::InputText writes into.
#include <cstring>
#include <vector>

// ImGui last, after the project headers: it is the heaviest and least
// used here, and the include order keeps the engine's own headers
// winning any collision.
#include <imgui/imgui.h>

namespace Wasteland
{

    // Default ctor exists only to get a valid starting state, so all it
    // does is the one reset every panel needs on construction.
    ContentBrowserPanel::ContentBrowserPanel()
    {
        // Delegated rather than duplicated: the same three fields have to be
        // cleared when the project changes, and one implementation cannot
        // drift from the other.
        ResetToRoot();
    }

    // Re-read from Project on every call instead of cached in a member.
    // The header says why: the active project can change at runtime, and a
    // cached root would leave the panel browsing the previous project's
    // tree. Returning by value costs a path copy but cannot go stale.
    std::filesystem::path ContentBrowserPanel::GetAssetRoot() const
    {
        // Forwarding accessor. Exists so the rest of the file never touches
        // Project::Get() for the root, keeping the singleton lookup in one
        // place if it ever needs a fallback.
        return Project::Get().GetAssetRoot();
    }

    // Full reset, not just navigation: the selection and the in-progress
    // folder name both belong to the directory being left, and carrying
    // either across would let a stale selection be opened in the new tree.
    void ContentBrowserPanel::ResetToRoot()
    {
        // The asset root, not the project root: the browser is scoped to the
        // assets subtree, and this is the ceiling it navigates back to.
        m_CurrentDirectory = GetAssetRoot();
        // clear() rather than assigning an empty path -- same effect, but it
        // cannot accidentally allocate or leave a capacity held.
        m_SelectedFile.clear();
        // Hide the inline input by default; it is opt-in from the toolbar.
        m_ShowNewFolderInput = false;
        // Only the first byte needs clearing: this is a C string and ImGui
        // reads it as one, so truncating at index 0 empties it. memset would
        // also work; this matches what the two button handlers do.
        m_NewFolderBuffer[0] = '\0';
    }

    // The form written into scene files and handed to tools, so it is
    // PROJECT-relative with forward slashes -- not the absolute path and
    // not asset-relative.
    std::string ContentBrowserPanel::GetSelectedProjectRelative() const
    {
        // Empty selection is not an error here; it is the normal state before
        // the first click, and callers treat "" as "nothing selected".
        if (m_SelectedFile.empty())
            return "";
        // MakeProjectRelative returns "" for a path outside the project, which
        // this function would then hand back unchanged. That is why the empty
        // check above has to come first: without it an out-of-project
        // selection would report as "selected: " with a blank name.
        return Project::Get().MakeProjectRelative(m_SelectedFile);
    }

    // The containment test that makes the panel safe. Everything that
    // touches the filesystem -- delete, import destination, navigation
    // target -- is gated on this, so it is written to fail closed.
    bool ContentBrowserPanel::IsInsideProject(const std::filesystem::path &path) const
    {
        // An empty path is not "inside the project". Failing closed matters
        // here: the alternative would let an unset path pass a later check.
        if (path.empty())
            return false;

        // Non-throwing error_code overloads throughout: a filesystem call that
        // throws on a permission error would take the editor down mid-frame.
        std::error_code ec;
        // With no project open there is no frame of reference, so containment
        // cannot be established. Returning false makes ResetToRoot a no-op
        // rather than letting navigation proceed against an undefined root.
        if (Project::Get().GetRoot().empty())
            return false;

        // Resolved against the process working directory, so a relative path
        // from a caller is interpreted the same way the filesystem would. The
        // error_code overload makes an unresolvable path detectable rather
        // than throwing.
        const std::filesystem::path abs = std::filesystem::absolute(path, ec);
        // A path that cannot be made absolute is treated as outside the
        // project -- the same fail-closed default as the two checks above.
        if (ec)
            return false;

        // MakeProjectRelative returns empty for anything that climbs out of
        // the project, which is exactly the containment test we want.
        // Reuses the containment rule rather than reimplementing a prefix
        // comparison. That is also why no canonicalization is needed here:
        // MakeProjectRelative normalizes both sides before comparing.
        return !Project::Get().MakeProjectRelative(abs).empty();
    }

    // Folder creation, reached only from the inline New Folder input.
    bool ContentBrowserPanel::CreateDirectory(const std::string &name)
    {
        // SanitizeName is the security-relevant step: it strips separators and
        // the Windows-illegal character set, so a name of "../../etc" cannot
        // escape m_CurrentDirectory. It is unconditional rather than
        // platform-conditional so a folder made on Linux still opens on
        // Windows.
        const std::string safeName = Project::SanitizeName(name);
        // SanitizeName returns the literal "Untitled" when nothing usable
        // survives, so that sentinel -- not emptiness -- is the real reject
        // condition. A name of literally "Untitled" is refused too, which
        // costs the user that one name and keeps the sentinel unambiguous.
        if (safeName.empty() || safeName == "Untitled")
        {
            // Logs the ORIGINAL name, not the sanitized one: the point of the
            // message is to show the user what they typed, since the sanitized
            // form is not what they will see on disk.
            WL_CORE_ERROR("ContentBrowser: '{0}' is not a usable folder name", name);
            return false;
        }

        std::error_code ec;
        // Joining a sanitized single component onto the current directory.
        // Because safeName cannot contain a separator, this stays one level
        // down -- the traversal guarantee rests on SanitizeName, not on this
        // line.
        const std::filesystem::path dir = m_CurrentDirectory / safeName;
        // Refuses rather than merging or appending a suffix. Renaming or
        // numbering is a deliberate action, not a side effect of clicking
        // Create twice.
        if (std::filesystem::exists(dir, ec))
        {
            WL_CORE_ERROR("ContentBrowser: '{0}' already exists", dir.string());
            return false;
        }

        // create_directories, not create_directory: the parent chain is
        // created if missing, which is what makes this robust to the current
        // directory having been removed underneath the panel.
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            WL_CORE_ERROR("ContentBrowser: could not create '{0}': {1}", dir.string(), ec.message());
            return false;
        }

        // Success is logged at INFO. Every failure above logs at ERROR with
        // the offending path, so a user who wonders why their folder did not
        // appear has a trail to follow.
        WL_CORE_INFO("ContentBrowser: created folder {0}", dir.string());
        return true;
    }

    // Recursive delete, so this is the most destructive call in the panel.
    // IsInsideProject below is the only thing standing between a bad path
    // and an entire directory tree.
    bool ContentBrowserPanel::DeleteEntry(const std::filesystem::path &absolutePath)
    {
        // Containment check BEFORE any filesystem mutation, not after -- once
        // remove_all returns there is nothing to undo.
        if (!IsInsideProject(absolutePath))
        {
            WL_CORE_ERROR("ContentBrowser: refusing to delete '{0}' (outside the project)", absolutePath.string());
            return false;
        }

        std::error_code ec;
        // remove_all rather than remove: deleting a folder in this panel must
        // take its contents with it. The return value is the number of
        // entries removed, and it is 0 for a path that did not exist -- which
        // is why the `!removed` half of the check below is needed to
        // distinguish that from success.
        const bool removed = std::filesystem::remove_all(absolutePath, ec);
        // Both halves matter: `ec` catches a genuine failure part-way through,
        // `!removed` catches "nothing was there". Either alone would report a
        // silent no-op delete as a success.
        if (ec || !removed)
        {
            WL_CORE_ERROR("ContentBrowser: could not delete '{0}': {1}", absolutePath.string(), ec.message());
            return false;
        }

        // Destructive operations are logged at INFO too, so there is a
        // record of what was removed after the fact.
        WL_CORE_INFO("ContentBrowser: deleted {0}", absolutePath.string());

        // Post-delete repair. The panel caches a directory it is displaying,
        // and deleting that directory (or an ancestor of it) leaves the
        // cache pointing at something that no longer exists.
        // The directory being viewed may be the one just deleted.
        std::error_code check;
        // Separate error_code: `ec` above still holds the delete result and
        // must not be reused, or a successful delete followed by a failed
        // is_directory probe would report as a delete failure.
        //
        // The ResetToRoot() on the next line fires only when the directory is
        // GONE. A directory that exists but cannot be inspected also reports
        // false here, so a permissions problem costs the user their current
        // directory and their selection.
        if (!std::filesystem::is_directory(m_CurrentDirectory, check))
            ResetToRoot();
        // Compare-then-clear: the selection is a full path, so this only
        // matches when the deleted entry was exactly the selected file.
        if (m_SelectedFile == absolutePath)
            m_SelectedFile.clear();

        return true;
    }

    // Copies an external file in. Note the asymmetry with DeleteEntry:
    // the SOURCE is unconstrained (importing from anywhere on disk is
    // the point) and only the destination is derived from the current
    // directory, which the panel controls.
    std::filesystem::path ContentBrowserPanel::ImportFile(const std::filesystem::path &externalPath)
    {
        std::error_code ec;
        // Rejects directories and dangling paths up front, so the copy below
        // cannot fail halfway through on a bad source.
        if (!std::filesystem::is_regular_file(externalPath, ec))
        {
            WL_CORE_ERROR("ContentBrowser: '{0}' is not a file", externalPath.string());
            // Empty path is the failure signal, matching the documented contract
            // in the header. Callers test the result rather than an error code.
            return {};
        }

        // The destination keeps only the FILENAME of the source: the source
        // directory is deliberately discarded, because the panel imports
        // "this file, into where I am looking", not "this file and its
        // siblings".
        const std::filesystem::path dst = m_CurrentDirectory / externalPath.filename();
        // No overwrite and no rename-on-collision, so an import can never
        // silently destroy an existing asset. The user must rename or delete
        // the destination first.
        if (std::filesystem::exists(dst, ec))
        {
            WL_CORE_ERROR("ContentBrowser: '{0}' already exists", dst.string());
            return {};
        }

        // copy_file with the default options copies content only, leaving
        // the source untouched -- an import must not consume the original.
        //
        // The `if (ec)` on the next line uses the error_code overload, so a
        // locked or read-only destination reports here instead of throwing
        // std::filesystem_error.
        std::filesystem::copy_file(externalPath, dst, ec);
        if (ec)
        {
            WL_CORE_ERROR("ContentBrowser: could not import '{0}': {1}", externalPath.string(), ec.message());
            return {};
        }

        WL_CORE_INFO("ContentBrowser: imported {0} -> {1}", externalPath.string(), dst.string());
        return dst;
    }

    // One ImGui window per frame when visible. Everything below is inside
    // a single Begin/End pair, which is why there are no early returns
    // past the visibility check: an early return after Begin would leave
    // the window stack unbalanced.
    void ContentBrowserPanel::OnImGuiRender()
    {
        // Visibility gate before Begin, so a hidden panel costs one branch and
        // no ImGui state. This is the only early return in the function.
        if (!m_Visible)
            return;

        // The bool overload lets ImGui own the close button, so the X and
        // SetVisible stay in sync through m_Visible itself.
        ImGui::Begin("Content Browser", &m_Visible);

        // Project::Get() is a singleton accessor, so this is a reference to
        // the one instance rather than a copy.
        Project &project = Project::Get();
        // Read once per frame instead of calling GetAssetRoot() at each of
        // the several use sites below; the value cannot change mid-frame.
        const std::filesystem::path assetRoot = GetAssetRoot();

        // The active project can change at runtime; a directory belonging to
        // the previous one means nothing in the new tree.
        // DEFECT (root vs asset root): this checks against the PROJECT root
        // via MakeProjectRelative, not against the asset root the panel is
        // scoped to. The "<-" button below therefore walks m_CurrentDirectory
        // up past the asset root to the project root and one level further,
        // because parent_path() of the project root is still inside the
        // project. The header promises the browser "refuses to navigate
        // above" the asset root; nothing here enforces that.
        if (m_CurrentDirectory.empty() || !IsInsideProject(m_CurrentDirectory))
            ResetToRoot();

        // --- Project header -------------------------------------------
        // The panel renders in both states: with no project open, Project
        // still has an asset root (a detached root from Bootstrap), so the
        // browser stays usable rather than showing an empty frame.
        if (project.IsOpen())
        {
            ImGui::Text("Project: %s", project.GetName().c_str());
        }
        else
        {
            // Amber rather than plain text so the degraded state is visible
            // without reading it.
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "No project (asset root only)");
        }

        // Single-line path label. Called a breadcrumb in intent, but it is
        // not clickable and has no segments -- there is no way to jump to an
        // ancestor from here, only via "<-" one level at a time.
        // Project-relative path of the current directory, as a breadcrumb.
        {
            // Cached into a local because it is used once but costs a path
            // conversion.
            const std::string rel = project.MakeProjectRelative(m_CurrentDirectory);
            // The fallback fires only when no project is open: with a project,
            // the asset root still resolves to a non-empty "assets/..." string,
            // so the assetRoot branch is effectively the no-project path.
            ImGui::TextDisabled("%s", rel.empty() ? assetRoot.string().c_str() : rel.c_str());
        }

        ImGui::Separator();

        // --- Navigation -------------------------------------------------
        // Navigation row. The comparison is against the asset root, so at the
        // root itself there is nowhere to go up to.
        if (m_CurrentDirectory != assetRoot)
        {
            // The "<-" glyph rather than a path: at deep nesting the full parent
            // path would not fit in a button.
            //
            // DEFECT (unbounded ascent): the assignment on the next line walks up
            // one level per click with no check that the result is still under
            // the asset root -- see the note at the ResetToRoot guard above.
            // Clamping to assetRoot here would make the panel match its own
            // documented contract.
            if (ImGui::Button("<-"))
            {
                m_CurrentDirectory = m_CurrentDirectory.parent_path();
                m_SelectedFile.clear();
            }
        }
        else
        {
            // Keep the row height stable so the listing does not jump.
            // Layout placeholder rather than nothing, so the Refresh and
            // New Folder buttons below keep the same row and column whether or
            // not there is an up-button. Without it the toolbar shifts right
            // every time the user reaches the root.
            ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
        }

        ImGui::SameLine();
        // Refresh clears the selection but does NOT re-read the directory:
        // the listing is rebuilt from scratch on every frame anyway, so
        // there is no cache to invalidate.
        if (ImGui::Button("Refresh"))
        {
            m_SelectedFile.clear();
        }

        ImGui::SameLine();
        // Toggle rather than one-way open, so the input can be dismissed
        // without creating anything.
        if (ImGui::Button("New Folder"))
        {
            m_ShowNewFolderInput = !m_ShowNewFolderInput;
            // Cleared on every toggle, so reopening the input never shows the
            // previously typed name.
            m_NewFolderBuffer[0] = '\0';
        }

        // Inline row, only present while m_ShowNewFolderInput is set.
        if (m_ShowNewFolderInput)
        {
            // "##NewFolder" is an ImGui ID with no visible label; the visible
            // label is the Create button beside it. The size argument is the
            // buffer capacity, which is what stops ImGui writing past the end.
            ImGui::InputText("##NewFolder", m_NewFolderBuffer, IM_ARRAYSIZE(m_NewFolderBuffer));
            ImGui::SameLine();
            // Fixed width so Create and Cancel line up. The "##" suffix hides
            // the label text from the button while keeping the ID stable.
            if (ImGui::Button("Create##CreateFolder", ImVec2(70, 0)))
            {
                // strlen guard: an empty buffer would otherwise be passed to
                // CreateDirectory, where SanitizeName would turn it into "Untitled"
                // and log an error for what was really just an empty field.
                if (strlen(m_NewFolderBuffer) > 0)
                    // Return value ignored: a failed create has already been logged at
                    // ERROR inside CreateDirectory, and this handler has nothing to add
                    // to the UI -- the folder simply will not appear in the listing.
                    CreateDirectory(m_NewFolderBuffer);
                m_ShowNewFolderInput = false;
                m_NewFolderBuffer[0] = '\0';
            }
            ImGui::SameLine();
            // Cancel duplicates the two reset lines of Create rather than
            // sharing them; at two statements the indirection would cost more
            // than it saves.
            if (ImGui::Button("Cancel##CancelFolder", ImVec2(70, 0)))
            {
                m_ShowNewFolderInput = false;
                m_NewFolderBuffer[0] = '\0';
            }
        }

        ImGui::Separator();

        // The listing is rebuilt from scratch EVERY frame rather than cached
        // and invalidated. Simple and always correct, at the cost of a
        // directory enumeration per frame while the panel is visible --
        // fine for an asset folder, expensive for a very large one.
        // --- Entries ---------------------------------------------------
        // Sorted, so the listing does not reshuffle on every refresh -- the
        // unsorted directory_iterator made this panel hard to use.
        std::vector<std::filesystem::path> entries;
        std::error_code iterEc;
        // Error_code constructor: a missing or unreadable directory yields
        // an end iterator instead of throwing.
        for (auto it = std::filesystem::directory_iterator(m_CurrentDirectory, iterEc);
             // increment(iterEc) rather than operator++, so a mid-enumeration
             // failure can be detected below instead of throwing.
             it != std::filesystem::directory_iterator(); it.increment(iterEc))
        {
            // Fail-closed on a partial read: better to show a few entries and
            // an error than to silently show a truncated tree as if complete.
            if (iterEc)
                break;
            entries.push_back(it->path());
        }

        // Sort for a stable listing. Directories first, then name, so the
        // order is predictable frame to frame.
        std::sort(entries.begin(), entries.end(),
                  [](const std::filesystem::path &a, const std::filesystem::path &b) {
                      std::error_code ea, eb;
                      // DEFECT (stat per comparison): this comparator re-runs is_directory
                      // for both operands on every call, so the sort performs O(n log n)
                      // stat syscalls instead of the O(n) a precomputed flag would cost.
                      // Harmless at asset-folder sizes, and invisible until it is not.
                      const bool aDir = std::filesystem::is_directory(a, ea);
                      const bool bDir = std::filesystem::is_directory(b, eb);
                      if (aDir != bDir)
                          // Strict weak ordering: directories sort before files, and no two
                          // distinct entries compare equal on both sides of this rule, which
                          // std::sort requires.
                          return aDir; // Directories first.
                      return a.filename().string() < b.filename().string();
                  });

        // Error and empty are different states and get different messages:
        // one means "try again", the other means "this folder really is
        // empty". Collapsing them would make a permissions problem look
        // like an empty folder.
        if (iterEc)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Could not read directory");
        }
        else if (entries.empty())
        {
            ImGui::TextDisabled("(empty)");
        }

        // Iterate the SORTED vector rather than the directory iterator, so
        // what is drawn matches the order decided above.
        for (const std::filesystem::path &path : entries)
        {
            const std::string filenameString = path.filename().string();
            std::error_code entryEc;
            // Directory-ness is re-queried here even though the sort already
            // determined it -- it was not cached, only used as a comparison key.
            // Same underlying cause as the comparator above.
            const bool isDirectory = std::filesystem::is_directory(path, entryEc);
            const std::string label = isDirectory ? "[D] " + filenameString : filenameString;

            // PushID with the filename, not the index: ImGui keys widgets by ID
            // string, so a stable per-file ID keeps selection state attached to
            // the right row across a refresh.
            ImGui::PushID(filenameString.c_str());

            // Single click selects a file; double click enters a directory.
            // Single click selects. Directories are highlighted too, so the
            // row gives feedback before the double-click that acts on it.
            if (ImGui::Selectable(label.c_str(), m_SelectedFile == path))
            {
                // Clicking a DIRECTORY clears the selection rather than selecting
                // it: a directory is something to enter, not to open, and leaving a
                // stale file selected while browsing folders is a known confusion.
                m_SelectedFile = isDirectory ? std::filesystem::path() : path;
            }

            // IsItemHovered gates the double-click so it cannot fire for a row
            // the mouse has already left. IsMouseDoubleClicked still reports the
            // click for whatever item is under the cursor, which is why the
            // hover test has to come first.
            if (isDirectory && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_CurrentDirectory = path;
                m_SelectedFile.clear();
            }
            else if (!isDirectory && path.extension() == ".wlavb" &&
                     ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                // AVB graphs open in their own graph panel; the browser only
                // queues the path and lets EditorLayer dispatch by extension.
                m_OpenRequested = path.string();
            }

            // Drag source. Wrapped in its own block because BeginDragDropSource
            // must be paired with EndDragDropSource() before anything else is
            // emitted for this item.
            if (ImGui::BeginDragDropSource())
            {
                // Payload is a UTF-8 project-relative path. The previous code
                // shipped a wchar_t*, which is 4 bytes wide on Linux and would
                // have been truncated there.
                // Project-relative so the receiving panel can resolve it through
                // Project regardless of where the project lives on disk.
                const std::string rel = project.MakeProjectRelative(path);
                // DEFECT (empty drag source): when `rel` is empty the payload is
                // skipped but EndDragDropSource() still runs, so the row is
                // presented as draggable and delivers nothing -- the drag looks
                // like it works and the drop is silently ignored. Returning early
                // (or setting an empty payload) would make the row non-draggable
                // instead.
                if (!rel.empty())
                    // rel.size() + 1 because the size is the buffer length INCLUDING the
                    // terminator; passing rel.size() would truncate the last character
                    // on the receiving side.
                    ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", rel.c_str(), rel.size() + 1);
                ImGui::EndDragDropSource();
            }

            ImGui::PopID();
        }

        // Footer: only built when something is selected, so an unselected
        // panel costs no separator.
        // --- Selection footer ------------------------------------------
        if (!m_SelectedFile.empty())
        {
            ImGui::Separator();
            // Project-relative form, consistent with what the drag payload
            // carries.
            ImGui::Text("Selected: %s", GetSelectedProjectRelative().c_str());

            // An explicit Open, rather than acting on double-click.
            //
            // Double-click on a row is also "enter this directory", and the
            // two meanings are indistinguishable to a user who double-clicks
            // slightly slowly. Making opening a button costs one click and
            // removes the ambiguity entirely -- and it is the only place that
            // has to know which editor owns which extension.
            //
            // Only shown for types something can open. A .fbx or .png
            // selected here gets no button rather than a button that opens
            // the wrong thing.
            // Button visibility is the type check. A .fbx selected here shows
            // no Open button rather than one that would open the wrong editor --
            // the panel deliberately does not know which editor owns which
            // extension.
            if (IsOpenable(m_SelectedFile))
            {
                ImGui::SameLine();
                if (ImGui::Button("Open"))
                {
                    // Absolute path. EditorLayer resolves it through
                    // Project where a panel needs project-relative, so a
                    // project opened from any directory works.
                    // ABSOLUTE path, unlike the drag payload just above. Documented in
                    // the header: EditorLayer resolves it through Project, so a project
                    // opened from any directory works.
                    m_OpenRequested = m_SelectedFile.string();
                }
            }
        }

        // Closes the Begin from the top of the function. Must be reached on
        // every path that passed Begin -- which is why the function has no
        // early return below line 146.
        ImGui::End();
    }

    // The extension allowlist that decides Open button visibility.
    bool ContentBrowserPanel::IsOpenable(const std::filesystem::path &path) const
    {
        std::error_code ec;
        // A directory is never openable, and this is checked before the
        // extension so an extensionless directory does not fall through.
        if (std::filesystem::is_directory(path, ec))
            return false;

        // extension() includes the leading dot, which is why the comparisons
        // below carry one.
        const std::string ext = path.extension().string();
        // DEFECT (case-sensitive): these comparisons are case-sensitive, but
        // the filesystems this panel browses are usually case-INSENSITIVE. A
        // file named Script.PY exists and is openable, yet matches nothing
        // here, so it gets no Open button and the type check silently
        // disagrees with what the platform would actually load.
        return ext == ".py"        // Script Editor
            || ext == ".wlbtree"   // NPC Behavior panel
            // Lowercasing the extension before comparison would fix the above
            // without changing any other behaviour.
            || ext == ".wlblocks" // Visual Blocks flow graph
            || ext == ".wlavb"    // AVB audio graph
            || ext == ".wlterrain"; // Terrain source asset
    }

    // Hand-off, not dispatch: swap takes the string and leaves the member
    // empty in one step, so the request cannot be handled twice and no
    // separate "clear" call can be forgotten.
    std::string ContentBrowserPanel::ConsumeOpenRequest()
    {
        std::string out;
        // swap rather than assign-and-clear: O(1), and out is left holding
        // exactly what the member held.
        out.swap(m_OpenRequested);
        return out;
    }

}