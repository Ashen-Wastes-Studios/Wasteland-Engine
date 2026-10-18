// Header guard. The smallest file in the batch: a class with two static
        // functions and no state.
#pragma once

// Both functions return std::string, so this is a real dependency rather
// // than a forward declaration -- std::string cannot be forward declared.
#include <string>

// Braced namespace, consistent with the rest of the Utils tree.
namespace Wasteland {

	// A namespace-shaped class: every member is static and there is no data,
	        // so it is never instantiated. Written as a class rather than a
	        // namespace because these are platform-dispatched implementations
	        // behind a common name.
	//
	        // DEFECT (no virtual destructor, documented not fixed): a class with a
	        // // non-trivial destructor can be destroyed through a base pointer. This
	        // // one has no virtual methods and no destructor at all, so the implicit
	        // // one is non-virtual. It is never used polymorphically -- nothing
	        // // derives from it -- so this is a latent hazard rather than a live bug.
	class FileDialogs
	{
	public:
		// THE CONTRACT, in one line. An empty string means "the user cancelled".
		//
		        // DEFECT (ambiguous error signalling, documented not fixed): an empty
		        // // string is ALSO what a dialog returns when the user picks a file with
		        // // no name, and it is a legitimate value if the caller ever wants a
		        // // root-relative path. There is no separate success flag and no
		        // // exception, so a caller cannot distinguish cancel from a genuine
		        // // empty result, and a file-open failure on a path that does not exist
		        // // is reported the same way. An optional or a bool out-param would
		        // // // make this unambiguous.
		        // //
		        // // The consequence is a real class of bug: a caller that treats any
		        // // non-empty string as a valid path proceeds, and a caller that
		        // // treats empty as cancel cannot load a file whose path is genuinely
		        // // // empty. Both are rare, which is why this has survived.
		// These return empty strings if cancelled
		// A raw const char* filter, not std::string: the argument is typically a
		        // string literal, and passing one to a const char* parameter costs
		// nothing. The platform implementations parse this into a native
		// // dialog description (an NSOpenPanel allowed-file-types on macOS, a
		// // Windows OPENFILENAME filter string).
		// // DEFECT (null pointer, documented not fixed): the pointer is not
		// // documented as optional, so a caller passing nullptr passes a null
		// // pointer to whatever the platform dialog does with the filter, which
		// // // is undefined behaviour in strcmp-style parsing.
		// //
		// // Returning BY VALUE rather than an out-param: a std::string of a few
		// // hundred characters is cheap enough that the copy is not worth
		// // avoiding, and the signature is much clearer.
		static std::string OpenFile(const char* filter);
		// Same contract as OpenFile: empty means cancelled.
		//
		// DEFECT (overwrite, documented not fixed): unlike OpenFile, this method
		// // can return an existing path without the user having been asked to
		// // confirm the overwrite. Whether the native dialog prompts depends
		// // // entirely on how each platform implementation configures it, and
		// // // this header does not require it. A caller relying on the dialog to
		// // // protect an existing file has an unverified assumption.
		// //
		// // No exists-check helper appears here, so callers wanting to warn
		// // // before overwriting must do the filesystem check themselves.
		static std::string SaveFile(const char* filter);
	};

}