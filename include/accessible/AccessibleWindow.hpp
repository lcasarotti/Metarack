#pragma once
#include <arch.hpp>

#if defined ARCH_WIN

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <functional>

// Forward-declare Rack types to avoid pulling heavy headers into this header.
namespace rack {
namespace engine {
struct Module;
}
namespace plugin  {
struct Model;
}
namespace app {
struct ModuleWidget;
}
}

namespace rack {
namespace accessible {


struct AccessibleWindow {

	enum View { RACK, LIBRARY, PARAM, OUTPUT, INPUT, CONTEXT_MENU };

	struct PendingCable {
		int  type   = 0;       // engine::Port::OUTPUT=1 / INPUT=0
		rack::engine::Module* module = nullptr;
		int  portId = -1;
		bool active = false;
	};

	struct ContextMenuItem {
		std::wstring          label;
		std::function<void()> action;
	};

	HWND hwnd            = nullptr;
	HWND listRack        = nullptr;
	HWND treeLibrary     = nullptr;
	HWND listParam       = nullptr;
	HWND listOutput      = nullptr;
	HWND listInput       = nullptr;
	HWND listContextMenu = nullptr;
	HWND statusBar       = nullptr;

	View                  currentView      = RACK;
	View                  previousView     = RACK;
	rack::engine::Module* currentModule    = nullptr;
	rack::engine::Module* lastParamModule  = nullptr;
	rack::plugin::Model*  selectedModel    = nullptr;
	PendingCable          pendingCable;
	bool                  libraryLoaded    = false;
	bool                  rackDirty        = true;
	std::vector<ContextMenuItem> contextItems;

	// Singleton instance so the main run loop can drain queued commands at a
	// safe point. There is only ever one accessible window.
	static AccessibleWindow* instance;

	static AccessibleWindow* create();
	~AccessibleWindow();

	void switchView(View v);
	void setStatus(const std::string& msg);

	// Run any commands queued from the Win32 message handlers. MUST be called
	// from the main run loop right after glfwPollEvents() — see Window::step().
	// Mutations of the widget tree (add/remove module, add cable) are queued
	// instead of run inline because the accessibility window's messages can be
	// dispatched from the idle message pump, a reentrancy point where tearing
	// down widgets and OpenGL framebuffers crashes.
	void drainCommands();

private:
	static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
	static LRESULT CALLBACK ChildSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

	void onCreate();
	void onSize();
	void onTimer();

	// Queue a command to run from the main loop (see drainCommands()).
	void pushCommand(std::function<void()> fn);
	std::vector<std::function<void()>> commandQueue;

	void refreshCurrentView();
	// focusModule: after rebuilding, put focus on that module's row instead of
	// restoring the previously-focused row (used right after inserting a module).
	void refreshRackView(rack::app::ModuleWidget* focusModule = nullptr);
	void refreshLibraryView();
	void repopulateParamView();   // full rebuild (on module switch)
	void refreshParamValues();    // update value column only (on timer)
	void refreshPortView(bool isOutput);

	void handleRackKey(WPARAM vk);
	// Clipboard / duplicate shortcuts in the RACK list (Ctrl+C/V/D, Ctrl+Shift+D).
	// shift selects the "with cables" variant of duplicate.
	void handleRackCtrlKey(WPARAM vk, bool shift);
	void handleParamKey(WPARAM vk);
	void handlePortEnter(bool isOutput);
	void handlePortDelete(bool isOutput);
	void focusPortRow(bool isOutput, int portId);
	void handleLibraryEnter();
	void handleContextMenuKey();

	void showContextMenu(std::vector<ContextMenuItem> items);
	void buildModuleContextMenu(rack::app::ModuleWidget* mw);
	void buildParamContextMenu(int paramId);

	void placeModule(rack::plugin::Model* model);
	// Insert a new module from the JSON currently on the system clipboard,
	// placed at the end of the row (mirrors Ctrl+V over empty rack space).
	void pasteModuleFromClipboard();
};


} // namespace accessible
} // namespace rack

#endif // ARCH_WIN
