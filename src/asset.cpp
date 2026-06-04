#include <arch.hpp>

#if defined ARCH_MAC
	#include <CoreFoundation/CoreFoundation.h>
	#include <CoreServices/CoreServices.h>
	#include <pwd.h>
#endif

#if defined ARCH_WIN
	#include <windows.h>
	#include <shlobj.h>
	#include <shlwapi.h>
#endif

#if defined ARCH_LIN
	#include <unistd.h>
	#include <sys/types.h>
	#include <pwd.h>
#endif

#include <asset.hpp>
#include <system.hpp>
#include <settings.hpp>
#include <string.hpp>
#include <plugin/Plugin.hpp>
#include <engine/Module.hpp>
#include <app/common.hpp>
#include <osdialog.h>


namespace rack {
namespace asset {


#if defined ARCH_MAC
std::string getApplicationSupportDir();
#endif


static void initSystemDir() {
	if (!systemDir.empty())
		return;

	if (settings::devMode) {
		systemDir = system::getWorkingDirectory();
		return;
	}

	// Environment variable overrides
	const char* envSystem = getenv("RACK_SYSTEM_DIR");
	if (envSystem) {
		systemDir = envSystem;
		return;
	}

#if defined ARCH_MAC
	CFBundleRef bundle = CFBundleGetMainBundle();
	assert(bundle);

	// Check if we're running as a command-line program or an app bundle.
	CFURLRef bundleUrl = CFBundleCopyBundleURL(bundle);
	// Thanks Ken Thomases! https://stackoverflow.com/a/58369256/272642
	CFStringRef uti;
	if (CFURLCopyResourcePropertyForKey(bundleUrl, kCFURLTypeIdentifierKey, &uti, NULL) && uti && UTTypeConformsTo(uti, kUTTypeApplicationBundle)) {
		char bundleBuf[PATH_MAX];
		Boolean success = CFURLGetFileSystemRepresentation(bundleUrl, TRUE, (UInt8*) bundleBuf, sizeof(bundleBuf));
		assert(success);
		bundlePath = bundleBuf;
	}

	CFURLRef resourcesUrl = CFBundleCopyResourcesDirectoryURL(bundle);
	char resourcesBuf[PATH_MAX];
	Boolean success = CFURLGetFileSystemRepresentation(resourcesUrl, TRUE, (UInt8*) resourcesBuf, sizeof(resourcesBuf));
	assert(success);
	CFRelease(resourcesUrl);
	systemDir = resourcesBuf;
#endif
#if defined ARCH_WIN
	// Get path to executable
	wchar_t moduleBufW[MAX_PATH] = L"";
	DWORD length = GetModuleFileNameW(NULL, moduleBufW, LENGTHOF(moduleBufW));
	assert(length > 0);
	// Get directory of executable
	PathRemoveFileSpecW(moduleBufW);
	systemDir = string::UTF16toUTF8(moduleBufW);
#endif
#if defined ARCH_LIN
	// Use the current working directory as the default path on Linux.
	systemDir = system::getWorkingDirectory();
#endif
}


static void initUserDir() {
	if (!userDir.empty())
		return;

	if (settings::devMode) {
		userDir = systemDir;
		return;
	}

	// Environment variable overrides
	const char* envUser = getenv("RACK_USER_DIR");
	if (envUser) {
		userDir = envUser;
		return;
	}

#if defined ARCH_WIN
	// Get AppData/Local path
	WCHAR localBufW[MAX_PATH] = {};
	HRESULT localH = SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, localBufW);
	assert(SUCCEEDED(localH));
	std::string localDir = string::UTF16toUTF8(localBufW);

	// Usually C:/Users/<username>/AppData/Local/Rack2
	userDir = system::join(localDir, "Rack" + APP_VERSION_MAJOR);

	// Get Documents path
	WCHAR documentsBufW[MAX_PATH] = {};
	HRESULT documentsH = SHGetFolderPathW(NULL, CSIDL_MYDOCUMENTS, NULL, SHGFP_TYPE_CURRENT, documentsBufW);
	assert(SUCCEEDED(documentsH));
	std::string documentsDir = string::UTF16toUTF8(documentsBufW);

	// Rack <2.5.0 used "My Documents/Rack2"
	oldUserDir = system::join(documentsDir, "Rack" + APP_VERSION_MAJOR);
#endif

#if defined ARCH_MAC
	// Usually ~/Library/Application Support/Rack2
	userDir = system::join(getApplicationSupportDir(), "Rack" + APP_VERSION_MAJOR);

	// Get home directory
	struct passwd* pw = getpwuid(getuid());
	assert(pw);
	std::string homeDir = pw->pw_dir;

	// Rack <2.5.0 used ~/Documents/Rack2
	oldUserDir = system::join(homeDir, "Documents", "Rack" + APP_VERSION_MAJOR);
#endif

#if defined ARCH_LIN
	// Get home path
	const char* homeBuf = getenv("HOME");
	if (!homeBuf) {
		struct passwd* pw = getpwuid(getuid());
		assert(pw);
		homeBuf = pw->pw_dir;
	}
	std::string homeDir = homeBuf;

	// Get XDG data path
	const char* envData = getenv("XDG_DATA_HOME");
	if (envData) {
		userDir = envData;
	}
	else {
		userDir = system::join(homeDir, ".local", "share");
	}
	// Usually ~/.local/share/Rack2
	userDir = system::join(userDir, "Rack" + APP_VERSION_MAJOR);

	// Rack <2.5.0 used ~/.Rack2
	oldUserDir = system::join(homeDir, ".Rack" + APP_VERSION_MAJOR);
#endif

	// If userDir doesn't exist but oldUserDir does, attempt to move it and inform user.
	if (oldUserDir != "" && !system::isDirectory(userDir) && system::isDirectory(oldUserDir)) {
		if (system::rename(oldUserDir, userDir)) {
			std::string msg = APP_NAME + "'s user folder has been moved from";
			msg += "\n" + oldUserDir;
			msg += "\nto";
			msg += "\n" + userDir;
			osdialog_message(OSDIALOG_INFO, OSDIALOG_OK, msg.c_str());
		}
		else {
			std::string msg = "Failed to move " + APP_NAME + "'s user folder from";
			msg += "\n" + oldUserDir;
			msg += "\nto";
			msg += "\n" + userDir;
			msg += "\nConsider moving this folder manually to ensure compatibility with future versions.";
			osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, msg.c_str());
			// Move failed, just use the old dir instead
			userDir = oldUserDir;
			oldUserDir = "";
		}
	}
	else {
		oldUserDir = "";
	}

	// Create user dir if it doesn't exist
	system::createDirectory(userDir);
}


void init() {
	initSystemDir();
	initUserDir();
}


std::string system(std::string filename) {
	return system::join(systemDir, filename);
}


std::string user(std::string filename) {
	return system::join(userDir, filename);
}


std::string plugin(plugin::Plugin* plugin, std::string filename) {
	assert(plugin);
	return system::join(plugin->path, filename);
}


std::string systemDir;
std::string userDir;
std::string oldUserDir;

std::string bundlePath;


} // namespace asset
} // namespace rack
