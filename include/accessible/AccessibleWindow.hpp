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
struct LedDisplayChoice;
}
namespace ui {
struct MenuOverlay;
struct Menu;
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
		bool                  isSubmenu; // if true, Enter pushes a new menu level (no switchView first)

		ContextMenuItem(std::wstring l, std::function<void()> a, bool sub = false)
			: label(std::move(l)), action(std::move(a)), isSubmenu(sub) {}
	};

	struct DisplayCell {
		rack::app::LedDisplayChoice* choice;
		std::wstring                 label;
	};

	// One "[ Free slot ]" per rack row. item = its index in listRack; (gridX, gridY)
	// is the grid cell a new module would take there (just past the row's rightmost
	// module). Rebuilt by refreshRackView(); read when the user adds on a free slot.
	struct FreeSlotTarget {
		int item;
		int gridX;
		int gridY;
	};
	std::vector<FreeSlotTarget> freeSlotTargets;

	HWND hwnd            = nullptr;
	HWND rackHwnd        = nullptr;  // main Rack window; owns this layer, regains focus when toggled off
	HWND listRack        = nullptr;
	HWND treeLibrary     = nullptr;
	HWND searchLibrary   = nullptr;  // library search edit (Ctrl+F); filters the tree
	HWND listTags        = nullptr;  // library tag filter list ("All modules" + every tag)
	HWND listParam       = nullptr;
	HWND listOutput      = nullptr;
	HWND listInput       = nullptr;
	HWND listContextMenu = nullptr;
	HWND statusBar       = nullptr;
	HWND announcer          = nullptr;  // hidden STATIC used as live region for NVDA
	std::wstring pendingAnnouncement;   // queued by setStatus, fired from onTimer (message-pump context)
	int  announcerTicks     = 0;        // ticks until announcer text is cleared

	View                  currentView      = RACK;
	View                  previousView     = RACK;
	rack::engine::Module* currentModule    = nullptr;
	rack::engine::Module* lastParamModule  = nullptr;

	// Momentary-button release: a Space press on a momentary button (e.g. a
	// sequencer's Run) pulses the param high; this records which param so a
	// one-shot timer can drop it back to rest a moment later. -1 = none pending.
	rack::engine::Module* momentaryModule  = nullptr;
	int                   momentaryParamId = -1;
	rack::plugin::Model*  selectedModel    = nullptr;
	PendingCable          pendingCable;
	bool                  libraryLoaded    = false;

	// ── Library search / tag filter ───────────────────────────────────────────
	// The library tree is filtered by both a fuzzy search string and a single
	// selected tag. librarySelectedTag == -1 means "All modules" (no tag filter).
	std::string           librarySearch;
	int                   librarySelectedTag = -1;
	bool                  rackDirty        = true;
	std::vector<ContextMenuItem> contextItems;

	// ── Computer-keyboard MIDI mode (Shift+K) ─────────────────────────────────
	// When on, keys that map to a musical note/octave are routed to Rack's
	// "Computer keyboard" MIDI driver instead of driving the accessible UI; every
	// other key still navigates. See ChildSubclassProc.
	bool                  midiKeyboardMode = false;
	bool                  swallowNextChar  = false;  // eat the WM_CHAR that trails a routed note
	std::vector<int>      heldMidiKeys;              // GLFW key codes of notes currently held down

	// ── Display cell navigation (D key) ───────────────────────────────────────
	std::vector<DisplayCell>                  displayCells;
	std::vector<std::vector<ContextMenuItem>> menuStack;       // stack of menu levels
	rack::ui::MenuOverlay*                    capturedOverlay = nullptr;
	rack::ui::Menu*                          ownedRootMenu   = nullptr;  // detached menu holding a module's appendContextMenu() items
	std::vector<rack::ui::Menu*>              ownedSubmenus;   // submenus we created; deleted on pop/cleanup
	rack::app::LedDisplayChoice*              learningCell    = nullptr;  // active Tier B learn target
	std::wstring                              learningLastText;
	int                                       lastDisplayCellRow = 0;    // row that opened the current Tier-A submenu

	// ── Native Win32 menu bar ──────────────────────────────────────────────────
	// A real HMENU attached with SetMenu(). Windows handles Alt to enter the bar,
	// left/right arrows between menus, up/down within a menu, Enter to activate and
	// Esc to leave — and native menus have first-class MSAA/UIA support, so NVDA
	// reads them (item name, role, checkbox state) with no custom code. Selections
	// arrive as WM_COMMAND; check states are refreshed on WM_INITMENUPOPUP.
	struct MenuCmd {
		std::function<void()> action;
		std::function<bool()> checked;   // optional: drives the item's checkmark
	};
	std::vector<MenuCmd> menuCmds;       // indexed by (command id - MENU_CMD_BASE)
	HMENU menuBar      = nullptr;
	HMENU popupRecent  = nullptr;        // rebuilt on open (File → Open Recent)
	HMENU popupLibrary = nullptr;        // rebuilt on open (login state varies)

	// Singleton instance so the main run loop can drain queued commands at a
	// safe point. There is only ever one accessible window.
	static AccessibleWindow* instance;

	// owner: the main Rack window's HWND. The accessible interface becomes an
	// owned tool window (no Alt+Tab / taskbar entry) shown as a toggleable layer
	// over Rack, rather than a standalone window.
	static AccessibleWindow* create(HWND owner);
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

	// HWND of the control backing the currently active View (the one to focus).
	HWND activeControl();

	// Queue a command to run from the main loop (see drainCommands()).
	void pushCommand(std::function<void()> fn);
	std::vector<std::function<void()>> commandQueue;

	void refreshCurrentView();
	// focusModule: after rebuilding, put focus on that module's row instead of
	// restoring the previously-focused row (used right after inserting a module).
	// focusRowFallback: if the previously-focused item is gone (e.g. just deleted),
	// focus this row index (clamped) so focus lands on the neighbouring slot rather
	// than jumping back to the first module.
	void refreshRackView(rack::app::ModuleWidget* focusModule = nullptr, int focusRowFallback = -1);
	void refreshLibraryView();
	// Rebuild the library tree applying the current search string and tag filter.
	// refreshLibraryView() is a thin wrapper around this.
	void rebuildLibraryTree();
	// Populate the tag filter list once ("All modules" + every Rack tag).
	void buildTagList();
	// Move focus to one of the three library panes (tree / search / tags),
	// selecting the tree's first row when landing on an empty tree.
	void focusLibraryPane(HWND pane);
	void repopulateParamView();   // full rebuild (on module switch)
	void refreshParamValues();    // update value column only (on timer)
	void refreshPortView(bool isOutput);

	// Toggle computer-keyboard MIDI mode (Shift+K); announces the new state and
	// releases any still-held notes when turning off.
	void toggleMidiKeyboard();
	// Win32 VK -> GLFW key code, but ONLY for keys the keyboard MIDI driver maps
	// to a note/octave (both QWERTY and Numpad layouts). Returns 0 for anything
	// else, so non-playable keys fall through to normal navigation.
	static int midiKeyForVk(WPARAM vk);

	void handleRackKey(WPARAM vk);
	// Clipboard / duplicate shortcuts in the RACK list (Ctrl+C/V/D, Ctrl+Shift+D).
	// shift selects the "with cables" variant of duplicate.
	void handleRackCtrlKey(WPARAM vk, bool shift);
	// Toggle the focused module in/out of the rack's multi-selection (Space in RACK).
	// Selection lives in RackWidget::selectedModules and is shown as a label marker.
	void toggleRackSelection();
	void handleParamKey(WPARAM vk);
	// True if the param's on-screen widget is a momentary app::Switch (sets max on
	// press, min on release) rather than a latching multi-value switch.
	bool isMomentaryParam(int paramId);
	// Fired by the one-shot momentary timer: drops the pulsed param back to rest.
	void onMomentaryRelease();
	void handlePortEnter(bool isOutput);
	void handlePortDelete(bool isOutput);
	void focusPortRow(bool isOutput, int portId);
	void handleLibraryEnter();
	void handleContextMenuKey();
	void handleModuleSpecificContextMenuKey();

	void showContextMenu(std::vector<ContextMenuItem> items);
	void buildModuleContextMenu(rack::app::ModuleWidget* mw);
	void buildModuleSpecificContextMenu(rack::app::ModuleWidget* mw);
	void buildParamContextMenu(int paramId);

	void collectDisplayCells(rack::app::ModuleWidget* mw);
	std::vector<ContextMenuItem> buildItemsFromMenu(rack::ui::Menu* menu);
	void openDisplayCell(DisplayCell cell);
	void cleanupCapturedMenu();
	void handleDisplayKey();

	// ── Menu bar helpers ───────────────────────────────────────────────────────
	void buildMenuBar();
	// Append a command item to popup h; returns its command id so the caller can
	// later toggle its checkmark. `checked` (optional) is queried on popup open.
	UINT addMenuCmd(HMENU h, const std::wstring& label, std::function<void()> action,
	                std::function<bool()> checked = nullptr, UINT flags = 0);
	void refreshPopupChecks(HMENU popup);
	void rebuildRecentPopup();
	void rebuildLibraryPopup();
	// Restore a clean RACK view after an op that may have changed the module set
	// (patch load, undo/redo, paste); clears now-dangling module pointers.
	void reloadRackAfterMutation();

	// Place a new module at grid cell (gridX, gridY) — the target carried by the
	// free slot the user activated, so modules land on the focused row.
	void placeModule(rack::plugin::Model* model, int gridX, int gridY);
	// Insert a new module from the JSON currently on the system clipboard, at the
	// given grid cell (the focused free slot's target).
	void pasteModuleFromClipboard(int gridX, int gridY);
	// Ctrl+Enter on a focused module: move it to a brand-new row below the lowest
	// existing one (the keyboard equivalent of dragging a module down in the GUI).
	void moveFocusedModuleToNewRow();
	// Look up the (gridX, gridY) target of the free-slot list item at `item`.
	// Returns false (and 0,0) if that item isn't a known free slot.
	bool freeSlotTargetForItem(int item, int& gridX, int& gridY);
};


} // namespace accessible
} // namespace rack

#endif // ARCH_WIN
