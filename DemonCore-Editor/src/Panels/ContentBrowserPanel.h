#pragma once

// The path type every member and accessor below uses. Forward-
// declaring it is not possible, so this header pays the template
// include cost in every translation unit that holds a panel.
#include <filesystem>
// Needed for the std::string members and for the const std::string&
// parameter on CreateDirectory/IsOpenable.
#include <string>

// The panel lives in the same namespace as the rest of the editor.
// is a bare namespace block, not namespace X { namespace Y { --
// it is a top-level Wasteland namespace rather than a nested editor one.
namespace Wasteland {

	// Browses the active project's asset tree. Every path shown is relative
	// to the project's asset root, and the browser refuses to navigate above
	// it, so a project can never reach into another project's files (or the
	// user's drive) by accident.
	class ContentBrowserPanel
	{
	public:
		// Out-of-line in the .cpp: it has to seed m_CurrentDirectory from
		// GetAssetRoot(), and touching the filesystem does not belong in a
		// header.
		ContentBrowserPanel();

		// The draw entry, named to match the convention the other editor
		// panels use (ScriptEditorPanel does the same) rather than overriding
		// a base hook -- this class derives from nothing.
		void OnImGuiRender();

		// Reads the MENU-level flag only.
		// DEFECT: this ignores m_ShowNewFolderInput entirely, which is fine,
		// but unlike ScriptEditorPanel::IsVisible there is no second
		// visibility flag being folded in here -- so m_Visible is the single
		// source of truth for panel visibility in this class.
		bool IsVisible() const { return m_Visible; }
		// One-liner assigned inline. Contrast with ContentBrowserPanel having
		// no destructor declared at all: the class holds only value types, so
		// the implicit one is correct and needs no out-of-line definition.
		void SetVisible(bool visible) { m_Visible = visible; }

		// Returns to the asset root. Called when the project changes, since
		// the directory being viewed belongs to a different tree.
		// Deliberately does NOT clear m_SelectedFile, per its comment above.
		// That is a real hazard worth stating: after a project switch,
		// m_SelectedFile may still hold an absolute path from the PREVIOUS
		// project, so GetSelectedFile() and GetSelectedProjectRelative() can
		// return a path that no longer exists or is no longer inside the
		// active project. The containment check in IsInsideProject would
		// reject it if called, but these accessors do not call it.
		// Not fixed here.
		void ResetToRoot();

		// Absolute path of the file currently selected, or empty.
		// Returns a REFERENCE to the member rather than a copy.
		// DEFECT: the class is not const-correct about this being safe to
		// hold -- the reference stays valid only while this panel lives, and
		// nothing prevents a caller from stashing it and then opening another
		// file, which reassigns m_SelectedFile underneath it. A by-value
		// return would cost one path copy and remove the hazard.
		const std::filesystem::path &GetSelectedFile() const { return m_SelectedFile; }

		// Project-relative form of the selection, suitable for writing into
		// a scene or handing to a tool. Empty when nothing is selected.
		// Returns a std::string rather than a path, and it is NOT inline: the
		// relative-path arithmetic needs the asset root from GetAssetRoot(),
		// which is a .cpp-only concern.
		std::string GetSelectedProjectRelative() const;

		// Creates a directory under the current one. Returns false and
		// leaves the tree untouched if the name is unusable.
		// The bool return is what lets a caller distinguish success from
		// failure -- contrast with ScriptEditorPanel::SaveFile, which returns
		// nothing and silently fails. This class gets it right.
		// No overload that takes a full path: the destination is always the
		// current directory, so a caller cannot direct it elsewhere.
		bool CreateDirectory(const std::string &name);

		// Deletes a project file or directory. Refuses anything that
		// resolves outside the active project.
		// Takes an ABSOLUTE path, unlike CreateDirectory, and has to
		// re-derive its project-relative form internally to run the
		// containment check.
		// DEFECT: the name promises deletion but there is no confirmation
		// flag parameter and the header says nothing about one existing in
		// the UI. As declared, a single call deletes recursively with no way
		// to request a dry run.
		bool DeleteEntry(const std::filesystem::path &absolutePath);

		// Copies an external file into the current directory. Returns the
		// destination, or an empty path on failure.
		// The only member that accepts a path from OUTSIDE the project, which
		// makes it the natural place for a containment check to be skipped --
		// and correctly so, since an external file is by definition outside.
		// It returns the destination rather than a bool, so failure is
		// signalled by an EMPTY path. That is a weaker signal than the bool
		// the other mutators use: a legitimate destination can never be
		// empty, so it works, but the two conventions coexist in one class.
		std::filesystem::path ImportFile(const std::filesystem::path &externalPath);

		// Takes the path the Open button was pressed on, if any, and clears
		// the slot. Empty when nothing was requested.
		//
		// A hand-off rather than a dispatch because this panel does not know
		// which editor owns which extension -- opening from here would make
		// the browser depend on every panel it can reach. EditorLayer drains
		// this after rendering.
		// Named Consume to make the destructive read explicit: calling it
		// twice returns empty the second time.
		// DEFECT: this is a non-const method with side effects that reads like
		// a getter, so it is easy to call speculatively. It is also not
		// thread-safe -- EditorLayer draining it while OnImGuiRender writes
		// m_OpenRequested would race on the std::string.
		std::string ConsumeOpenRequest();

		// True when some editor can open this path. Decides whether the Open
		// button appears at all, so an unsupported type shows no button
		// rather than one that opens the wrong thing.
		// Takes a path rather than consulting m_SelectedFile, so the same
		// predicate can be asked about a non-selected entry (for deciding
		// whether to show a per-row Open button).
		bool IsOpenable(const std::filesystem::path &path) const;

	private:
		// The asset root. Re-read from Project rather than cached, since the
		// active project can change at runtime.
		// By value, not by reference: it constructs a fresh path each call by
		// querying the project singleton, so a reference would have nothing
		// stable to point at.
		// Being private is what stops a caller from caching a stale root.
		std::filesystem::path GetAssetRoot() const;

		// True when path lies inside the active project.
		// The single containment gate the class relies on.
		// DEFECT (important, unverifiable from this header): the header says
		// the browser "refuses to navigate above" the root, but a correct
		// implementation needs not just a string-prefix comparison (which
		// would accept a sibling directory like "MyProject_backup") but a
		// canonicalised path comparison, and it has to resolve symlinks and
		// "..". Whether that is done cannot be confirmed here -- the .cpp
		// would have to be read.
		bool IsInsideProject(const std::filesystem::path &path) const;

	private:
		// Where the listing is currently showing.
		// DEFECT: no in-class initialiser, so it is EMPTY until the
		// constructor runs. An empty current directory is a subtly different
		// state from the asset root, and every method that builds a path from
		// it would produce a relative path. The constructor has to set it
		// for the class to work at all.
		std::filesystem::path m_CurrentDirectory;
		// The current selection; empty means nothing selected.
		// DEFECT: also uninitialised, unlike m_OpenRequested and the three
		// below which all have in-class initialisers. The inconsistency
		// within this member block is itself the signal: someone added the
		// defaults later and did not backfill.
		std::filesystem::path m_SelectedFile;

		// Set by the Open button, drained by EditorLayer. See
		// ConsumeOpenRequest for why this is a hand-off, not a dispatch.
		// The hand-off slot. Empty by default, which is what makes
		// ConsumeOpenRequest()'s empty return unambiguous.
		std::string m_OpenRequested;
		// Panels start visible: this is a docked browser, not something the
		// user opts into.
		bool m_Visible = true;

		// Toggles the inline "new folder" text field. False by default, so the
		// browser shows only the file listing on first render.
		bool m_ShowNewFolderInput = false;
		// Fixed 128-byte buffer, zero-initialised.
		// This is the storage for the ImGui InputText call, which wants a
		// char* -- hence the raw array rather than a std::string.
		// DEFECT: nothing in this header bounds the typed text to 128 bytes
		// other than the ImGui call's own max-length argument, which is not
		// visible here. If that argument is wrong, CreateDirectory on line 35
		// receives a std::string built from an over-long buffer.
		// Also note the class now has a non-trivial member (a std::string and
		// a std::filesystem::path in m_OpenRequested/m_CurrentDirectory), yet
		// declares no copy constructor -- copying a panel would copy the
		// C-string buffer correctly (it is a member array) so this one is
		// actually safe, unlike ScriptEditorPanel's raw-pointer member.
		char m_NewFolderBuffer[128] = {0};
	};

}