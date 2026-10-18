#include "wlpch.h"

#include "Wasteland/Avb/Recorder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if !defined(AVB_RECORDER_NO_MINIAUDIO_CAPTURE)
#include "miniaudio.h"
#endif

namespace Wasteland::Avb
{
	namespace
	{
		constexpr std::size_t kPcm16BytesPerSample = 2;
		constexpr std::uint32_t kRiffHeaderRemainder = 36;
#if defined(AVB_RECORDER_TEST_HOOKS)
		AvbStemPublicationTestHook g_StemPublicationTestHook = nullptr;
		void* g_StemPublicationTestContext = nullptr;
		AvbStemTempFileOpenedTestHook g_StemTempFileOpenedTestHook = nullptr;
		void* g_StemTempFileOpenedTestContext = nullptr;
		bool g_ForcePrimaryTempDispositionFailure = false;
#if !defined(_WIN32)
		bool g_ForceStageDirectoryOpenFailure = false;
#endif
#endif
		static_assert(std::atomic<std::size_t>::is_always_lock_free,
			"capture ring indexes must be lock-free on the audio callback thread");
		static_assert(std::atomic<bool>::is_always_lock_free,
			"capture recording state must be lock-free on the audio callback thread");

		void SetError(std::string* error, const char* message)
		{
			if (error)
				*error = message;
		}

		void WriteU16(std::uint8_t* bytes, std::uint16_t value) noexcept
		{
			bytes[0] = static_cast<std::uint8_t>(value & 0xff);
			bytes[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
		}

		void WriteU32(std::uint8_t* bytes, std::uint32_t value) noexcept
		{
			bytes[0] = static_cast<std::uint8_t>(value & 0xff);
			bytes[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
			bytes[2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
			bytes[3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
		}

		struct PinnedStemDirectory
		{
#if defined(_WIN32)
			HANDLE Handle = INVALID_HANDLE_VALUE;
			~PinnedStemDirectory() { if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle); }
#else
			int Handle = -1;
			~PinnedStemDirectory() { if (Handle >= 0) ::close(Handle); }
#endif
			PinnedStemDirectory() = default;
			PinnedStemDirectory(const PinnedStemDirectory&) = delete;
			PinnedStemDirectory& operator=(const PinnedStemDirectory&) = delete;
		};

		struct StemOutputTarget
		{
			std::shared_ptr<PinnedStemDirectory> Parent;
			std::filesystem::path Filename;
			std::filesystem::path DisplayPath;
		};

#if defined(_WIN32)
		// These layouts match the NT ABI used by NtCreateFile and NtSetInformationFile.
		// Dynamically resolving the native entry points avoids a new import dependency.
		using NativeStatus = LONG;
		struct NativeUnicodeString
		{
			USHORT Length;
			USHORT MaximumLength;
			PWSTR Buffer;
		};
		struct NativeObjectAttributes
		{
			ULONG Length;
			HANDLE RootDirectory;
			NativeUnicodeString* ObjectName;
			ULONG Attributes;
			PVOID SecurityDescriptor;
			PVOID SecurityQualityOfService;
		};
		struct NativeIoStatusBlock
		{
			union { NativeStatus Status; PVOID Pointer; };
			ULONG_PTR Information;
		};
		using NtCreateFileFunction = NativeStatus (NTAPI*)(PHANDLE, ACCESS_MASK,
			NativeObjectAttributes*, NativeIoStatusBlock*, PLARGE_INTEGER,
			ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
		using NtSetInformationFileFunction = NativeStatus (NTAPI*)(HANDLE,
			NativeIoStatusBlock*, PVOID, ULONG, ULONG);
		struct NativeFileRenameInformation
		{
			BOOLEAN ReplaceIfExists;
			HANDLE RootDirectory;
			ULONG FileNameLength;
			WCHAR FileName[1];
		};
		constexpr ULONG kFileRenameInformationClass = 10;
		constexpr NativeStatus kStatusObjectNameNotFound = static_cast<NativeStatus>(0xC0000034L);
		constexpr NativeStatus kStatusObjectPathNotFound = static_cast<NativeStatus>(0xC000003AL);
		constexpr NativeStatus kStatusObjectNameCollision = static_cast<NativeStatus>(0xC0000035L);
		constexpr ULONG kFileDirectoryOption = 0x00000001;
		constexpr ULONG kFileNonDirectoryOption = 0x00000040;
		constexpr ULONG kFileOpenReparsePointOption = 0x00200000;
		constexpr ULONG kFileSynchronousIoNonAlertOption = 0x00000020;
		constexpr ULONG kFileOpenDisposition = 1;
		constexpr ULONG kFileCreateDisposition = 2;
		constexpr ULONG kObjectCaseInsensitive = 0x00000040;

		NtCreateFileFunction GetNtCreateFile()
		{
			static NtCreateFileFunction function = [] {
				HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
				return ntdll ? reinterpret_cast<NtCreateFileFunction>(GetProcAddress(ntdll, "NtCreateFile")) : nullptr;
			}();
			return function;
		}

		NtSetInformationFileFunction GetNtSetInformationFile()
		{
			static NtSetInformationFileFunction function = [] {
				HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
				return ntdll ? reinterpret_cast<NtSetInformationFileFunction>(GetProcAddress(ntdll, "NtSetInformationFile")) : nullptr;
			}();
			return function;
		}

		// Every name passed here is a single validated component. RootDirectory pins
		// lookup to the already-open parent instead of re-walking a pathname.
		bool OpenRelativeNative(HANDLE parent, const std::filesystem::path& filename,
			ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options,
			ULONG attributes, HANDLE& handle, NativeStatus& status)
		{
			const std::wstring name = filename.native();
			const std::size_t nameBytes = name.size() * sizeof(wchar_t);
			if (name.empty() || nameBytes > (std::numeric_limits<USHORT>::max)())
				return false;
			NtCreateFileFunction createFile = GetNtCreateFile();
			if (!createFile)
				return false;
			NativeUnicodeString objectName{};
			objectName.Length = static_cast<USHORT>(nameBytes);
			objectName.MaximumLength = objectName.Length;
			objectName.Buffer = const_cast<PWSTR>(name.c_str());
			NativeObjectAttributes objectAttributes{};
			objectAttributes.Length = sizeof(objectAttributes);
			objectAttributes.RootDirectory = parent;
			objectAttributes.ObjectName = &objectName;
			objectAttributes.Attributes = kObjectCaseInsensitive;
			NativeIoStatusBlock ioStatus{};
			handle = INVALID_HANDLE_VALUE;
			status = createFile(&handle, access, &objectAttributes, &ioStatus, nullptr,
				attributes, share, disposition, options, nullptr, 0);
			return status >= 0;
		}

		bool IsNativeNotFound(NativeStatus status)
		{
			return status == kStatusObjectNameNotFound || status == kStatusObjectPathNotFound;
		}

		bool IsOrdinaryDirectory(HANDLE handle)
		{
			FILE_ATTRIBUTE_TAG_INFO info{};
			return GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) &&
				(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
				(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
		}

		bool OpenOrCreateDirectoryChild(HANDLE parent, const std::filesystem::path& name,
			HANDLE& child, bool willContainPublishedFiles = false)
		{
			constexpr ULONG share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
			constexpr ULONG options = kFileDirectoryOption | kFileOpenReparsePointOption |
				kFileSynchronousIoNonAlertOption;
			const ACCESS_MASK access = FILE_LIST_DIRECTORY | FILE_TRAVERSE |
				FILE_READ_ATTRIBUTES | SYNCHRONIZE |
				(willContainPublishedFiles ? (FILE_ADD_FILE | FILE_DELETE_CHILD) : 0);
			NativeStatus status = 0;
			if (!OpenRelativeNative(parent, name, access, share, kFileOpenDisposition,
				options, FILE_ATTRIBUTE_DIRECTORY, child, status))
			{
				if (!IsNativeNotFound(status))
					return false;
				if (!OpenRelativeNative(parent, name, access, share, kFileCreateDisposition,
					options, FILE_ATTRIBUTE_DIRECTORY, child, status) &&
					status != kStatusObjectNameCollision)
					return false;
				if (status == kStatusObjectNameCollision &&
					!OpenRelativeNative(parent, name, access, share, kFileOpenDisposition,
						options, FILE_ATTRIBUTE_DIRECTORY, child, status))
					return false;
			}
			if (!IsOrdinaryDirectory(child))
			{
				CloseHandle(child);
				child = INVALID_HANDLE_VALUE;
				return false;
			}
			return true;
		}

		bool OpenOrCreatePinnedRoot(const std::filesystem::path& root,
			std::shared_ptr<PinnedStemDirectory>& pinnedRoot, std::string* error)
		{
			const std::filesystem::path anchorPath = root.root_path();
			HANDLE anchor = CreateFileW(anchorPath.c_str(),
				FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (anchor == INVALID_HANDLE_VALUE || !IsOrdinaryDirectory(anchor))
			{
				if (anchor != INVALID_HANDLE_VALUE) CloseHandle(anchor);
				SetError(error, "could not securely open stem output root");
				return false;
			}
			auto current = std::make_shared<PinnedStemDirectory>();
			current->Handle = anchor;
			std::vector<std::filesystem::path> components;
			for (const auto& component : root)
			{
				if (component == root.root_name() || component == root.root_directory() ||
					component.empty() || component == L".")
					continue;
				components.push_back(component);
			}
			for (std::size_t i = 0; i < components.size(); ++i)
			{
				auto next = std::make_shared<PinnedStemDirectory>();
				if (!OpenOrCreateDirectoryChild(current->Handle, components[i], next->Handle,
					i + 1 == components.size()))
				{
					SetError(error, "could not securely create or open a stem output directory");
					return false;
				}
				current = std::move(next);
			}
			pinnedRoot = std::move(current);
			return true;
		}

		bool OpenPinnedChildDirectory(const std::shared_ptr<PinnedStemDirectory>& parent,
			const std::filesystem::path& name, std::shared_ptr<PinnedStemDirectory>& child,
			bool willContainPublishedFiles = false)
		{
			auto opened = std::make_shared<PinnedStemDirectory>();
			if (!OpenOrCreateDirectoryChild(parent->Handle, name, opened->Handle,
				willContainPublishedFiles))
				return false;
			child = std::move(opened);
			return true;
		}
#else
		int OpenDirectoryAt(int parent, const std::filesystem::path& name)
		{
#if defined(O_NOFOLLOW) && defined(O_DIRECTORY)
			int flags = O_RDONLY | O_NOFOLLOW | O_DIRECTORY;
#ifdef O_CLOEXEC
			flags |= O_CLOEXEC;
#endif
			return ::openat(parent, name.c_str(), flags);
#else
			(void)parent;
			(void)name;
			errno = ENOTSUP;
			return -1;
#endif
		}

		bool OpenOrCreateDirectoryChild(int parent, const std::filesystem::path& name, int& child,
			bool willContainPublishedFiles = false)
		{
			(void)willContainPublishedFiles;
			child = OpenDirectoryAt(parent, name);
			if (child < 0 && errno == ENOENT)
			{
				if (::mkdirat(parent, name.c_str(), 0777) != 0 && errno != EEXIST)
					return false;
				child = OpenDirectoryAt(parent, name);
			}
			if (child < 0)
				return false;
			struct stat info{};
			if (::fstat(child, &info) != 0 || !S_ISDIR(info.st_mode))
			{
				::close(child);
				child = -1;
				return false;
			}
			return true;
		}

		bool OpenOrCreatePinnedRoot(const std::filesystem::path& root,
			std::shared_ptr<PinnedStemDirectory>& pinnedRoot, std::string* error)
		{
#if !defined(O_NOFOLLOW) || !defined(O_DIRECTORY)
			(void)root;
			(void)pinnedRoot;
			SetError(error, "this platform lacks no-follow directory-relative file operations");
			return false;
#else
			int anchor = ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW
#ifdef O_CLOEXEC
				| O_CLOEXEC
#endif
			);
			if (anchor < 0)
			{
				SetError(error, "could not securely open the filesystem root for stem output");
				return false;
			}
			auto current = std::make_shared<PinnedStemDirectory>();
			current->Handle = anchor;
			for (auto component = root.begin(); component != root.end(); ++component)
			{
				if (component->empty() || *component == root.root_directory() || *component == ".")
					continue;
				if (*component == "..")
				{
					SetError(error, "stem output root must be normalized before secure opening");
					return false;
				}
				auto next = std::make_shared<PinnedStemDirectory>();
				if (!OpenOrCreateDirectoryChild(current->Handle, *component, next->Handle))
				{
					SetError(error, "could not securely create or open a stem output directory");
					return false;
				}
				current = std::move(next);
			}
			pinnedRoot = std::move(current);
			return true;
#endif
		}

		bool OpenPinnedChildDirectory(const std::shared_ptr<PinnedStemDirectory>& parent,
			const std::filesystem::path& name, std::shared_ptr<PinnedStemDirectory>& child,
			bool willContainPublishedFiles = false)
		{
			auto opened = std::make_shared<PinnedStemDirectory>();
			if (!OpenOrCreateDirectoryChild(parent->Handle, name, opened->Handle,
				willContainPublishedFiles))
				return false;
			child = std::move(opened);
			return true;
		}
#endif

		bool ResolveStemOutput(const std::filesystem::path& root,
			const std::shared_ptr<PinnedStemDirectory>& pinnedRoot,
			const std::filesystem::path& relative, StemOutputTarget& output,
			std::string* error);

		struct SecureStemTempFile
		{
			std::shared_ptr<PinnedStemDirectory> Parent;
			std::shared_ptr<PinnedStemDirectory> Owner;
			std::filesystem::path Name;
			std::filesystem::path DirectoryName;
			bool Published = false;
#if defined(_WIN32)
			HANDLE Handle = INVALID_HANDLE_VALUE;
			~SecureStemTempFile()
			{
				if (Handle != INVALID_HANDLE_VALUE)
				{
					if (!Published)
					{
						BOOL primarySucceeded = FALSE;
#if defined(AVB_RECORDER_TEST_HOOKS)
						if (g_ForcePrimaryTempDispositionFailure)
							SetLastError(ERROR_ACCESS_DENIED);
						else
#endif
						{
						FILE_DISPOSITION_INFO disposition{ TRUE };
						primarySucceeded = SetFileInformationByHandle(
							Handle, FileDispositionInfo, &disposition, sizeof(disposition));
						}
						if (!primarySucceeded)
						{
							const DWORD primaryError = GetLastError();
						struct TempFileDispositionInfoEx
						{
							DWORD Flags;
						} dispositionEx{ 0x00000001u | 0x00000002u }; // DELETE | POSIX_SEMANTICS
							constexpr auto fileDispositionInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
							if (!SetFileInformationByHandle(Handle, fileDispositionInfoEx,
								&dispositionEx, sizeof(dispositionEx)))
							{
								const DWORD fallbackError = GetLastError();
								std::fprintf(stderr,
									"AvbRecorder: failed to remove unpublished temporary WAV (errors %lu, %lu); a partial file may remain\n",
									static_cast<unsigned long>(primaryError),
									static_cast<unsigned long>(fallbackError));
							}
						}
					}
					CloseHandle(Handle);
				}
			}
#else
			int Handle = -1;
			dev_t StagingDevice = 0;
			ino_t StagingInode = 0;
			bool HasStagingIdentity = false;
			~SecureStemTempFile()
			{
				if (Handle >= 0)
					::close(Handle);
				if (!Published && Parent && Parent->Handle >= 0 && !Name.empty())
					::unlinkat(Parent->Handle, Name.c_str(), 0);
				if (Owner && Owner->Handle >= 0 && !DirectoryName.empty() && HasStagingIdentity)
				{
					struct stat namedInfo{};
					if (::fstatat(Owner->Handle, DirectoryName.c_str(), &namedInfo, AT_SYMLINK_NOFOLLOW) == 0 &&
						S_ISDIR(namedInfo.st_mode) && namedInfo.st_dev == StagingDevice &&
						namedInfo.st_ino == StagingInode)
						::unlinkat(Owner->Handle, DirectoryName.c_str(), AT_REMOVEDIR);
				}
			}
#endif
		};

		std::atomic<std::uint64_t> g_StemTempSequence{ 0 };

		bool CreateSecureStemTempFile(const std::shared_ptr<PinnedStemDirectory>& parent,
			const std::filesystem::path& displayParent, SecureStemTempFile& file)
		{
#if !defined(_WIN32) || !defined(AVB_RECORDER_TEST_HOOKS)
			(void)displayParent;
#endif
			file.Parent = parent;
			for (unsigned attempt = 0; attempt < 128; ++attempt)
			{
				const std::uint64_t sequence = g_StemTempSequence.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
				file.Name = std::filesystem::path(L".avb-stem-" + std::to_wstring(GetCurrentProcessId()) +
					L"-" + std::to_wstring(sequence) + L".tmp");
				NativeStatus status = 0;
				constexpr ACCESS_MASK access = GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
				// Prevent other handles from reading partial capture data before publication.
				if (OpenRelativeNative(parent->Handle, file.Name, access, 0,
					kFileCreateDisposition, kFileNonDirectoryOption | kFileOpenReparsePointOption |
					kFileSynchronousIoNonAlertOption, FILE_ATTRIBUTE_TEMPORARY, file.Handle, status))
				{
#if defined(AVB_RECORDER_TEST_HOOKS)
					if (g_StemTempFileOpenedTestHook)
					{
						const std::string tempPath = (displayParent / file.Name).u8string();
						g_StemTempFileOpenedTestHook(tempPath, g_StemTempFileOpenedTestContext);
					}
#endif
					return true;
				}
				if (status != kStatusObjectNameCollision)
					return false;
#else
#if defined(O_NOFOLLOW) && defined(O_DIRECTORY)
				file.Owner = parent;
				// Keep the rename source in an owner-only directory opened by handle;
				// this blocks different-UID output-directory writers, not same-UID processes.
				file.DirectoryName = ".avb-stem-stage-" +
					std::to_string(static_cast<unsigned long long>(::getpid())) + "-" +
					std::to_string(static_cast<unsigned long long>(sequence));
				if (::mkdirat(parent->Handle, file.DirectoryName.c_str(), 0700) != 0)
				{
					if (errno == EEXIST)
						continue;
					return false;
				}
				struct stat namedInfo{};
				if (::fstatat(parent->Handle, file.DirectoryName.c_str(), &namedInfo, AT_SYMLINK_NOFOLLOW) != 0)
				{
					// Without an inode identity, leave the entry untouched rather than unlink a substituted path.
					return false;
				}
				if (!S_ISDIR(namedInfo.st_mode) || namedInfo.st_uid != ::geteuid())
					return false;
				file.StagingDevice = namedInfo.st_dev;
				file.StagingInode = namedInfo.st_ino;
				file.HasStagingIdentity = true;
				if ((namedInfo.st_mode & 0077) != 0)
					return false;
				auto stagingDirectory = std::make_shared<PinnedStemDirectory>();
			#if defined(AVB_RECORDER_TEST_HOOKS)
				if (g_ForceStageDirectoryOpenFailure)
				{
					errno = EACCES;
					stagingDirectory->Handle = -1;
				}
				else
			#endif
				{
				stagingDirectory->Handle = OpenDirectoryAt(parent->Handle, file.DirectoryName);
				}
				struct stat stagingInfo{};
				if (stagingDirectory->Handle < 0 || ::fstat(stagingDirectory->Handle, &stagingInfo) != 0 ||
					!S_ISDIR(stagingInfo.st_mode) || stagingInfo.st_uid != ::geteuid() ||
					(stagingInfo.st_mode & 0077) != 0 || stagingInfo.st_dev != file.StagingDevice ||
					stagingInfo.st_ino != file.StagingInode)
					return false;
				file.Parent = std::move(stagingDirectory);
				file.Name = ".avb-stem-" + std::to_string(static_cast<unsigned long long>(sequence)) + ".tmp";
				int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
#ifdef O_CLOEXEC
				flags |= O_CLOEXEC;
#endif
				file.Handle = ::openat(file.Parent->Handle, file.Name.c_str(), flags, 0600);
				if (file.Handle >= 0)
					return true;
				file.Name.clear();
				return false;
#else
				(void)sequence;
				return false;
#endif
#endif
			}
			return false;
		}

		bool WriteSecureStemBytes(SecureStemTempFile& file, const std::uint8_t* bytes,
			std::size_t length)
		{
			while (length != 0)
			{
#if defined(_WIN32)
				const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(length, MAXDWORD));
				DWORD written = 0;
				if (!WriteFile(file.Handle, bytes, chunk, &written, nullptr) || written == 0)
					return false;
				bytes += written;
				length -= written;
#else
				const ssize_t written = ::write(file.Handle, bytes, length);
				if (written < 0 && errno == EINTR)
					continue;
				if (written <= 0)
					return false;
				bytes += static_cast<std::size_t>(written);
				length -= static_cast<std::size_t>(written);
#endif
			}
			return true;
		}

		bool PublishSecureStemTemp(SecureStemTempFile& file, const StemOutputTarget& target)
		{
#if defined(_WIN32)
			const std::wstring destination = target.Filename.native();
			const std::size_t nameBytes = destination.size() * sizeof(wchar_t);
			if (nameBytes > (std::numeric_limits<ULONG>::max)() - sizeof(NativeFileRenameInformation) ||
				!FlushFileBuffers(file.Handle))
				return false;
			std::vector<std::uint8_t> storage(sizeof(NativeFileRenameInformation) - sizeof(WCHAR) + nameBytes);
			auto* renameInfo = reinterpret_cast<NativeFileRenameInformation*>(storage.data());
			std::memset(renameInfo, 0, storage.size());
			renameInfo->ReplaceIfExists = TRUE;
			renameInfo->RootDirectory = target.Parent->Handle;
			renameInfo->FileNameLength = static_cast<ULONG>(nameBytes);
			std::memcpy(renameInfo->FileName, destination.data(), nameBytes);
			// Class 10 is FileRenameInformation: atomically replace the leaf relative
			// to the pinned destination directory while renaming the held temp handle.
			NtSetInformationFileFunction setInformation = GetNtSetInformationFile();
			if (!setInformation)
				return false;
			NativeIoStatusBlock ioStatus{};
			const NativeStatus status = setInformation(file.Handle, &ioStatus, renameInfo,
				static_cast<ULONG>(storage.size()), kFileRenameInformationClass);
			if (status < 0)
			{
				SetLastError(static_cast<DWORD>(status));
				return false;
			}
			file.Published = true;
			return true;
#else
			if (::fsync(file.Handle) != 0 ||
				::renameat(file.Parent->Handle, file.Name.c_str(), target.Parent->Handle,
					target.Filename.c_str()) != 0)
				return false;
			file.Published = true;
			return true;
#endif
		}

		bool WriteStereoWav(const StemOutputTarget& target,
		                    const std::vector<float>& samples,
		                    std::uint32_t sampleRate,
		                    std::string* error)
		{
			if (samples.empty() || samples.size() % 2 != 0 || sampleRate == 0)
			{
				SetError(error, "stereo WAV requires non-empty interleaved samples and a positive sample rate");
				return false;
			}

			const std::uint32_t maxDataBytes =
				(std::numeric_limits<std::uint32_t>::max)() - kRiffHeaderRemainder;
			if (samples.size() > static_cast<std::size_t>(maxDataBytes / kPcm16BytesPerSample))
			{
				SetError(error, "stereo WAV exceeds the RIFF 32-bit size limit");
				return false;
			}
			const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples.size() * kPcm16BytesPerSample);
			const std::uint64_t byteRate = static_cast<std::uint64_t>(sampleRate) * 4u;
			if (byteRate > (std::numeric_limits<std::uint32_t>::max)())
			{
				SetError(error, "stereo WAV sample rate exceeds the RIFF byte-rate limit");
				return false;
			}

			std::array<std::uint8_t, 44> header{};
			std::memcpy(header.data(), "RIFF", 4);
			WriteU32(header.data() + 4, kRiffHeaderRemainder + dataBytes);
			std::memcpy(header.data() + 8, "WAVE", 4);
			std::memcpy(header.data() + 12, "fmt ", 4);
			WriteU32(header.data() + 16, 16);
			WriteU16(header.data() + 20, 1);
			WriteU16(header.data() + 22, 2);
			WriteU32(header.data() + 24, sampleRate);
			WriteU32(header.data() + 28, static_cast<std::uint32_t>(byteRate));
			WriteU16(header.data() + 32, 4);
			WriteU16(header.data() + 34, 16);
			std::memcpy(header.data() + 36, "data", 4);
			WriteU32(header.data() + 40, dataBytes);

			SecureStemTempFile file;
			if (!target.Parent || !CreateSecureStemTempFile(target.Parent,
				target.DisplayPath.parent_path(), file))
			{
				SetError(error, "could not create a new temporary stem output file relative to its pinned directory");
				return false;
			}
			if (!WriteSecureStemBytes(file, header.data(), header.size()))
			{
				SetError(error, "failed writing stereo WAV header");
				return false;
			}

			std::array<std::uint8_t, 16384> pcm{};
			std::size_t buffered = 0;
			for (float sample : samples)
			{
				double value = std::isfinite(sample) ? static_cast<double>(sample) : 0.0;
				value = std::max(-1.0, std::min(1.0, value));
				long quantized = std::lround(value * 32768.0);
				quantized = std::max(-32768L, std::min(32767L, quantized));
				const std::uint16_t encoded = static_cast<std::uint16_t>(static_cast<std::int16_t>(quantized));
				pcm[buffered++] = static_cast<std::uint8_t>(encoded & 0xff);
				pcm[buffered++] = static_cast<std::uint8_t>((encoded >> 8) & 0xff);
				if (buffered == pcm.size())
				{
					if (!WriteSecureStemBytes(file, pcm.data(), buffered))
					{
						SetError(error, "failed writing stereo WAV samples");
						return false;
					}
					buffered = 0;
				}
			}
			if (buffered != 0 && !WriteSecureStemBytes(file, pcm.data(), buffered))
			{
				SetError(error, "failed writing stereo WAV samples");
				return false;
			}
			if (!PublishSecureStemTemp(file, target))
			{
#if defined(_WIN32)
				const unsigned long nativeError = GetLastError();
#else
				const unsigned long nativeError = static_cast<unsigned long>(errno);
#endif
				if (error)
					*error = std::string("could not atomically publish stereo WAV output in its pinned directory (native error ") +
						std::to_string(nativeError) + ")";
				return false;
			}
			return true;
		}

		bool WriteMonoWav(const StemOutputTarget& target, const std::vector<float>& samples,
			std::uint32_t sampleRate, std::string* error)
		{
			if (samples.empty() || sampleRate == 0)
			{
				SetError(error, "mono WAV requires non-empty samples and a positive sample rate");
				return false;
			}

			const std::uint32_t maxDataBytes =
				(std::numeric_limits<std::uint32_t>::max)() - kRiffHeaderRemainder;
			if (samples.size() > static_cast<std::size_t>(maxDataBytes / kPcm16BytesPerSample))
			{
				SetError(error, "mono WAV exceeds the RIFF 32-bit size limit");
				return false;
			}
			const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples.size() * kPcm16BytesPerSample);
			const std::uint64_t byteRate = static_cast<std::uint64_t>(sampleRate) * kPcm16BytesPerSample;
			if (byteRate > (std::numeric_limits<std::uint32_t>::max)())
			{
				SetError(error, "mono WAV sample rate exceeds the RIFF byte-rate limit");
				return false;
			}

			std::array<std::uint8_t, 44> header{};
			std::memcpy(header.data(), "RIFF", 4);
			WriteU32(header.data() + 4, kRiffHeaderRemainder + dataBytes);
			std::memcpy(header.data() + 8, "WAVE", 4);
			std::memcpy(header.data() + 12, "fmt ", 4);
			WriteU32(header.data() + 16, 16);
			WriteU16(header.data() + 20, 1);
			WriteU16(header.data() + 22, 1);
			WriteU32(header.data() + 24, sampleRate);
			WriteU32(header.data() + 28, static_cast<std::uint32_t>(byteRate));
			WriteU16(header.data() + 32, kPcm16BytesPerSample);
			WriteU16(header.data() + 34, 16);
			std::memcpy(header.data() + 36, "data", 4);
			WriteU32(header.data() + 40, dataBytes);

			SecureStemTempFile file;
			if (!target.Parent || !CreateSecureStemTempFile(target.Parent,
				target.DisplayPath.parent_path(), file))
			{
				SetError(error, "could not create a new temporary recording output file in its pinned directory");
				return false;
			}
			if (!WriteSecureStemBytes(file, header.data(), header.size()))
			{
				SetError(error, "failed writing mono WAV header");
				return false;
			}

			std::array<std::uint8_t, 16384> pcm{};
			std::size_t buffered = 0;
			for (float sample : samples)
			{
				double value = std::isfinite(sample) ? static_cast<double>(sample) : 0.0;
				value = std::max(-1.0, std::min(1.0, value));
				long quantized = std::lround(value * 32768.0);
				quantized = std::max(-32768L, std::min(32767L, quantized));
				const std::uint16_t encoded = static_cast<std::uint16_t>(static_cast<std::int16_t>(quantized));
				pcm[buffered++] = static_cast<std::uint8_t>(encoded & 0xff);
				pcm[buffered++] = static_cast<std::uint8_t>((encoded >> 8) & 0xff);
				if (buffered == pcm.size())
				{
					if (!WriteSecureStemBytes(file, pcm.data(), buffered))
					{
						SetError(error, "failed writing mono WAV samples");
						return false;
					}
					buffered = 0;
				}
			}
			if (buffered != 0 && !WriteSecureStemBytes(file, pcm.data(), buffered))
			{
				SetError(error, "failed writing mono WAV samples");
				return false;
			}
			if (!PublishSecureStemTemp(file, target))
			{
#if defined(_WIN32)
				const unsigned long nativeError = GetLastError();
#else
				const unsigned long nativeError = static_cast<unsigned long>(errno);
#endif
				if (error)
					*error = std::string("could not atomically publish mono WAV output in its pinned directory (native error ") +
						std::to_string(nativeError) + ")";
				return false;
			}
			return true;
		}

		std::string SafePathComponent(const std::string& value)
		{
			std::string safe;
			safe.reserve(value.size());
			for (unsigned char character : value)
			{
				const bool asciiAlphaNumeric = (character >= 'a' && character <= 'z') ||
					(character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
				if (asciiAlphaNumeric || character == '-' || character == '_' || character == '.' || character == ' ')
					safe.push_back(static_cast<char>(character));
				else
					safe.push_back('_');
			}
			if (safe.empty() || safe == "." || safe == "..")
				return "_";
			return safe;
		}

		std::string StemPathCollisionKey(const std::filesystem::path& path)
		{
			std::string key = path.lexically_normal().generic_string();
#if defined(_WIN32)
			// SafePathComponent restricts generated path components to ASCII, so
			// ASCII folding matches Windows' case-insensitive stem-name behavior.
			for (char& character : key)
				if (character >= 'A' && character <= 'Z')
					character = static_cast<char>(character - 'A' + 'a');
#endif
			return key;
		}

		bool ExpandPattern(const AvbStemRenderOptions& options, const AvbStemSpec& stem,
		                   std::filesystem::path& relativePath, std::string* error)
		{
			if (options.FilenamePattern.empty())
			{
				SetError(error, "stem filename pattern is empty");
				return false;
			}
			std::string pattern;
			pattern.reserve(options.FilenamePattern.size());
			for (char character : options.FilenamePattern)
				pattern.push_back(character == '\\' ? '/' : character);
			if (pattern.front() == '/')
			{
				SetError(error, "stem filename pattern must be relative to the output root");
				return false;
			}
			std::string patternExtension = std::filesystem::path(pattern).extension().string();
			std::transform(patternExtension.begin(), patternExtension.end(), patternExtension.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			if (!patternExtension.empty() && patternExtension != ".wav")
			{
				SetError(error, "stem export only writes WAV files");
				return false;
			}

			const std::string project = SafePathComponent(options.ProjectName);
			const std::string region = SafePathComponent(options.RegionName);
			const std::string track = SafePathComponent(stem.TrackName);
			std::string expanded;
			expanded.reserve(pattern.size() + project.size() + region.size() + track.size());
			for (std::size_t i = 0; i < pattern.size();)
			{
				if (pattern[i] != '$')
				{
					expanded.push_back(pattern[i++]);
					continue;
				}
				if (pattern.compare(i, 8, "$project") == 0)
				{
					expanded += project;
					i += 8;
				}
				else if (pattern.compare(i, 7, "$region") == 0)
				{
					expanded += region;
					i += 7;
				}
				else if (pattern.compare(i, 6, "$track") == 0)
				{
					expanded += track;
					i += 6;
				}
				else
				{
					SetError(error, "stem filename pattern contains an unknown wildcard");
					return false;
				}
			}

			if (expanded.find(':') != std::string::npos)
			{
				SetError(error, "stem filename pattern contains a drive or alternate-stream separator");
				return false;
			}
			relativePath = std::filesystem::path(expanded);
			if (relativePath.empty() || relativePath.is_absolute() || relativePath.has_root_name() ||
				relativePath.has_root_directory())
			{
				SetError(error, "stem filename pattern must remain below the output root");
				return false;
			}
			for (const auto& component : relativePath)
			{
				if (component == ".." || component == ".")
				{
					SetError(error, "stem filename pattern may not traverse outside the output root");
					return false;
				}
			}

			if (patternExtension.empty())
				relativePath += ".wav";
			return true;
		}

		bool ResolveStemOutput(const std::filesystem::path& root,
			const std::shared_ptr<PinnedStemDirectory>& pinnedRoot,
			const std::filesystem::path& relative, StemOutputTarget& output,
			std::string* error)
		{
			auto parent = pinnedRoot;
			auto component = relative.begin();
			for (; component != relative.end(); ++component)
			{
				auto next = component;
				++next;
				if (next == relative.end())
					break;
				auto afterNext = next;
				++afterNext;
				const bool willContainPublishedFiles = afterNext == relative.end();
				std::shared_ptr<PinnedStemDirectory> child;
				if (!OpenPinnedChildDirectory(parent, *component, child, willContainPublishedFiles))
				{
					SetError(error, "stem output path contains a non-directory component");
					return false;
				}
				parent = std::move(child);
			}

			const std::filesystem::path filename = relative.filename();
			if (filename.empty())
			{
				SetError(error, "stem output pattern must end with a filename");
				return false;
			}
			output.Parent = std::move(parent);
			output.Filename = filename;
			output.DisplayPath = root;
			for (const auto& pathComponent : relative)
				output.DisplayPath /= pathComponent;
#if defined(_WIN32)
			HANDLE existing = INVALID_HANDLE_VALUE;
			NativeStatus status = 0;
			if (OpenRelativeNative(output.Parent->Handle, filename, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, kFileOpenDisposition,
				kFileOpenReparsePointOption | kFileSynchronousIoNonAlertOption, 0, existing, status))
			{
				FILE_ATTRIBUTE_TAG_INFO info{};
				const bool queried = GetFileInformationByHandleEx(existing, FileAttributeTagInfo, &info, sizeof(info));
				const bool reparse = queried && (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
				const bool regular = queried && (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
					GetFileType(existing) == FILE_TYPE_DISK;
				CloseHandle(existing);
				if (!queried)
				{
					SetError(error, "could not inspect stem output path");
					return false;
				}
				if (reparse)
				{
					SetError(error, "stem output may not replace a symbolic link");
					return false;
				}
				if (!regular)
				{
					SetError(error, "stem output path is not a regular file");
					return false;
				}
			}
			else if (!IsNativeNotFound(status))
			{
				SetError(error, "could not inspect stem output path");
				return false;
			}

#else
			struct stat info{};
			if (::fstatat(output.Parent->Handle, filename.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0)
			{
				if (!S_ISREG(info.st_mode))
				{
					SetError(error, S_ISLNK(info.st_mode)
						? "stem output may not replace a symbolic link"
						: "stem output path is not a regular file");
					return false;
				}
			}
			else if (errno != ENOENT)
			{
				SetError(error, "could not inspect stem output path");
				return false;
			}
#endif
			return true;
		}
	}

#if defined(AVB_RECORDER_TEST_HOOKS)
	void SetAvbStemPublicationTestHookForTesting(AvbStemPublicationTestHook hook,
		void* context) noexcept
	{
		g_StemPublicationTestContext = context;
		g_StemPublicationTestHook = hook;
	}

	void SetAvbStemTempFileOpenedTestHookForTesting(AvbStemTempFileOpenedTestHook hook,
		void* context) noexcept
	{
		g_StemTempFileOpenedTestContext = context;
		g_StemTempFileOpenedTestHook = hook;
	}

	void SetAvbForcePrimaryTempDispositionFailureForTesting(bool force) noexcept
	{
		g_ForcePrimaryTempDispositionFailure = force;
	}
#if !defined(_WIN32)
	void SetAvbForceStageDirectoryOpenFailureForTesting(bool force) noexcept
	{
		g_ForceStageDirectoryOpenFailure = force;
	}
#endif
#endif

	struct AvbRecorder::Impl
	{
		explicit Impl(std::size_t capacityFrames)
		{
			if (capacityFrames == 0)
				throw std::invalid_argument("capture ring capacity must be positive");
			Ring.resize(capacityFrames);
		}

		bool PushMono(float sample) noexcept
		{
			const std::size_t head = Head.load(std::memory_order_relaxed);
			const std::size_t tail = Tail.load(std::memory_order_acquire);
			if (head - tail >= Ring.size())
				return false;
			Ring[head % Ring.size()] = sample;
			Head.store(head + 1, std::memory_order_release);
			return true;
		}

		std::size_t Drain()
		{
			const std::size_t tail = Tail.load(std::memory_order_relaxed);
			const std::size_t head = Head.load(std::memory_order_acquire);
			const std::size_t count = head - tail;
			if (count == 0)
				return 0;
			if (count > Accumulated.max_size() - Accumulated.size())
				throw std::length_error("capture accumulation exceeds vector capacity");
			Accumulated.reserve(Accumulated.size() + count);
			for (std::size_t offset = 0; offset < count; ++offset)
				Accumulated.push_back(Ring[(tail + offset) % Ring.size()]);
			Tail.store(head, std::memory_order_release);
			return count;
		}

		std::vector<float> Ring;
		std::atomic<std::size_t> Head{0};
		std::atomic<std::size_t> Tail{0};
		std::atomic<std::size_t> Dropped{0};
		std::atomic<bool> Recording{false};
		std::vector<float> Accumulated;
		std::string WavPath;
		StemOutputTarget Output;
#if !defined(AVB_RECORDER_NO_MINIAUDIO_CAPTURE)
		ma_device CaptureDevice{};
		bool DeviceInitialized = false;
		bool Capturing = false;

		static void CaptureCallback(ma_device* device, void*, const void* input,
		                           ma_uint32 frameCount)
		{
			auto* recorder = static_cast<AvbRecorder*>(device ? device->pUserData : nullptr);
			if (!recorder || !device || !input)
				return;
			recorder->IngestCaptureBlock(static_cast<const float*>(input),
				static_cast<std::size_t>(frameCount), device->capture.channels);
		}
#endif
	};

	AvbRecorder::AvbRecorder(std::size_t ringCapacityFrames)
		: m_Impl(std::make_unique<Impl>(ringCapacityFrames))
	{
	}

	AvbRecorder::~AvbRecorder()
	{
		StopCapture();
	}

	bool AvbRecorder::StartRecording(const std::string& wavPath, std::string* error)
	{
		if (error)
			error->clear();
		if (!m_Impl || m_Impl->Ring.empty() || wavPath.empty())
		{
			SetError(error, "recording requires a non-empty WAV path and capture ring");
			return false;
		}
		if (m_Impl->Recording.load(std::memory_order_acquire) || IsCapturing())
		{
			SetError(error, "recording is already active");
			return false;
		}

		std::error_code pathError;
		std::filesystem::path absoluteWavPath = std::filesystem::absolute(
			std::filesystem::path(wavPath), pathError);
		if (pathError || absoluteWavPath.empty() || absoluteWavPath.filename().empty())
		{
			SetError(error, "recording output path must name a WAV file");
			return false;
		}
		absoluteWavPath = absoluteWavPath.lexically_normal();
		const std::filesystem::path parent = absoluteWavPath.parent_path();
		const std::filesystem::path outputRoot = parent.lexically_normal();
		if (pathError || outputRoot.empty() || !outputRoot.is_absolute())
		{
			SetError(error, "recording output directory must resolve to an absolute path");
			return false;
		}
		std::shared_ptr<PinnedStemDirectory> pinnedParent;
		if (!OpenOrCreatePinnedRoot(outputRoot, pinnedParent, error))
			return false;
		StemOutputTarget output;
		if (!ResolveStemOutput(outputRoot, pinnedParent, absoluteWavPath.filename(), output, error))
			return false;

		m_Impl->Head.store(0, std::memory_order_relaxed);
		m_Impl->Tail.store(0, std::memory_order_relaxed);
		m_Impl->Dropped.store(0, std::memory_order_relaxed);
		m_Impl->Accumulated.clear();
		m_Impl->WavPath = wavPath;
		m_Impl->Output = std::move(output);
		m_Impl->Recording.store(true, std::memory_order_release);
		return true;
	}

	bool AvbRecorder::IngestCaptureBlock(const float* interleavedSamples,
	                                     std::size_t frames,
	                                     std::size_t channels) noexcept
	{
		if (!m_Impl || !m_Impl->Recording.load(std::memory_order_acquire) ||
			(channels != 1 && channels != 2) || (frames != 0 && interleavedSamples == nullptr))
			return false;
		if (frames > (std::numeric_limits<std::size_t>::max)() / channels)
			return false;
		if (frames == 0)
			return true;

		std::size_t accepted = 0;
		for (; accepted < frames; ++accepted)
		{
			float sample = interleavedSamples[accepted * channels];
			if (channels == 2)
				sample = (sample + interleavedSamples[accepted * channels + 1]) * 0.5f;
			if (!std::isfinite(sample))
				sample = 0.0f;
			sample = std::clamp(sample, -1.0f, 1.0f);
			if (!m_Impl->PushMono(sample))
				break;
		}
		if (accepted != frames)
			m_Impl->Dropped.fetch_add(frames - accepted, std::memory_order_relaxed);
		return accepted == frames;
	}

	std::size_t AvbRecorder::DrainCapture()
	{
		if (!m_Impl || !m_Impl->Recording.load(std::memory_order_acquire))
			return 0;
		return m_Impl->Drain();
	}

	bool AvbRecorder::StopRecording(AvbRecordedTake& take, std::string* error)
	{
		if (error)
			error->clear();
		take = AvbRecordedTake{};
		StopCapture();
		if (!m_Impl || !m_Impl->Recording.exchange(false, std::memory_order_acq_rel))
		{
			SetError(error, "recording is not active");
			return false;
		}

		try
		{
			m_Impl->Drain();
			const std::size_t droppedFrames = m_Impl->Dropped.load(std::memory_order_relaxed);
			if (droppedFrames != 0)
			{
				if (error)
					*error = "capture overrun dropped " + std::to_string(droppedFrames) +
						" frame(s); refusing to publish a truncated take";
				return false;
			}
			if (m_Impl->Accumulated.empty())
			{
				SetError(error, "recording contains no captured frames");
				return false;
			}
			auto clip = Audio::AudioClip::FromSamples(std::move(m_Impl->Accumulated),
				static_cast<int>(kAvbAudioSampleRate));
			if (!clip)
			{
				SetError(error, "could not create the recorded AudioClip");
				return false;
			}
			const StemOutputTarget& output = m_Impl->Output;
			if (!output.Parent || output.Filename.empty())
			{
				SetError(error, "recording output directory was not bound at start");
				return false;
			}
			if (!WriteMonoWav(output, clip->GetSamples(),
				static_cast<std::uint32_t>(clip->GetSampleRate()), error))
			{
				if (error && error->empty())
					SetError(error, "failed to securely write the recording WAV");
				return false;
			}
			take.Clip = std::move(clip);
			take.SourcePath = m_Impl->WavPath;
			take.SourceOffset = 0.0;
			take.FrameCount = take.Clip->GetFrameCount();
			return true;
		}
		catch (const std::exception& exception)
		{
			if (error)
				*error = exception.what();
			return false;
		}
	}

	bool AvbRecorder::StartCapture(std::string* error)
	{
		if (error)
			error->clear();
		if (!m_Impl || !m_Impl->Recording.load(std::memory_order_acquire))
		{
			SetError(error, "start recording before opening the capture device");
			return false;
		}
#if defined(AVB_RECORDER_NO_MINIAUDIO_CAPTURE)
		SetError(error, "miniaudio capture is disabled in this test build");
		return false;
#else
		if (m_Impl->Capturing)
			return true;
		ma_device_config config = ma_device_config_init(ma_device_type_capture);
		config.capture.format = ma_format_f32;
		config.capture.channels = 2;
		config.sampleRate = kAvbAudioSampleRate;
		config.periodSizeInMilliseconds = 10;
		config.dataCallback = &Impl::CaptureCallback;
		config.pUserData = this;
		ma_result result = ma_device_init(nullptr, &config, &m_Impl->CaptureDevice);
		if (result != MA_SUCCESS)
		{
			if (error)
				*error = std::string("miniaudio capture initialization failed: ") +
					ma_result_description(result);
			return false;
		}
		m_Impl->DeviceInitialized = true;
		if (m_Impl->CaptureDevice.sampleRate != kAvbAudioSampleRate ||
			m_Impl->CaptureDevice.capture.format != ma_format_f32 ||
			m_Impl->CaptureDevice.capture.channels != 2)
		{
			SetError(error, "miniaudio returned an unexpected capture callback format");
			ma_device_uninit(&m_Impl->CaptureDevice);
			m_Impl->DeviceInitialized = false;
			return false;
		}
		result = ma_device_start(&m_Impl->CaptureDevice);
		if (result != MA_SUCCESS)
		{
			if (error)
				*error = std::string("miniaudio capture start failed: ") + ma_result_description(result);
			ma_device_uninit(&m_Impl->CaptureDevice);
			m_Impl->DeviceInitialized = false;
			return false;
		}
		m_Impl->Capturing = true;
		return true;
#endif
	}

	void AvbRecorder::StopCapture() noexcept
	{
#if !defined(AVB_RECORDER_NO_MINIAUDIO_CAPTURE)
		if (m_Impl && m_Impl->DeviceInitialized)
		{
			ma_device_stop(&m_Impl->CaptureDevice);
			ma_device_uninit(&m_Impl->CaptureDevice);
			m_Impl->DeviceInitialized = false;
			m_Impl->Capturing = false;
		}
#endif
	}

	bool AvbRecorder::IsCapturing() const noexcept
	{
#if defined(AVB_RECORDER_NO_MINIAUDIO_CAPTURE)
		return false;
#else
		return m_Impl && m_Impl->Capturing;
#endif
	}

	std::size_t AvbRecorder::DroppedFrames() const noexcept
	{
		return m_Impl ? m_Impl->Dropped.load(std::memory_order_relaxed) : 0;
	}

	bool RenderStems(AudioBusGraph& graph, const AudioBusInput* inputs,
	                 std::size_t inputCount, std::size_t frames,
	                 const std::vector<AvbStemSpec>& stems,
	                 const AvbStemRenderOptions& options,
	                 std::vector<AvbRenderedStem>& rendered,
	                 std::string* error)
	{
		if (error)
			error->clear();
		if (frames == 0 || frames > (std::numeric_limits<std::size_t>::max)() / 2 ||
			(inputCount != 0 && inputs == nullptr) || stems.empty() || graph.MaxFrames() == 0 ||
			options.OutputRoot.empty())
		{
			SetError(error, "stem rendering requires prepared graph, source frames, stems, and an output root");
			return false;
		}

		try
		{
			std::filesystem::path root(options.OutputRoot);
			std::error_code ec;
			root = std::filesystem::absolute(root, ec);
			if (ec || root.empty())
			{
				SetError(error, "could not resolve stem output root to an absolute path");
				return false;
			}
			root = root.lexically_normal();
			if (!root.is_absolute())
			{
				SetError(error, "stem output root must be absolute after normalization");
				return false;
			}
			std::shared_ptr<PinnedStemDirectory> pinnedRoot;
			if (!OpenOrCreatePinnedRoot(root, pinnedRoot, error))
				return false;

			std::vector<StemOutputTarget> outputPaths;
			outputPaths.reserve(stems.size());
			std::set<std::string> distinctPaths;
			for (const AvbStemSpec& stem : stems)
			{
				if (!graph.GetBus(stem.Bus) || !graph.GetTapBuffer(stem.Bus, stem.Tap))
				{
					SetError(error, "stem matrix references a missing bus or tap");
					return false;
				}
				std::filesystem::path relative;
				if (!ExpandPattern(options, stem, relative, error))
					return false;
				StemOutputTarget output;
				if (!ResolveStemOutput(root, pinnedRoot, relative, output, error))
					return false;
				const std::string normalized = StemPathCollisionKey(output.DisplayPath);
				if (!distinctPaths.insert(normalized).second)
				{
					SetError(error, "stem matrix maps multiple tracks to the same output file");
					return false;
				}
				outputPaths.push_back(std::move(output));
			}

			for (std::size_t i = 0; i < inputCount; ++i)
			{
				if (!graph.GetBus(inputs[i].Bus))
				{
					SetError(error, "offline source matrix references a missing bus");
					return false;
				}
			}

			std::vector<AvbRenderedStem> result(stems.size());
			for (std::size_t i = 0; i < stems.size(); ++i)
			{
				result[i].TrackName = stems[i].TrackName;
				result[i].FilePath = outputPaths[i].DisplayPath.string();
				result[i].InterleavedStereo.resize(frames * 2);
			}
			std::vector<AudioBusInput> blockInputs(inputCount);
			std::vector<float> masterOutput(graph.MaxFrames() * 2, 0.0f);
			graph.Reset();
			for (std::size_t offset = 0; offset < frames;)
			{
				const std::size_t blockFrames = std::min(graph.MaxFrames(), frames - offset);
				for (std::size_t i = 0; i < inputCount; ++i)
				{
					blockInputs[i] = inputs[i];
					if (blockInputs[i].InterleavedStereo)
						blockInputs[i].InterleavedStereo += offset * 2;
				}
				if (!graph.Process(blockInputs.data(), blockInputs.size(), masterOutput.data(), blockFrames))
				{
					SetError(error, "AudioBusGraph rejected an offline render block");
					return false;
				}
				for (std::size_t stemIndex = 0; stemIndex < stems.size(); ++stemIndex)
				{
					const float* tap = graph.GetTapBuffer(stems[stemIndex].Bus, stems[stemIndex].Tap);
					if (!tap)
					{
					SetError(error, "AudioBusGraph did not expose a requested stem tap");
					return false;
				}
				std::copy_n(tap, blockFrames * 2,
					result[stemIndex].InterleavedStereo.data() + offset * 2);
				}
				offset += blockFrames;
			}

#if defined(AVB_RECORDER_TEST_HOOKS)
			if (g_StemPublicationTestHook)
				g_StemPublicationTestHook(g_StemPublicationTestContext);
#endif
			for (std::size_t i = 0; i < result.size(); ++i)
			{
				if (!WriteStereoWav(outputPaths[i], result[i].InterleavedStereo,
					kAvbAudioSampleRate, error))
					return false;
			}
			rendered = std::move(result);
			return true;
		}
		catch (const std::exception& exception)
		{
			if (error)
				*error = exception.what();
			return false;
		}
	}
}
