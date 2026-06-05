#include <accessible/AccessibleWindow.hpp>

#if defined ARCH_WIN

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include <common.hpp>
#include <context.hpp>
#include <app/Scene.hpp>
#include <app/RackWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <app/CableWidget.hpp>
#include <app/PortWidget.hpp>
#include <app/common.hpp>
#include <plugin.hpp>
#include <plugin/Plugin.hpp>
#include <plugin/Model.hpp>
#include <engine/Engine.hpp>
#include <engine/Module.hpp>
#include <engine/Cable.hpp>
#include <engine/Port.hpp>
#include <engine/ParamQuantity.hpp>
#include <engine/PortInfo.hpp>
#include <history.hpp>
#include <math.hpp>
#include <system.hpp>
#include <patch.hpp>
#include <settings.hpp>
#include <library.hpp>
#include <asset.hpp>
#include <string.hpp>
#include <window/Window.hpp>
#include <app/RackScrollWidget.hpp>
#include <app/TipWindow.hpp>
#include <ui/common.hpp>
#include <app/LedDisplay.hpp>
#include <ui/MenuItem.hpp>
#include <ui/MenuOverlay.hpp>
#include <widget/event.hpp>

#include <algorithm>
#include <string>
#include <vector>
#include <thread>
#include <cmath>

using namespace rack;

// ── UIA notification (lazy-loaded, Win10+) ───────────────────────────────────
// UiaRaiseNotificationEvent embeds the display text directly in the event so
// the AT never needs a WM_GETTEXT callback. NotificationProcessing value 1
// (ImportantMostRecent) tells NVDA to queue the announcement even while it is
// already speaking, instead of dropping it as it does with plain WinEvents.
// Loaded at runtime so the binary runs fine on pre-Win10 without uiautomation.h
// or linking against UIAutomationCore.lib.
typedef HRESULT(WINAPI *pfnUiaHostProviderFromHwnd_t)(HWND, IUnknown**);
typedef HRESULT(WINAPI *pfnUiaRaiseNotificationEvent_t)(IUnknown*, int, int, BSTR, BSTR);
typedef BSTR(WINAPI *pfnSysAllocString_t)(const OLECHAR*);
typedef void(WINAPI *pfnSysFreeString_t)(BSTR);

static pfnUiaHostProviderFromHwnd_t   s_UiaHostProvider  = nullptr;
static pfnUiaRaiseNotificationEvent_t s_UiaRaiseNotify   = nullptr;
static pfnSysAllocString_t            s_SysAllocString   = nullptr;
static pfnSysFreeString_t             s_SysFreeString    = nullptr;

// Suppress GCC's "cast between incompatible function types" on GetProcAddress returns.
template<typename T> static T procAddr(HMODULE h, const char* name) {
	return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(h, name)));
}

static void uiaInit() {
	static bool done = false;
	if (done)
		return;
	done = true;
	HMODULE ole = LoadLibraryW(L"oleaut32.dll");
	if (ole) {
		s_SysAllocString = procAddr<pfnSysAllocString_t>(ole, "SysAllocString");
		s_SysFreeString  = procAddr<pfnSysFreeString_t> (ole, "SysFreeString");
	}
	HMODULE uia = LoadLibraryW(L"UIAutomationCore.dll");
	if (!uia)
		return;
	s_UiaHostProvider = procAddr<pfnUiaHostProviderFromHwnd_t> (uia, "UiaHostProviderFromHwnd");
	s_UiaRaiseNotify  = procAddr<pfnUiaRaiseNotificationEvent_t>(uia, "UiaRaiseNotificationEvent");
}

namespace rack {
namespace accessible {

// ── Constants ────────────────────────────────────────────────────────────────

static const UINT_PTR TIMER_ID = 1;
static const UINT     TIMER_MS = 200;
static const wchar_t* WND_CLASS = L"RackAccessibleWnd";

static const int ID_RACK         = 101;
static const int ID_LIBRARY      = 102;
static const int ID_PARAM        = 103;
static const int ID_OUTPUT       = 104;
static const int ID_INPUT        = 105;
static const int ID_CONTEXT_MENU = 106;

// Menu-bar command ids start here so they never collide with the control ids
// above (101–106) or the status bar (999).
static const UINT MENU_CMD_BASE = 2000;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::wstring toWide(const std::string& s) {
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
	return w;
}

static std::string toUtf8(const std::wstring& w) {
	if (w.empty())
		return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string s(n - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
	return s;
}

static void lvAddColumn(HWND lv, int col, const wchar_t* label, int width) {
	LVCOLUMNW c = {};
	c.mask    = LVCF_TEXT | LVCF_WIDTH;
	c.cx      = width;
	c.pszText = const_cast<wchar_t*>(label);
	ListView_InsertColumn(lv, col, &c);
}

// Insert a new row with text in column 0; lParam is caller data.
static int lvAppendRow(HWND lv, const std::wstring& col0text, LPARAM lp) {
	LVITEMW item = {};
	item.mask    = LVIF_TEXT | LVIF_PARAM;
	item.iItem   = ListView_GetItemCount(lv);
	item.lParam  = lp;
	item.pszText = const_cast<wchar_t*>(col0text.c_str());
	return ListView_InsertItem(lv, &item);
}

static void lvSetSubtext(HWND lv, int row, int col, const std::wstring& text) {
	LVITEMW item = {};
	item.mask     = LVIF_TEXT;
	item.iItem    = row;
	item.iSubItem = col;
	item.pszText  = const_cast<wchar_t*>(text.c_str());
	ListView_SetItem(lv, &item);
}

static LPARAM lvGetParam(HWND lv, int row) {
	LVITEMW item = {};
	item.mask  = LVIF_PARAM;
	item.iItem = row;
	ListView_GetItem(lv, &item);
	return item.lParam;
}

static int lvFocused(HWND lv) {
	return ListView_GetNextItem(lv, -1, LVNI_FOCUSED);
}

// Focus + select a row and make a screen reader (re)announce it. Re-firing
// EVENT_OBJECT_FOCUS forces NVDA to read the whole row again, including the
// value column — used both when entering a list and after editing a value.
static void lvFocusRow(HWND lv, int row, bool announce = true) {
	if (row < 0 || row >= ListView_GetItemCount(lv))
		return;
	ListView_SetItemState(lv, row, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
	ListView_EnsureVisible(lv, row, FALSE);
	if (announce)
		NotifyWinEvent(EVENT_OBJECT_FOCUS, lv, OBJID_CLIENT, row + 1);
}

// Position for a newly inserted module: just past the rightmost existing module,
// so inserts land in a tidy left-to-right row. Shared by placeModule() and
// pasteModuleFromClipboard().
static math::Vec nextModulePos() {
	auto existing = APP->scene->rack->getModules();
	math::Vec pos = app::RACK_OFFSET;
	for (app::ModuleWidget* e : existing) {
		float right = e->box.pos.x + e->box.size.x;
		if (right > pos.x)
			pos.x = right + app::RACK_GRID_WIDTH;
	}
	pos.y = app::RACK_OFFSET.y;
	return pos;
}

// Recursively walks a widget subtree to collect all LedDisplayChoice instances.
// Does not recurse INTO a LedDisplayChoice (its own children are rendering details).
static void collectDisplayCellsRec(widget::Widget* w,
                                   std::vector<AccessibleWindow::DisplayCell>& out) {
	if (auto* dc = dynamic_cast<app::LedDisplayChoice*>(w)) {
		dc->step();
		out.push_back({dc, toWide(dc->text.empty() ? "(display)" : dc->text)});
		return;
	}
	for (widget::Widget* child : w->children)
		collectDisplayCellsRec(child, out);
}

// Read the system clipboard as UTF-8. We talk to the Win32 clipboard directly
// (rather than glfwGetClipboardString) to avoid pulling GLFW into this Win32
// translation unit; it's the same underlying clipboard GLFW uses on Windows, so
// it interoperates with the standard GUI's Ctrl+C/Ctrl+V.
static std::string getClipboardTextUtf8() {
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
		return {};
	if (!OpenClipboard(nullptr))
		return {};
	std::string out;
	if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
		if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
			int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
			if (n > 1) {
				out.resize(n - 1);
				WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
			}
			GlobalUnlock(h);
		}
	}
	CloseClipboard();
	return out;
}

// ── create / destroy ─────────────────────────────────────────────────────────

AccessibleWindow* AccessibleWindow::instance = nullptr;

AccessibleWindow* AccessibleWindow::create() {
	HINSTANCE hInst = GetModuleHandleW(nullptr);

	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES;
	InitCommonControlsEx(&icc);

	WNDCLASSEXW wc  = {};
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = hInst;
	wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = WND_CLASS;
	RegisterClassExW(&wc);

	AccessibleWindow* self = new AccessibleWindow;
	instance = self;

	HWND hwnd = CreateWindowExW(
	              0,
	              WND_CLASS,
	              L"VCV Rack — Interfaccia accessibile",
	              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
	              CW_USEDEFAULT, CW_USEDEFAULT, 580, 720,
	              nullptr, nullptr, hInst, self);

	if (!hwnd) {
		instance = nullptr;
		delete self;
		return nullptr;
	}
	return self;
}

AccessibleWindow::~AccessibleWindow() {
	if (hwnd) {
		KillTimer(hwnd, TIMER_ID);
		DestroyWindow(hwnd);
		hwnd = nullptr;
	}
	if (instance == this)
		instance = nullptr;
}

// ── Deferred command queue ─────────────────────────────────────────────────────

void AccessibleWindow::pushCommand(std::function<void()> fn) {
	commandQueue.push_back(std::move(fn));
}

void AccessibleWindow::drainCommands() {
	// Swap out the queue first: a command may push further commands, and we
	// don't want to run those until the next drain (nor invalidate iterators).
	std::vector<std::function<void()>> cmds;
	cmds.swap(commandQueue);
	for (auto& fn : cmds)
		fn();
}

// ── WndProc ───────────────────────────────────────────────────────────────────

LRESULT CALLBACK AccessibleWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	AccessibleWindow* self = (AccessibleWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

	switch (msg) {
		case WM_CREATE: {
			auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
			self = static_cast<AccessibleWindow*>(cs->lpCreateParams);
			self->hwnd = hwnd;
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
			self->onCreate();
			return 0;
		}
		case WM_ACTIVATE:
			// Restore keyboard focus to the active control whenever the window
			// is brought to front (e.g. via Alt+Tab). Without this, the window
			// frame becomes active but no child control has focus, so NVDA and
			// keyboard input both fail.
			if (self && LOWORD(wp) != WA_INACTIVE) {
				HWND views[] = { self->listRack, self->treeLibrary, self->listParam,
				                 self->listOutput, self->listInput, self->listContextMenu
				               };
				SetFocus(views[(int)self->currentView]);
			}
			return 0;
		case WM_SIZE:
			if (self)
				self->onSize();
			return 0;
		case WM_TIMER:
			if (self && wp == TIMER_ID)
				self->onTimer();
			return 0;
		case WM_HOTKEY:
			// Ctrl+Shift+A: bring accessibility window to front from any context
			if (self && wp == 1) {
				ShowWindow(hwnd, SW_RESTORE);
				SetForegroundWindow(hwnd);
				HWND views[] = { self->listRack, self->treeLibrary, self->listParam,
				                 self->listOutput, self->listInput, self->listContextMenu
				               };
				SetFocus(views[(int)self->currentView]);
			}
			return 0;
		case WM_COMMAND:
			// Menu-bar selection. The lambda decides whether to run inline or defer
			// itself via pushCommand() (tree/engine/window mutations must defer).
			if (self && HIWORD(wp) == 0) {
				UINT id = LOWORD(wp);
				if (id >= MENU_CMD_BASE && id < MENU_CMD_BASE + self->menuCmds.size()) {
					auto& action = self->menuCmds[id - MENU_CMD_BASE].action;
					if (action)
						action();
					return 0;
				}
			}
			return DefWindowProcW(hwnd, msg, wp, lp);
		case WM_INITMENUPOPUP:
			// Fired once per popup just before it opens. Rebuild the popups whose
			// structure varies, then refresh every item's checkmark from settings.
			if (self) {
				HMENU popup = (HMENU)wp;
				if (popup == self->popupRecent)
					self->rebuildRecentPopup();
				else if (popup == self->popupLibrary)
					self->rebuildLibraryPopup();
				self->refreshPopupChecks(popup);
			}
			return 0;
		case WM_CLOSE:
			ShowWindow(hwnd, SW_MINIMIZE);
			return 0;
		case WM_DESTROY:
			UnregisterHotKey(hwnd, 1);
			KillTimer(hwnd, TIMER_ID);
			return 0;
		case WM_NOTIFY:
			return DefWindowProcW(hwnd, msg, wp, lp);
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── onCreate ─────────────────────────────────────────────────────────────────

void AccessibleWindow::onCreate() {
	HINSTANCE hInst = GetModuleHandleW(nullptr);

	// Attach the native menu bar first: SetMenu shrinks the client area, so the
	// GetClientRect below already excludes the bar and the controls size correctly.
	buildMenuBar();
	SetMenu(hwnd, menuBar);

	RECT rc;
	GetClientRect(hwnd, &rc);
	int w = rc.right;
	int h = rc.bottom;

	// Status bar (auto-sizes itself)
	statusBar = CreateWindowExW(0, STATUSCLASSNAME, L"Pronto.",
	                            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
	                            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)999, hInst, nullptr);

	// Off-screen 1×1 STATIC that acts as a live region: NVDA reliably fires on
	// EVENT_OBJECT_NAMECHANGE for STATIC controls, so setStatus() updates this
	// alongside the visible status bar to guarantee screen-reader announcement.
	announcer = CreateWindowExW(0, L"STATIC", L"",
	                            WS_CHILD | WS_VISIBLE,
	                            -2, -2, 1, 1, hwnd, nullptr, hInst, nullptr);
	RECT sbRc;
	SendMessageW(statusBar, WM_SIZE, 0, 0);
	GetWindowRect(statusBar, &sbRc);
	int sbH  = sbRc.bottom - sbRc.top;
	int listH = h - sbH;

	DWORD lvStyle = WS_CHILD | WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS;
	DWORD lvEx    = LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES;

	// Rack ListView
	listRack = CreateWindowExW(0, WC_LISTVIEWW, L"",
	                           lvStyle | WS_VISIBLE,
	                           0, 0, w, listH,
	                           hwnd, (HMENU)(INT_PTR)ID_RACK, hInst, nullptr);
	ListView_SetExtendedListViewStyle(listRack, lvEx);
	lvAddColumn(listRack, 0, L"Modulo", 230);
	lvAddColumn(listRack, 1, L"Manufacturer", 180);
	lvAddColumn(listRack, 2, L"HP", 60);

	// Library TreeView
	treeLibrary = CreateWindowExW(0, WC_TREEVIEWW, L"",
	                              WS_CHILD | WS_BORDER | WS_TABSTOP |
	                              TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
	                              0, 0, w, listH,
	                              hwnd, (HMENU)(INT_PTR)ID_LIBRARY, hInst, nullptr);

	// Param ListView
	listParam = CreateWindowExW(0, WC_LISTVIEWW, L"",
	                            lvStyle,
	                            0, 0, w, listH,
	                            hwnd, (HMENU)(INT_PTR)ID_PARAM, hInst, nullptr);
	ListView_SetExtendedListViewStyle(listParam, lvEx);
	lvAddColumn(listParam, 0, L"Parametro", 250);
	lvAddColumn(listParam, 1, L"Valore", 210);

	// Output ListView
	listOutput = CreateWindowExW(0, WC_LISTVIEWW, L"",
	                             lvStyle,
	                             0, 0, w, listH,
	                             hwnd, (HMENU)(INT_PTR)ID_OUTPUT, hInst, nullptr);
	ListView_SetExtendedListViewStyle(listOutput, lvEx);
	lvAddColumn(listOutput, 0, L"Output", 250);
	lvAddColumn(listOutput, 1, L"Stato", 210);

	// Input ListView
	listInput = CreateWindowExW(0, WC_LISTVIEWW, L"",
	                            lvStyle,
	                            0, 0, w, listH,
	                            hwnd, (HMENU)(INT_PTR)ID_INPUT, hInst, nullptr);
	ListView_SetExtendedListViewStyle(listInput, lvEx);
	lvAddColumn(listInput, 0, L"Input", 250);
	lvAddColumn(listInput, 1, L"Stato", 210);

	// Context-menu ListView (single column, initially hidden)
	listContextMenu = CreateWindowExW(0, WC_LISTVIEWW, L"",
	                                  lvStyle,
	                                  0, 0, w, listH,
	                                  hwnd, (HMENU)(INT_PTR)ID_CONTEXT_MENU, hInst, nullptr);
	ListView_SetExtendedListViewStyle(listContextMenu, lvEx);
	lvAddColumn(listContextMenu, 0, L"Azione", w - 4);

	// Subclass all controls for keyboard interception
	SetWindowSubclass(listRack,        ChildSubclassProc, 0, (DWORD_PTR)this);
	SetWindowSubclass(treeLibrary,     ChildSubclassProc, 1, (DWORD_PTR)this);
	SetWindowSubclass(listParam,       ChildSubclassProc, 2, (DWORD_PTR)this);
	SetWindowSubclass(listOutput,      ChildSubclassProc, 3, (DWORD_PTR)this);
	SetWindowSubclass(listInput,       ChildSubclassProc, 4, (DWORD_PTR)this);
	SetWindowSubclass(listContextMenu, ChildSubclassProc, 5, (DWORD_PTR)this);

	SetTimer(hwnd, TIMER_ID, TIMER_MS, nullptr);

	// Ctrl+Shift+A: global hotkey to bring this window to front from anywhere
	RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_SHIFT, 'A');

	// Initial population and focus
	refreshRackView();
	rackDirty = false;
	SetForegroundWindow(hwnd);
	SetFocus(listRack);
}

// ── Layout ───────────────────────────────────────────────────────────────────

void AccessibleWindow::onSize() {
	// SetMenu() in onCreate can trigger WM_SIZE before the controls exist.
	if (!statusBar || !listRack)
		return;
	RECT rc;
	GetClientRect(hwnd, &rc);
	int w = rc.right;
	int h = rc.bottom;

	SendMessageW(statusBar, WM_SIZE, 0, 0);
	RECT sbRc;
	GetWindowRect(statusBar, &sbRc);
	int sbH   = sbRc.bottom - sbRc.top;
	int listH = h - sbH;

	HWND ctrls[] = { listRack, treeLibrary, listParam, listOutput, listInput, listContextMenu };
	for (HWND c : ctrls)
		SetWindowPos(c, nullptr, 0, 0, w, listH, SWP_NOZORDER | SWP_NOMOVE);
}

// ── Status bar ───────────────────────────────────────────────────────────────

void AccessibleWindow::setStatus(const std::string& msg) {
	std::wstring w = toWide(msg);
	SetWindowTextW(statusBar, w.c_str());
	// Queue announcement for onTimer: NotifyWinEvent must fire from the message-pump
	// thread context so NVDA can service its cross-process WM_GETTEXT response
	// synchronously. Firing from drainCommands (main loop, outside the pump) leaves
	// NVDA's SendMessage unanswered and the text unread.
	pendingAnnouncement = std::move(w);
}

// ── View switching ───────────────────────────────────────────────────────────

void AccessibleWindow::switchView(View v) {
	HWND ctrls[] = { listRack, treeLibrary, listParam, listOutput, listInput, listContextMenu };
	for (int i = 0; i < 6; i++)
		ShowWindow(ctrls[i], (i == (int)v) ? SW_SHOW : SW_HIDE);
	currentView = v;

	switch (v) {
		case RACK:
			// Rebuild only when content has changed; avoids flooding NVDA with
			// N×EVENT_OBJECT_CREATE on every R keypress.
			if (rackDirty) {
				refreshRackView();
				rackDirty = false;
			}
			break;
		case LIBRARY:
			if (!libraryLoaded) {
				refreshLibraryView();
				libraryLoaded = true;
			}
			break;
		case PARAM:
			// Rebuild only when the target module changed.
			if (currentModule != lastParamModule) {
				repopulateParamView();
				lastParamModule = currentModule;
			}
			break;
		case OUTPUT:
			refreshPortView(true);
			break;
		case INPUT:
			refreshPortView(false);
			break;
		case CONTEXT_MENU:
			// Items already populated by showContextMenu() before this call.
			break;
	}

	// In the item lists, land focus on the first row so a screen-reader user
	// hears item 1 on entry and the first Down arrow moves to item 2 (the
	// expected behaviour). Don't override an existing focus on revisits.
	if (v == PARAM || v == OUTPUT || v == INPUT || v == CONTEXT_MENU) {
		HWND lv = ctrls[(int)v];
		if (lvFocused(lv) < 0)
			lvFocusRow(lv, 0, false);   // SetFocus below makes NVDA announce it
	}

	SetFocus(ctrls[(int)v]);
}

// ── Timer ────────────────────────────────────────────────────────────────────

void AccessibleWindow::onTimer() {
	if (!pendingAnnouncement.empty()) {
		uiaInit();
		bool ok = false;
		if (s_UiaHostProvider && s_UiaRaiseNotify && s_SysAllocString && s_SysFreeString) {
			IUnknown* prov = nullptr;
			if (SUCCEEDED(s_UiaHostProvider(hwnd, &prov)) && prov) {
				BSTR bText = s_SysAllocString(pendingAnnouncement.c_str());
				BSTR bAct  = s_SysAllocString(L"RackStatus");
				// NotificationKind_ActionCompleted = 2
				// NotificationProcessing_ImportantMostRecent = 1
				ok = SUCCEEDED(s_UiaRaiseNotify(prov, 2, 1, bText, bAct));
				s_SysFreeString(bText);
				s_SysFreeString(bAct);
				prov->Release();
			}
		}
		if (!ok) {
			// Fallback (pre-Win10): set text on the announcer STATIC + WinEvent.
			SetWindowTextW(announcer, pendingAnnouncement.c_str());
			NotifyWinEvent(EVENT_SYSTEM_ALERT, announcer, OBJID_CLIENT, 0);
			announcerTicks = 3;
		}
		pendingAnnouncement.clear();
	}
	if (announcerTicks > 0 && --announcerTicks == 0)
		SetWindowTextW(announcer, L"");

	// Tier B learn mode: poll the learning cell for value changes.
	if (learningCell && APP && APP->event) {
		if (APP->event->getSelectedWidget() != learningCell) {
			// Deselected externally: cancel learn mode.
			learningCell = nullptr;
			learningLastText.clear();
		}
		else {
			std::wstring newText = toWide(learningCell->text);
			if (newText != learningLastText) {
				learningLastText = newText;
				setStatus("Valore aggiornato: " + toUtf8(newText) + ".");
			}
		}
	}

	refreshCurrentView();
}

void AccessibleWindow::refreshCurrentView() {
	// Intentionally empty: updating all param rows every 100 ms fires
	// one EVENT_OBJECT_NAMECHANGE per row per tick via LVM_SETITEM,
	// which floods NVDA's event queue and causes 1-second+ speech latency.
	// Values are refreshed on row focus change (WM_NOTIFY/LVN_ITEMCHANGED)
	// and after explicit user edits (handleParamKey → lvSetSubtext).
}

// ── Context menu ─────────────────────────────────────────────────────────────

void AccessibleWindow::showContextMenu(std::vector<ContextMenuItem> items) {
	previousView = currentView;
	contextItems = std::move(items);
	ListView_DeleteAllItems(listContextMenu);
	for (int i = 0; i < (int)contextItems.size(); i++)
		lvAppendRow(listContextMenu, contextItems[i].label, (LPARAM)i);
	switchView(CONTEXT_MENU);
}

void AccessibleWindow::buildModuleContextMenu(app::ModuleWidget* mw) {
	if (!mw || !mw->module)
		return;

	engine::Module* mod     = mw->module;
	bool            bypassed = mod->isBypassed();

	std::vector<ContextMenuItem> items;

	items.push_back({L"Azzera parametri", [this, mw]() {
		pushCommand([mw]() {
			mw->resetAction();
		});
	}});

	items.push_back({L"Randomizza parametri", [this, mw]() {
		pushCommand([mw]() {
			mw->randomizeAction();
		});
	}});

	items.push_back({L"Disconnetti cavi", [this, mw]() {
		pushCommand([mw]() {
			mw->disconnectAction();
		});
	}});

	std::wstring bypassLabel = bypassed ? L"Bypass: disattiva" : L"Bypass: attiva";
	items.push_back({bypassLabel, [this, mw, bypassed]() {
		pushCommand([mw, bypassed]() {
			mw->bypassAction(!bypassed);
		});
	}});

	items.push_back({L"Duplica (senza cavi)", [this, mw]() {
		pushCommand([this, mw]() {
			std::string sname = mw->model ? mw->model->name : "?";
			mw->cloneAction(false);
			// cloneAction() inserts a new ModuleWidget into the rack, but only the
			// OpenGL view (redrawn every frame) reflects it automatically. The
			// accessible RACK list is rebuilt lazily, so refresh it here — otherwise
			// the duplicate stays invisible in the list until the next rebuild.
			refreshRackView();
			rackDirty = false;
			setStatus("Modulo \"" + sname + "\" duplicato.");
		});
	}});

	items.push_back({L"Duplica con cavi", [this, mw]() {
		pushCommand([this, mw]() {
			std::string sname = mw->model ? mw->model->name : "?";
			mw->cloneAction(true);
			// See note above: refresh the accessible RACK list so the clone appears.
			refreshRackView();
			rackDirty = false;
			setStatus("Modulo \"" + sname + "\" duplicato (con cavi).");
		});
	}});

	items.push_back({L"Elimina", [this, mw]() {
		std::string  sname = mw->model ? mw->model->name : "?";
		std::wstring name  = toWide(sname);
		if (MessageBoxW(hwnd,
		                (L"Rimuovere \"" + name + L"\"?").c_str(),
		                L"Conferma", MB_YESNO | MB_ICONQUESTION) == IDYES) {
			pushCommand([this, mw, sname]() {
				cleanupCapturedMenu();
				engine::Module* mod = mw->module;
				mw->removeAction();
				if (currentModule == mod) {
					currentModule   = nullptr;
					lastParamModule = nullptr;
				}
				refreshRackView();
				rackDirty = false;
				setStatus("Modulo \"" + sname + "\" rimosso.");
			});
		}
	}});

	showContextMenu(std::move(items));
}

// ── Input dialog (in-memory DLGTEMPLATE, no .rc file needed) ─────────────────

struct InputDlgData {
	std::wstring prompt;
	std::wstring initial;
	std::wstring result;
	bool ok = false;
};

static INT_PTR CALLBACK inputDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
		case WM_INITDIALOG: {
			auto* d = reinterpret_cast<InputDlgData*>(lp);
			SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
			SetDlgItemTextW(dlg, 1001, d->prompt.c_str());
			SetDlgItemTextW(dlg, 1002, d->initial.c_str());
			SendDlgItemMessageW(dlg, 1002, EM_SETSEL, 0, -1);
			return TRUE;
		}
		case WM_COMMAND: {
			auto* d = reinterpret_cast<InputDlgData*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
			if (LOWORD(wp) == IDOK) {
				int len = GetWindowTextLengthW(GetDlgItem(dlg, 1002));
				d->result.resize(len);
				if (len > 0)
					GetWindowTextW(GetDlgItem(dlg, 1002), &d->result[0], len + 1);
				d->ok = true;
				EndDialog(dlg, IDOK);
			}
			else if (LOWORD(wp) == IDCANCEL) {
				EndDialog(dlg, IDCANCEL);
			}
			return TRUE;
		}
	}
	return FALSE;
}

// Builds a DLGTEMPLATE in memory for a simple prompt dialog with one edit field.
// Layout (dialog units): 220×80, 4 items: static label, edit, OK, Cancel.
static std::vector<BYTE> buildInputDlgTemplate(const std::wstring& title,
    const std::wstring& prompt,
    const std::wstring& initial) {
	std::vector<BYTE> buf;
	buf.reserve(512);

	auto writeW = [&](WORD w) {
		buf.push_back((BYTE)(w & 0xFF));
		buf.push_back((BYTE)(w >> 8));
	};
	auto writeD = [&](DWORD d) {
		buf.push_back((BYTE)(d & 0xFF));
		buf.push_back((BYTE)((d >> 8) & 0xFF));
		buf.push_back((BYTE)((d >> 16) & 0xFF));
		buf.push_back((BYTE)((d >> 24) & 0xFF));
	};
	auto writeWStr = [&](const std::wstring & s) {
		for (wchar_t c : s)
			writeW((WORD)c);
		writeW(0);
	};
	auto align4 = [&]() {
		while (buf.size() % 4 != 0)
			buf.push_back(0);
	};

	// DLGTEMPLATE header
	writeD(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
	writeD(0);         // dwExtendedStyle
	writeW(4);         // cdit: static + edit + OK + Cancel
	writeW(0); writeW(0); writeW(220); writeW(80); // x,y,cx,cy
	writeW(0);         // no menu
	writeW(0);         // default window class
	writeWStr(title);
	writeW(8);         // font point size
	writeWStr(L"MS Shell Dlg");

	// Item 1: Static label
	align4();
	writeD(WS_CHILD | WS_VISIBLE | SS_LEFT);
	writeD(0);
	writeW(7); writeW(7); writeW(206); writeW(20); // x,y,cx,cy
	writeW(1001);
	writeW(0xFFFF); writeW(0x0082); // Static class atom
	writeWStr(prompt);
	writeW(0);

	// Item 2: Edit control (pre-filled with current value)
	align4();
	writeD(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL);
	writeD(0);
	writeW(7); writeW(30); writeW(206); writeW(14); // x,y,cx,cy
	writeW(1002);
	writeW(0xFFFF); writeW(0x0081); // Edit class atom
	writeWStr(initial);
	writeW(0);

	// Item 3: OK button
	align4();
	writeD(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON);
	writeD(0);
	writeW(60); writeW(52); writeW(50); writeW(14);
	writeW((WORD)IDOK);
	writeW(0xFFFF); writeW(0x0080); // Button class atom
	writeWStr(L"OK");
	writeW(0);

	// Item 4: Cancel button
	align4();
	writeD(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON);
	writeD(0);
	writeW(116); writeW(52); writeW(50); writeW(14);
	writeW((WORD)IDCANCEL);
	writeW(0xFFFF); writeW(0x0080); // Button class atom
	writeWStr(L"Annulla");
	writeW(0);

	return buf;
}

// Shows a modal input dialog; returns the entered text if OK, empty string if cancelled.
static std::wstring showInputDialog(HWND parent, const std::wstring& title,
                                    const std::wstring& prompt, const std::wstring& initial) {
	InputDlgData d;
	d.prompt  = prompt;
	d.initial = initial;

	auto tmpl = buildInputDlgTemplate(title, prompt, initial);
	HINSTANCE hInst = (HINSTANCE)GetModuleHandleW(nullptr);

	DialogBoxIndirectParamW(hInst,
	                        reinterpret_cast<LPCDLGTEMPLATEW>(tmpl.data()),
	                        parent, inputDlgProc, (LPARAM)&d);

	return d.ok ? d.result : std::wstring{};
}

// ── Parameter context menu ────────────────────────────────────────────────────

void AccessibleWindow::buildParamContextMenu(int paramId) {
	if (!currentModule)
		return;
	engine::ParamQuantity* pq = currentModule->getParamQuantity(paramId);
	if (!pq)
		return;

	std::vector<ContextMenuItem> items;

	int row = lvFocused(listParam);

	items.push_back({L"Imposta valore…", [this, pq, row]() {
		std::wstring paramName = toWide(pq->name);
		std::wstring current   = toWide(pq->getDisplayValueString());
		std::wstring prompt    = L"Valore per «" + paramName + L"»:\n"
		                         L"(Es: 440, C4, log2(8), dbtogain(-6))";

		std::wstring text = showInputDialog(hwnd, L"Imposta valore", prompt, current);
		if (text.empty())
			return;

		pq->setDisplayValueString(toUtf8(text));

		if (row >= 0) {
			std::wstring valW = toWide(pq->getDisplayValueString() + pq->getUnit());
			lvSetSubtext(listParam, row, 1, valW);
			lvFocusRow(listParam, row);
		}
	}});

	items.push_back({L"Azzera al valore predefinito", [this, pq, row]() {
		pq->reset();
		if (row >= 0) {
			std::wstring valW = toWide(pq->getDisplayValueString() + pq->getUnit());
			lvSetSubtext(listParam, row, 1, valW);
			lvFocusRow(listParam, row);
		}
	}});

	showContextMenu(std::move(items));
}

void AccessibleWindow::handleContextMenuKey() {
	switch (currentView) {
		case RACK: {
			int row = lvFocused(listRack);
			if (row < 0)
				return;
			LPARAM lp = lvGetParam(listRack, row);
			if (lp == 0)
				return;
			buildModuleContextMenu(reinterpret_cast<app::ModuleWidget*>(lp));
			break;
		}
		case PARAM: {
			int row = lvFocused(listParam);
			if (row < 0 || !currentModule)
				return;
			buildParamContextMenu((int)lvGetParam(listParam, row));
			break;
		}
		default:
			break;
	}
}

// Ctrl+Application key — Reaper-style separate context menu holding ONLY the
// module's own options (the appendContextMenu() override: e.g. MIDI-to-CV's
// Polyphony channels, Polyphony allocation Rotate/Reuse/Reset, CLK/N divider).
void AccessibleWindow::handleModuleSpecificContextMenuKey() {
	app::ModuleWidget* mw = nullptr;
	if (currentView == RACK) {
		int row = lvFocused(listRack);
		if (row < 0)
			return;
		LPARAM lp = lvGetParam(listRack, row);
		if (lp == 0)
			return;
		mw = reinterpret_cast<app::ModuleWidget*>(lp);
	}
	else if (currentModule && APP && APP->scene && APP->scene->rack) {
		// PARAM/OUTPUT/INPUT: the menu still belongs to the focused module.
		mw = APP->scene->rack->getModule(currentModule->id);
	}
	if (!mw)
		return;
	buildModuleSpecificContextMenu(mw);
}

void AccessibleWindow::buildModuleSpecificContextMenu(app::ModuleWidget* mw) {
	if (!mw || !mw->module)
		return;

	cleanupCapturedMenu();

	// Pour the module's own context-menu items into a detached ui::Menu, then read
	// them with the same walker used for display menus — it already handles nested
	// submenus (createChildMenu) and checkmarks/right-arrows. The items' lambdas
	// mutate the module directly, so no MenuOverlay in the scene is needed.
	ui::Menu* extra = new ui::Menu;
	mw->appendContextMenu(extra);
	auto items = buildItemsFromMenu(extra);

	if (items.empty()) {
		delete extra;
		setStatus("Nessuna opzione specifica per questo modulo.");
		return;
	}

	// Keep the detached menu alive while the user navigates: the item lambdas hold
	// pointers into its children, and submenus are built on demand. Freed by
	// cleanupCapturedMenu() when the menu closes.
	ownedRootMenu = extra;
	menuStack.push_back(items);    // level 0 = module-specific list
	showContextMenu(items);        // sets previousView, shows CONTEXT_MENU
}

// ── Display cell navigation (D key) ──────────────────────────────────────────

void AccessibleWindow::collectDisplayCells(app::ModuleWidget* mw) {
	displayCells.clear();
	if (!mw)
		return;
	for (widget::Widget* child : mw->children)
		collectDisplayCellsRec(child, displayCells);
}

// Build ContextMenuItems from a Rack ui::Menu. Leaf items call doAction(false)
// (fires onAction without closing the captured overlay). Submenu items push a new
// level lazily (createChildMenu is called when the user navigates into the submenu,
// not when the parent menu is built).
std::vector<AccessibleWindow::ContextMenuItem> AccessibleWindow::buildItemsFromMenu(ui::Menu* menu) {
	std::vector<ContextMenuItem> items;
	for (widget::Widget* w : menu->children) {
		auto* mi = dynamic_cast<ui::MenuItem*>(w);
		if (!mi)
			continue;

		std::wstring label = toWide(mi->text);
		if (!mi->rightText.empty()) {
			if (mi->rightText.find(CHECKMARK_STRING) != std::string::npos)
				label += L" ✓";
			else if (mi->rightText.find(RIGHT_ARROW) != std::string::npos)
				label += L" ▸";
			else
				label += L"  " + toWide(mi->rightText);
		}

		if (mi->disabled) {
			label += L" (non disponibile)";
			items.push_back({label, []() {}, false});
			continue;
		}

		// Probe for submenu: createChildMenu() returns non-null for submenu items.
		// We delete the probe immediately; a fresh one will be created on demand.
		ui::Menu* probe = mi->createChildMenu();
		bool hasSub = (probe != nullptr);
		delete probe;

		if (hasSub) {
			items.push_back({label, [this, mi]() {
				ui::Menu* sub = mi->createChildMenu();
				if (!sub)
					return;
				ownedSubmenus.push_back(sub);
				auto subItems = buildItemsFromMenu(sub);
				menuStack.push_back(subItems);
				contextItems = subItems;
				ListView_DeleteAllItems(listContextMenu);
				for (int i = 0; i < (int)contextItems.size(); i++)
					lvAppendRow(listContextMenu, contextItems[i].label, (LPARAM)i);
				lvFocusRow(listContextMenu, 0);
			}, true});
		}
		else {
			items.push_back({label, [this, mi]() {
				mi->doAction(false);
				cleanupCapturedMenu();
			}, false});
		}
	}
	return items;
}

// Fire the display cell's click, then fork: if a MenuOverlay appeared (Tier A)
// capture it and show its items in the accessible CONTEXT_MENU. Otherwise
// (Tier B) enter learn/select mode on the widget directly.
void AccessibleWindow::openDisplayCell(DisplayCell cell) {
	if (!APP || !APP->scene || !cell.choice)
		return;

	widget::Widget* lastBefore = APP->scene->children.empty() ? nullptr : APP->scene->children.back();
	widget::Widget::ActionEvent eAction;
	cell.choice->onAction(eAction);
	widget::Widget* lastAfter = APP->scene->children.empty() ? nullptr : APP->scene->children.back();

	if (lastAfter && lastAfter != lastBefore) {
		auto* overlay = dynamic_cast<ui::MenuOverlay*>(lastAfter);
		if (overlay) {
			capturedOverlay = overlay;
			ui::Menu* menu = nullptr;
			for (widget::Widget* child : overlay->children) {
				menu = dynamic_cast<ui::Menu*>(child);
				if (menu)
					break;
			}
			if (menu) {
				auto items = buildItemsFromMenu(menu);
				menuStack.push_back(items);
				contextItems = items;
				ListView_DeleteAllItems(listContextMenu);
				for (int i = 0; i < (int)contextItems.size(); i++)
					lvAppendRow(listContextMenu, contextItems[i].label, (LPARAM)i);
				switchView(CONTEXT_MENU);
				lvFocusRow(listContextMenu, 0, true);
			}
			else {
				APP->scene->removeChild(overlay);
				delete overlay;
				capturedOverlay = nullptr;
				setStatus("Errore: struttura del menu non riconosciuta.");
			}
			return;
		}
	}

	// Tier B: no overlay appeared — enter learn/select mode.
	if (!APP->event)
		return;
	APP->event->setSelectedWidget(cell.choice);
	learningCell = cell.choice;
	learningLastText = toWide(cell.choice->text);
	setStatus("In apprendimento — premi il controllo MIDI. Spazio = toggle. Esc = annulla.");
}

// Remove the captured overlay from the scene and free all display-navigation state.
void AccessibleWindow::cleanupCapturedMenu() {
	for (ui::Menu* sub : ownedSubmenus)
		delete sub;
	ownedSubmenus.clear();

	if (capturedOverlay) {
		if (APP && APP->scene)
			APP->scene->removeChild(capturedOverlay);
		delete capturedOverlay;
		capturedOverlay = nullptr;
	}

	if (ownedRootMenu) {
		delete ownedRootMenu;
		ownedRootMenu = nullptr;
	}

	menuStack.clear();
	displayCells.clear();
	learningCell = nullptr;
	learningLastText.clear();
}

// Open the display-cell list for the current module (D key from RACK or PARAM).
void AccessibleWindow::handleDisplayKey() {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;

	// In the rack list, act on the module the screen reader is focused on, just
	// like P/O/I do (handleRackKey). Without this we'd use a stale currentModule
	// left over from an earlier interaction, so D would only work after the user
	// had already opened that module's param/port view.
	if (currentView == RACK) {
		int row = lvFocused(listRack);
		if (row < 0)
			return;
		LPARAM lp = lvGetParam(listRack, row);
		if (lp == 0)
			return;
		currentModule = reinterpret_cast<app::ModuleWidget*>(lp)->module;
	}

	if (!currentModule)
		return;
	app::ModuleWidget* mw = APP->scene->rack->getModule(currentModule->id);
	if (!mw)
		return;

	cleanupCapturedMenu();
	collectDisplayCells(mw);

	if (displayCells.empty()) {
		setStatus("Nessun display cliccabile per questo modulo.");
		return;
	}

	std::vector<ContextMenuItem> items;
	for (auto& cell : displayCells) {
		app::LedDisplayChoice* choice = cell.choice;
		std::wstring label = cell.label;
		items.push_back({label, [this, choice, label]() {
			openDisplayCell({choice, label});
		}, false});
	}

	menuStack.push_back(items);   // level 0 = display cell list
	showContextMenu(items);        // sets previousView, shows CONTEXT_MENU
}

// ── Menu bar ─────────────────────────────────────────────────────────────────

UINT AccessibleWindow::addMenuCmd(HMENU h, const std::wstring& label,
                                  std::function<void()> action,
                                  std::function<bool()> checked, UINT flags) {
	UINT id = MENU_CMD_BASE + (UINT)menuCmds.size();
	menuCmds.push_back({std::move(action), std::move(checked)});
	AppendMenuW(h, MF_STRING | flags, id, label.c_str());
	return id;
}

// Set the checkmark of every command item in this popup from its `checked` getter.
// Called on WM_INITMENUPOPUP, so toggles and radio presets always show live state.
void AccessibleWindow::refreshPopupChecks(HMENU popup) {
	int n = GetMenuItemCount(popup);
	for (int i = 0; i < n; i++) {
		UINT id = GetMenuItemID(popup, i);
		// (UINT)-1 = submenu/separator; ids outside our range belong to nothing.
		if (id < MENU_CMD_BASE || id >= MENU_CMD_BASE + menuCmds.size())
			continue;
		auto& chk = menuCmds[id - MENU_CMD_BASE].checked;
		if (chk)
			CheckMenuItem(popup, id, MF_BYCOMMAND | (chk() ? MF_CHECKED : MF_UNCHECKED));
	}
}

void AccessibleWindow::reloadRackAfterMutation() {
	// A patch load / undo / paste may have destroyed the modules these point at.
	cleanupCapturedMenu();
	currentModule   = nullptr;
	lastParamModule = nullptr;
	pendingCable    = PendingCable();
	rackDirty       = true;
	switchView(RACK);   // refreshes (rackDirty) and lands focus on the RACK list
}

void AccessibleWindow::rebuildRecentPopup() {
	while (DeleteMenu(popupRecent, 0, MF_BYPOSITION)) {}
	if (settings::recentPatchPaths.empty()) {
		AppendMenuW(popupRecent, MF_STRING | MF_GRAYED, 0, L"(nessuna patch recente)");
		return;
	}
	for (const std::string& path : settings::recentPatchPaths) {
		std::string p = path;
		addMenuCmd(popupRecent, toWide(system::getStem(path)), [this, p]() {
			pushCommand([this, p]() {
				APP->patch->loadPathDialog(p);
				reloadRackAfterMutation();
			});
		});
	}
}

void AccessibleWindow::rebuildLibraryPopup() {
	while (DeleteMenu(popupLibrary, 0, MF_BYPOSITION)) {}
	if (!library::isLoggedIn()) {
		addMenuCmd(popupLibrary, L"Registrati…", []() {
			system::openBrowser("https://vcvrack.com/login");
		});
		// Login needs email/password text fields, which a native menu can't host;
		// use the main GUI's Library menu to sign in.
		AppendMenuW(popupLibrary, MF_STRING | MF_GRAYED, 0, L"(accedi dalla finestra principale)");
		return;
	}
	addMenuCmd(popupLibrary, L"Esci", []() {
		library::logOut();
	});
	addMenuCmd(popupLibrary, L"Account", []() {
		system::openBrowser("https://vcvrack.com/account");
	});
	addMenuCmd(popupLibrary, L"Sfoglia libreria", []() {
		system::openBrowser("https://library.vcvrack.com/");
	});
	addMenuCmd(popupLibrary, L"Aggiorna tutto", []() {
		std::thread([]() {
			library::syncUpdates();
		}).detach();
	});
	// Refresh the update list in the background, like the native Library menu.
	std::thread([]() {
		library::checkUpdates();
	}).detach();
}

void AccessibleWindow::buildMenuBar() {
	menuBar = CreateMenu();

	// Local helpers (capture `this` for addMenuCmd / pushCommand).
	auto sub = [&](HMENU parent, const wchar_t* label) -> HMENU {
		HMENU h = CreatePopupMenu();
		AppendMenuW(parent, MF_POPUP, (UINT_PTR)h, label);
		return h;
	};
	auto sep = [](HMENU h) {
		AppendMenuW(h, MF_SEPARATOR, 0, nullptr);
	};
	// Radio preset over a float* setting.
	auto fpreset = [&](HMENU h, const wchar_t* label, float* s, float v) {
		addMenuCmd(h, label, [s, v]() {
			*s = v;
		}, [s, v]() {
			return *s == v;
		});
	};

	// ── File ──────────────────────────────────────────────────────────────────
	HMENU file = sub(menuBar, L"&File");
	addMenuCmd(file, L"Nuovo", [this]() {
		pushCommand([this]() {
			APP->patch->loadTemplateDialog();
			reloadRackAfterMutation();
		});
	});
	addMenuCmd(file, L"Apri…", [this]() {
		pushCommand([this]() {
			APP->patch->loadDialog();
			reloadRackAfterMutation();
		});
	});
	popupRecent = sub(file, L"Apri recenti");   // filled in WM_INITMENUPOPUP
	addMenuCmd(file, L"Salva", [this]() {
		pushCommand([]() {
			APP->patch->saveDialog();
		});
	});
	addMenuCmd(file, L"Salva come…", [this]() {
		pushCommand([]() {
			APP->patch->saveAsDialog();
		});
	});
	addMenuCmd(file, L"Salva una copia…", [this]() {
		pushCommand([]() {
			APP->patch->saveAsDialog(false);
		});
	});
	addMenuCmd(file, L"Ripristina", [this]() {
		pushCommand([this]() {
			APP->patch->revertDialog();
			reloadRackAfterMutation();
		});
	});
	addMenuCmd(file, L"Sovrascrivi template", [this]() {
		pushCommand([]() {
			APP->patch->saveTemplateDialog();
		});
	});
	sep(file);
	addMenuCmd(file, L"Importa selezione…", [this]() {
		pushCommand([this]() {
			APP->scene->rack->loadSelectionDialog();
			reloadRackAfterMutation();
		});
	});
	sep(file);
	addMenuCmd(file, L"Esci", [this]() {
		pushCommand([]() {
			APP->window->close();
		});
	});

	// ── Edit ──────────────────────────────────────────────────────────────────
	HMENU edit = sub(menuBar, L"&Modifica");
	addMenuCmd(edit, L"Annulla", [this]() {
		pushCommand([this]() {
			if (APP->history->canUndo()) {
				APP->history->undo();
				reloadRackAfterMutation();
			}
		});
	});
	addMenuCmd(edit, L"Ripristina", [this]() {
		pushCommand([this]() {
			if (APP->history->canRedo()) {
				APP->history->redo();
				reloadRackAfterMutation();
			}
		});
	});
	addMenuCmd(edit, L"Scollega tutti i cavi", [this]() {
		pushCommand([]() {
			APP->patch->disconnectDialog();
		});
	});
	sep(edit);
	addMenuCmd(edit, L"Seleziona tutto", [this]() {
		pushCommand([]() {
			APP->scene->rack->selectAll();
		});
	});
	addMenuCmd(edit, L"Deseleziona", [this]() {
		pushCommand([]() {
			APP->scene->rack->deselectAll();
		});
	});
	addMenuCmd(edit, L"Copia selezione", [this]() {
		pushCommand([]() {
			APP->scene->rack->copyClipboardSelection();
		});
	});
	addMenuCmd(edit, L"Incolla", [this]() {
		pushCommand([this]() {
			APP->scene->rack->pasteClipboardAction();
			reloadRackAfterMutation();
		});
	});
	addMenuCmd(edit, L"Salva selezione come…", [this]() {
		pushCommand([]() {
			APP->scene->rack->saveSelectionDialog();
		});
	});
	addMenuCmd(edit, L"Azzera selezione", [this]() {
		pushCommand([]() {
			APP->scene->rack->resetSelectionAction();
		});
	});
	addMenuCmd(edit, L"Randomizza selezione", [this]() {
		pushCommand([]() {
			APP->scene->rack->randomizeSelectionAction();
		});
	});
	addMenuCmd(edit, L"Scollega selezione", [this]() {
		pushCommand([]() {
			APP->scene->rack->disconnectSelectionAction();
		});
	});
	addMenuCmd(edit, L"Bypass selezione", [this]() {
		pushCommand([]() {
			APP->scene->rack->bypassSelectionAction(!APP->scene->rack->isSelectionBypassed());
		});
	}, []() {
		return APP->scene->rack->isSelectionBypassed();
	});

	// ── View ──────────────────────────────────────────────────────────────────
	HMENU view = sub(menuBar, L"&Vista");
	addMenuCmd(view, L"Schermo intero", [this]() {
		pushCommand([]() {
			APP->window->setFullScreen(!APP->window->isFullScreen());
		});
	}, []() {
		return APP->window->isFullScreen();
	});

	HMENU zoom = sub(view, L"Zoom");
	struct {
		const wchar_t* l;
		float v;
	} zooms[] = {
		{L"25%", 0.25f}, {L"50%", 0.5f}, {L"100%", 1.0f}, {L"200%", 2.0f}, {L"400%", 4.0f}
	};
	for (auto& z : zooms) {
		float v = z.v;
		addMenuCmd(zoom, z.l, [this, v]() {
			pushCommand([v]() {
				APP->scene->rackScroll->setZoom(v);
			});
		}, [v]() {
			return std::abs(APP->scene->rackScroll->getZoom() - v) < 0.01f;
		});
	}
	addMenuCmd(view, L"Adatta allo schermo", [this]() {
		pushCommand([]() {
			APP->scene->rackScroll->zoomToModules();
		});
	});

	HMENU theme = sub(view, L"Tema interfaccia");
	struct {
		const wchar_t* l;
		const char* t;
	} themes[] = {
		{L"Scuro", "dark"}, {L"Chiaro", "light"}, {L"Scuro alto contrasto", "hcdark"}
	};
	for (auto& t : themes) {
		const char* tn = t.t;
		addMenuCmd(theme, t.l, [this, tn]() {
			settings::uiTheme = tn;
			pushCommand([]() {
				ui::refreshTheme();
			});
		}, [tn]() {
			return settings::uiTheme == tn;
		});
	}

	HMENU pixel = sub(view, L"Rapporto pixel");
	struct {
		const wchar_t* l;
		float v;
	} pixels[] = {
		{L"Auto", 0.f}, {L"100%", 1.f}, {L"150%", 1.5f}, {L"200%", 2.f}, {L"250%", 2.5f}, {L"300%", 3.f}
	};
	for (auto& p : pixels)
		fpreset(pixel, p.l, &settings::pixelRatio, p.v);

	HMENU wheel = sub(view, L"Rotellina del mouse");
	addMenuCmd(wheel, L"Scorri", []() {
		settings::mouseWheelZoom = false;
	},
	[]() {
		return !settings::mouseWheelZoom;
	});
	addMenuCmd(wheel, L"Zoom", []() {
		settings::mouseWheelZoom = true;
	},
	[]() {
		return settings::mouseWheelZoom;
	});

	addMenuCmd(view, L"Mostra tooltip", []() {
		settings::tooltips ^= true;
	},
	[]() {
		return settings::tooltips;
	});

	HMENU opacity = sub(view, L"Opacità cavi");
	for (int p = 0; p <= 100; p += 25)
		fpreset(opacity, toWide(std::to_string(p) + "%").c_str(), &settings::cableOpacity, p / 100.f);
	HMENU tension = sub(view, L"Tensione cavi");
	for (int p = 0; p <= 100; p += 25)
		fpreset(tension, toWide(std::to_string(p) + "%").c_str(), &settings::cableTension, p / 100.f);
	HMENU room = sub(view, L"Luminosità stanza");
	for (int p = 50; p <= 200; p += 25)
		fpreset(room, toWide(std::to_string(p) + "%").c_str(), &settings::rackBrightness, p / 100.f);
	HMENU halo = sub(view, L"Bagliore luci");
	for (int p = 0; p <= 100; p += 25)
		fpreset(halo, toWide(std::to_string(p) + "%").c_str(), &settings::haloBrightness, p / 100.f);

	addMenuCmd(view, L"Blocca cursore", []() {
		settings::allowCursorLock ^= true;
	},
	[]() {
		return settings::allowCursorLock;
	});

	HMENU knob = sub(view, L"Modalità manopole");
	struct {
		const wchar_t* l;
		int v;
	} knobModes[] = {
		{L"Lineare", settings::KNOB_MODE_LINEAR},
		{L"Rotativa assoluta", settings::KNOB_MODE_ROTARY_ABSOLUTE},
		{L"Rotativa relativa", settings::KNOB_MODE_ROTARY_RELATIVE},
	};
	for (auto& k : knobModes) {
		int v = k.v;
		addMenuCmd(knob, k.l, [v]() {
			settings::knobMode = (settings::KnobMode)v;
		},
		[v]() {
			return (int)settings::knobMode == v;
		});
	}
	addMenuCmd(view, L"Scorrimento manopole", []() {
		settings::knobScroll ^= true;
	},
	[]() {
		return settings::knobScroll;
	});
	HMENU wheelSens = sub(view, L"Sensibilità rotellina");
	struct {
		const wchar_t* l;
		float v;
	} senss[] = {
		{L"Bassa", 0.0005f}, {L"Media", 0.001f}, {L"Alta", 0.002f}
	};
	for (auto& s : senss)
		fpreset(wheelSens, s.l, &settings::knobScrollSensitivity, s.v);

	addMenuCmd(view, L"Blocca moduli", []() {
		settings::lockModules ^= true;
	},
	[]() {
		return settings::lockModules;
	});
	addMenuCmd(view, L"Comprimi moduli", []() {
		settings::squeezeModules ^= true;
	},
	[]() {
		return settings::squeezeModules;
	});
	addMenuCmd(view, L"Preferisci pannelli scuri", []() {
		settings::preferDarkPanels ^= true;
	},
	[]() {
		return settings::preferDarkPanels;
	});

	// ── Engine ────────────────────────────────────────────────────────────────
	HMENU engine = sub(menuBar, L"M&otore");
	addMenuCmd(engine, L"Indicatore CPU", []() {
		settings::cpuMeter ^= true;
	},
	[]() {
		return settings::cpuMeter;
	});
	HMENU srate = sub(engine, L"Frequenza di campionamento");
	addMenuCmd(srate, L"Auto", []() {
		settings::sampleRate = 0;
	},
	[]() {
		return settings::sampleRate == 0;
	});
	float rates[] = {44100.f, 48000.f, 88200.f, 96000.f, 176400.f, 192000.f};
	for (float r : rates) {
		std::wstring label = toWide(string::f("%g kHz", r / 1000.f));
		addMenuCmd(srate, label, [r]() {
			settings::sampleRate = r;
		},
		[r]() {
			return settings::sampleRate == r;
		});
	}
	HMENU threads = sub(engine, L"Thread");
	int cores = system::getLogicalCoreCount() / 2;
	if (cores < 1)
		cores = 1;
	for (int i = 1; i <= 2 * cores; i++) {
		addMenuCmd(threads, toWide(std::to_string(i)), [i]() {
			settings::threadCount = i;
		},
		[i]() {
			return settings::threadCount == i;
		});
	}

	// ── Library ───────────────────────────────────────────────────────────────
	popupLibrary = sub(menuBar, L"&Libreria");   // filled in WM_INITMENUPOPUP

	// ── Help ──────────────────────────────────────────────────────────────────
	HMENU help = sub(menuBar, L"&Aiuto");
	HMENU lang = sub(help, L"Lingua");
	for (const std::string& language : string::getLanguages()) {
		std::string l = language;
		addMenuCmd(lang, toWide(string::translate("language", l)), [this, l]() {
			if (settings::language == l)
				return;
			settings::language = l;
			if (MessageBoxW(hwnd, L"Riavviare ora per applicare la lingua?",
			                L"Lingua", MB_YESNO | MB_ICONQUESTION) == IDYES)
				pushCommand([]() {
				APP->window->close();
				settings::restart = true;
			});
		}, [l]() {
			return settings::language == l;
		});
	}
	addMenuCmd(help, L"Suggerimenti", [this]() {
		pushCommand([]() {
			APP->scene->addChild(app::tipWindowCreate());
		});
	});
	addMenuCmd(help, L"Manuale", []() {
		system::openBrowser("https://vcvrack.com/manual");
	});
	addMenuCmd(help, L"Supporto", []() {
		system::openBrowser("https://vcvrack.com/support");
	});
	addMenuCmd(help, L"VCVRack.com", []() {
		system::openBrowser("https://vcvrack.com/");
	});
	sep(help);
	addMenuCmd(help, L"Cartella utente", []() {
		system::openDirectory(asset::user(""));
	});
	addMenuCmd(help, L"Changelog", []() {
		system::openBrowser("https://github.com/VCVRack/Rack/blob/v2/CHANGELOG.md");
	});
	addMenuCmd(help, L"Controlla aggiornamenti di Rack", []() {
		std::thread([]() {
			library::checkAppUpdate();
		}).detach();
	});
}

// ── Rack view ────────────────────────────────────────────────────────────────

void AccessibleWindow::refreshRackView(app::ModuleWidget* focusModule) {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;
	HWND lv = listRack;

	// Decide which row to focus after the rebuild. By default we restore the
	// previously-focused module; callers can instead request a specific module
	// (e.g. the one just inserted) via focusModule.
	int    prevFocusedRow = lvFocused(lv);
	LPARAM prevFocusedLp  = focusModule
	                        ? (LPARAM)focusModule
	                        : ((prevFocusedRow >= 0) ? lvGetParam(lv, prevFocusedRow) : -1);

	ListView_DeleteAllItems(lv);

	auto modules = APP->scene->rack->getModules();
	std::sort(modules.begin(), modules.end(), [](app::ModuleWidget * a, app::ModuleWidget * b) {
		return a->box.pos.x < b->box.pos.x;
	});

	for (app::ModuleWidget* mw : modules) {
		if (!mw || !mw->model)
			continue;
		int  hp    = (int)(mw->box.size.x / app::RACK_GRID_WIDTH + 0.5f);
		int  row   = lvAppendRow(lv, toWide(mw->model->name), (LPARAM)mw);
		std::wstring brand = mw->model->plugin ? toWide(mw->model->plugin->getBrand()) : L"";
		lvSetSubtext(lv, row, 1, brand);
		lvSetSubtext(lv, row, 2, std::to_wstring(hp) + L" HP");
	}

	// Free slot (lParam == 0 marks it)
	int freeRow = lvAppendRow(lv, L"[ Slot libero ]", 0);
	lvSetSubtext(lv, freeRow, 1, L"");
	lvSetSubtext(lv, freeRow, 2, L"—");

	// Restore focus: find the item with the same lParam, or default to row 0
	int count = ListView_GetItemCount(lv);
	int restoreTo = 0;
	if (prevFocusedLp != -1) {
		for (int i = 0; i < count; i++) {
			if (lvGetParam(lv, i) == prevFocusedLp) {
				restoreTo = i;
				break;
			}
		}
	}
	if (count > 0) {
		ListView_SetItemState(lv, restoreTo, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
		ListView_EnsureVisible(lv, restoreTo, FALSE);
	}
}

// ── Library view ─────────────────────────────────────────────────────────────

void AccessibleWindow::refreshLibraryView() {
	TreeView_DeleteAllItems(treeLibrary);

	for (plugin::Plugin* plug : plugin::plugins) {
		if (!plug)
			continue;
		std::wstring brand = toWide(plug->getBrand());

		TVINSERTSTRUCTW tvis     = {};
		tvis.hParent             = TVI_ROOT;
		tvis.hInsertAfter        = TVI_LAST;
		tvis.item.mask           = TVIF_TEXT | TVIF_PARAM;
		tvis.item.pszText        = const_cast<wchar_t*>(brand.data());
		tvis.item.lParam         = 0;
		HTREEITEM hPlug = TreeView_InsertItem(treeLibrary, &tvis);

		for (plugin::Model* model : plug->models) {
			if (!model || model->hidden)
				continue;
			std::wstring name = toWide(model->name);

			TVINSERTSTRUCTW mvis  = {};
			mvis.hParent          = hPlug;
			mvis.hInsertAfter     = TVI_LAST;
			mvis.item.mask        = TVIF_TEXT | TVIF_PARAM;
			mvis.item.pszText     = const_cast<wchar_t*>(name.data());
			mvis.item.lParam      = (LPARAM)model;
			TreeView_InsertItem(treeLibrary, &mvis);
		}
	}
}

// ── Param view ───────────────────────────────────────────────────────────────

void AccessibleWindow::repopulateParamView() {
	ListView_DeleteAllItems(listParam);
	if (!currentModule)
		return;

	for (int i = 0; i < currentModule->getNumParams(); i++) {
		engine::ParamQuantity* pq = currentModule->getParamQuantity(i);
		if (!pq || pq->name.empty())
			continue;
		int row = lvAppendRow(listParam, toWide(pq->name), (LPARAM)i);
		lvSetSubtext(listParam, row, 1, toWide(pq->getDisplayValueString() + pq->getUnit()));
	}
}

void AccessibleWindow::refreshParamValues() {
	if (!currentModule)
		return;
	int row = 0;
	for (int i = 0; i < currentModule->getNumParams(); i++) {
		engine::ParamQuantity* pq = currentModule->getParamQuantity(i);
		if (!pq || pq->name.empty())
			continue;
		lvSetSubtext(listParam, row, 1, toWide(pq->getDisplayValueString() + pq->getUnit()));
		row++;
	}
}

// ── Port view ────────────────────────────────────────────────────────────────

void AccessibleWindow::refreshPortView(bool isOutput) {
	if (!currentModule || !APP || !APP->scene || !APP->scene->rack)
		return;
	HWND lv = isOutput ? listOutput : listInput;
	ListView_DeleteAllItems(lv);

	app::RackWidget* rack = APP->scene->rack;
	app::ModuleWidget* mw = rack->getModule(currentModule->id);

	int numPorts = isOutput ? currentModule->getNumOutputs() : currentModule->getNumInputs();
	for (int i = 0; i < numPorts; i++) {
		engine::PortInfo* info = isOutput
		                         ? currentModule->getOutputInfo(i)
		                         : currentModule->getInputInfo(i);
		std::string portName = info ? info->getName() : ("Porta " + std::to_string(i));

		// Determine cable status
		std::string status = "libero";
		if (mw) {
			app::PortWidget* pw = isOutput ? mw->getOutput(i) : mw->getInput(i);
			if (pw) {
				auto cables = rack->getCompleteCablesOnPort(pw);
				if (!cables.empty()) {
					app::CableWidget* cw = cables[0];
					app::PortWidget* remote = isOutput ? cw->inputPort : cw->outputPort;
					if (remote) {
						engine::Module* remMod = remote->module;
						if (remMod && remMod->model)
							status = "→ " + remMod->model->name;
						else
							status = "connesso";
					}
				}
			}
		}

		int row = lvAppendRow(lv, toWide(portName), (LPARAM)i);
		lvSetSubtext(lv, row, 1, toWide(status));
	}
}

// ── Actions: rack ─────────────────────────────────────────────────────────────

void AccessibleWindow::placeModule(plugin::Model* model) {
	if (!model || !APP || !APP->scene || !APP->scene->rack)
		return;

	engine::Module* m = model->createModule();
	if (!m)
		return;
	// Register the module with the engine BEFORE creating the widget, exactly as
	// the native module browser does (see Browser.cpp). This is the step we were
	// missing: RackWidget::addModule() only inserts the *widget* into the scene,
	// it does NOT add the module to the engine. Without this call the engine never
	// knows the module exists, so when the ModuleWidget is later destroyed
	// (~ModuleWidget -> setModule(NULL) -> Engine::removeModule) the engine asserts
	// that the module isn't in its list and aborts. That fired on delete, on
	// duplicate (cloneAction -> prepareSaveModule), and on every shutdown when
	// RackWidget::clear() tears down all module widgets.
	APP->engine->addModule(m);

	app::ModuleWidget* mw = model->createModuleWidget(m);
	if (!mw) {
		APP->engine->removeModule(m);
		delete m;
		return;
	}

	APP->scene->rack->setModulePosNearest(mw, nextModulePos());
	APP->scene->rack->addModule(mw);

	// Load the module's default preset, like the native browser does.
	mw->loadTemplate();

	// Register an undo action so Ctrl+Z removes the module (matches native add).
	history::ModuleAdd* ha = new history::ModuleAdd;
	ha->setModule(mw);
	APP->history->push(ha);

	setStatus("Modulo \"" + model->name + "\" aggiunto.");
	// Keep focus on the inserted module's row (not on the new free slot) so the
	// user gets immediate confirmation of what was added.
	refreshRackView(mw);
	rackDirty = false;
}

void AccessibleWindow::pasteModuleFromClipboard() {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;

	std::string clip = getClipboardTextUtf8();
	if (clip.empty()) {
		setStatus("Appunti vuoti.");
		return;
	}

	json_error_t error;
	json_t* moduleJ = json_loads(clip.c_str(), 0, &error);
	if (!moduleJ) {
		setStatus("Appunti: nessun modulo valido.");
		return;
	}
	DEFER({json_decref(moduleJ);});
	engine::Module::jsonStripIds(moduleJ);

	// Resolve the model from the JSON; bail with a message if the plugin/model
	// isn't installed (modelFromJson throws in that case).
	plugin::Model* model;
	try {
		model = plugin::modelFromJson(moduleJ);
	}
	catch (Exception& e) {
		WARN("%s", e.what());
		setStatus("Appunti: modulo non riconosciuto.");
		return;
	}

	engine::Module* m = model->createModule();
	if (!m)
		return;
	// Load state BEFORE adding to the engine: the module isn't live yet, so
	// fromJson() needs no engine lock (same reasoning as cloneAction()).
	try {
		m->fromJson(moduleJ);
	}
	catch (Exception& e) {
		WARN("%s", e.what());
	}
	APP->engine->addModule(m);

	app::ModuleWidget* mw = model->createModuleWidget(m);
	if (!mw) {
		APP->engine->removeModule(m);
		delete m;
		return;
	}

	APP->scene->rack->setModulePosNearest(mw, nextModulePos());
	APP->scene->rack->addModule(mw);

	history::ModuleAdd* ha = new history::ModuleAdd;
	ha->setModule(mw);
	APP->history->push(ha);

	setStatus("Modulo \"" + model->name + "\" incollato.");
	refreshRackView(mw);
	rackDirty = false;
}

// Ctrl+C / Ctrl+V / Ctrl+D (+Shift) on the focused RACK row. Like the context
// menu, mutations are deferred via pushCommand() so they run at the safe point
// in drainCommands() rather than during message reentrancy.
void AccessibleWindow::handleRackCtrlKey(WPARAM vk, bool shift) {
	int row = lvFocused(listRack);
	if (row < 0)
		return;
	LPARAM lp = lvGetParam(listRack, row);

	// Free slot ([ Slot libero ], lParam == 0): only paste-as-new applies here,
	// mirroring Ctrl+V over empty rack space in the standard GUI.
	if (lp == 0) {
		if (vk == 'V')
			pushCommand([this]() {
			pasteModuleFromClipboard();
		});
		return;
	}

	auto* mw = reinterpret_cast<app::ModuleWidget*>(lp);
	std::string sname = mw->model ? mw->model->name : "?";

	switch (vk) {
		case 'C':
			// Copy the focused module's preset to the clipboard.
			pushCommand([this, mw, sname]() {
				mw->copyClipboard();
				setStatus("Modulo \"" + sname + "\" copiato.");
			});
			break;

		case 'V':
			// Paste a copied preset onto the focused module, in place (same as
			// Ctrl+V over an existing module in the standard GUI).
			pushCommand([this, mw, sname]() {
				if (mw->pasteClipboardAction())
					setStatus("Preset incollato su \"" + sname + "\".");
				else
					setStatus("Impossibile incollare il preset.");
			});
			break;

		case 'D':
			// Duplicate; Shift keeps the cables. Refresh the list afterwards so the
			// clone is visible (see the matching context-menu actions).
			pushCommand([this, mw, sname, shift]() {
				mw->cloneAction(shift);
				refreshRackView();
				rackDirty = false;
				setStatus(shift
				          ? "Modulo \"" + sname + "\" duplicato (con cavi)."
				          : "Modulo \"" + sname + "\" duplicato.");
			});
			break;
	}
}

void AccessibleWindow::handleRackKey(WPARAM vk) {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;

	if (vk == VK_RETURN) {
		int row = lvFocused(listRack);
		if (row < 0)
			return;
		LPARAM lp = lvGetParam(listRack, row);

		if (lp == 0) {
			// Free slot: place model if one is queued, otherwise open the library
			// (mirrors the standard GUI's double-click-on-empty-slot behaviour).
			if (selectedModel) {
				plugin::Model* model = selectedModel;
				selectedModel = nullptr;
				// Defer the widget-tree mutation to a safe point (see drainCommands).
				pushCommand([this, model]() {
					placeModule(model);
				});
			}
			else {
				switchView(LIBRARY);
			}
		}
		else {
			currentModule = reinterpret_cast<app::ModuleWidget*>(lp)->module;
			switchView(PARAM);
		}
	}
	else if (vk == VK_DELETE || vk == VK_BACK) {
		int row = lvFocused(listRack);
		if (row < 0)
			return;
		LPARAM lp = lvGetParam(listRack, row);
		if (lp == 0)
			return;

		auto* mw = reinterpret_cast<app::ModuleWidget*>(lp);
		std::string  sname = mw->model ? mw->model->name : "?";
		std::wstring name  = toWide(sname);
		if (MessageBoxW(hwnd,
		                (L"Rimuovere \"" + name + L"\"?").c_str(),
		                L"Conferma", MB_YESNO | MB_ICONQUESTION) == IDYES) {
			// Defer the deletion: removeAction() deletes the widget and its
			// OpenGL framebuffer, which is unsafe from the message-pump
			// reentrancy point this handler can run in. drainCommands() runs it
			// from the main loop right after glfwPollEvents() instead.
			pushCommand([this, mw, sname]() {
				cleanupCapturedMenu();
				engine::Module* mod = mw->module;
				mw->removeAction();
				if (currentModule == mod) {
					currentModule   = nullptr;
					lastParamModule = nullptr;
				}
				refreshRackView();
				rackDirty = false;
				setStatus("Modulo \"" + sname + "\" rimosso.");
			});
		}
	}
	else if (vk == 'P' || vk == 'O' || vk == 'I') {
		int row = lvFocused(listRack);
		if (row < 0)
			return;
		LPARAM lp = lvGetParam(listRack, row);
		if (lp == 0)
			return;
		currentModule = reinterpret_cast<app::ModuleWidget*>(lp)->module;
		if (vk == 'P')
			switchView(PARAM);
		else if (vk == 'O')
			switchView(OUTPUT);
		else
			switchView(INPUT);
	}
}

// ── Actions: library ─────────────────────────────────────────────────────────

void AccessibleWindow::handleLibraryEnter() {
	HTREEITEM sel = TreeView_GetSelection(treeLibrary);
	if (!sel)
		return;

	TVITEMW tvi   = {};
	tvi.mask      = TVIF_PARAM | TVIF_HANDLE;
	tvi.hItem     = sel;
	TreeView_GetItem(treeLibrary, &tvi);

	if (tvi.lParam == 0) {
		// Manufacturer node: expand/collapse
		TreeView_Expand(treeLibrary, sel, TVE_TOGGLE);
		return;
	}

	selectedModel = reinterpret_cast<plugin::Model*>(tvi.lParam);
	switchView(RACK);
	setStatus("\"" + selectedModel->name + "\" selezionato — vai su [ Slot libero ] e premi Invio.");
}

// ── Actions: params ───────────────────────────────────────────────────────────

void AccessibleWindow::handleParamKey(WPARAM vk) {
	int row = lvFocused(listParam);
	if (row < 0 || !currentModule)
		return;

	int paramId = (int)lvGetParam(listParam, row);
	engine::ParamQuantity* pq = currentModule->getParamQuantity(paramId);
	if (!pq)
		return;

	if (vk == VK_BACK) {
		// Reset to default value (Ableton-style).
		pq->reset();
	}
	else if (vk == 'V') {
		std::wstring prompt = L"Valore per «" + toWide(pq->name) + L"»:\n"
		                      L"(Es: 440, C4, log2(8), dbtogain(-6))";
		std::wstring text = showInputDialog(hwnd, L"Imposta valore",
		                                    prompt, toWide(pq->getDisplayValueString()));
		if (text.empty())
			return;
		pq->setDisplayValueString(toUtf8(text));
	}
	else {
		float cur  = pq->getValue();
		float step = (pq->maxValue - pq->minValue) / 100.f;
		bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
		if (ctrl && shift)
			step *= (1.f / 100.f);  // Ctrl+Shift: very slow (mirrors Knob drag)
		else if (ctrl)
			step *= (1.f / 10.f);   // Ctrl: slow
		else if (shift)
			step *= 4.f;            // Shift: fast

		float next = cur;
		if (vk == VK_SPACE && pq->snapEnabled) {
			next = std::round(cur) + 1.f;
			if (next > pq->maxValue)
				next = pq->minValue;
		}
		else if (vk == VK_RIGHT) {
			next = cur + step;
		}
		else if (vk == VK_LEFT) {
			next = cur - step;
		}
		else {
			return;
		}

		if (pq->snapEnabled)
			next = std::round(next);
		next = math::clamp(next, pq->minValue, pq->maxValue);
		pq->setValue(next);
	}

	// Update the visible value cell, then re-fire EVENT_OBJECT_FOCUS on the row so
	// the screen reader re-reads it. Verbose (reads full row) but reliably audible.
	// TODO: announce ONLY the value (UIA notification or custom IAccessible proxy).
	std::wstring valW = toWide(pq->getDisplayValueString() + pq->getUnit());
	lvSetSubtext(listParam, row, 1, valW);
	lvFocusRow(listParam, row);
}

// ── Actions: ports / cables ──────────────────────────────────────────────────

void AccessibleWindow::handlePortEnter(bool isOutput) {
	HWND lv   = isOutput ? listOutput : listInput;
	int  row  = lvFocused(lv);
	if (row < 0 || !currentModule || !APP || !APP->engine || !APP->scene || !APP->scene->rack)
		return;

	int portId   = (int)lvGetParam(lv, row);
	int portType = isOutput ? engine::Port::OUTPUT : engine::Port::INPUT;

	if (!pendingCable.active) {
		// Start connection
		pendingCable.type   = portType;
		pendingCable.module = currentModule;
		pendingCable.portId = portId;
		pendingCable.active = true;
		engine::PortInfo* info = isOutput
		                         ? currentModule->getOutputInfo(portId)
		                         : currentModule->getInputInfo(portId);
		std::string portName = info ? info->getName() : "";
		std::string modName  = currentModule->model ? currentModule->model->name : "?";
		setStatus("Connessione da \"" + portName + "\" di " + modName +
		          " avviata. Seleziona porta di destinazione (Esc per annullare).");
		switchView(RACK);
	}
	else {
		// Complete connection
		if (pendingCable.type == portType) {
			setStatus("Porta incompatibile: un output deve collegarsi a un input.");
			return;
		}

		engine::Module* outMod; int outId;
		engine::Module* inMod;  int inId;

		if (pendingCable.type == engine::Port::OUTPUT) {
			outMod = pendingCable.module; outId = pendingCable.portId;
			inMod  = currentModule;       inId  = portId;
		}
		else {
			inMod  = pendingCable.module; inId  = pendingCable.portId;
			outMod = currentModule;       outId = portId;
		}
		pendingCable.active = false;

		std::string outName = outMod->model ? outMod->model->name : "?";
		std::string inName  = inMod->model  ? inMod->model->name  : "?";
		setStatus("Connesso: " + outName + " → " + inName + ".");
		switchView(RACK);

		// Defer the widget-tree mutation to a safe point (see drainCommands).
		pushCommand([this, outMod, outId, inMod, inId]() {
			app::RackWidget* rack = APP->scene->rack;
			// Build the cable exactly like Rack does natively: set the two
			// PortWidgets on the CableWidget and let updateCable() create the
			// engine cable, then addCable() so onAdd() registers the plug
			// widgets in plugContainer. (Hand-rolling the engine::Cable and
			// using setCable() skips the plugs, so the cable can't be found by
			// getCompleteCablesOnPort() and is never disconnected when its
			// module is removed — which fires an assert in Engine::removeModule
			// and crashes, both on delete and at shutdown.)
			app::ModuleWidget* outMw = rack->getModule(outMod->id);
			app::ModuleWidget* inMw  = rack->getModule(inMod->id);
			if (!outMw || !inMw)
				return;
			app::PortWidget* outPort = outMw->getOutput(outId);
			app::PortWidget* inPort  = inMw->getInput(inId);
			if (!outPort || !inPort)
				return;

			app::CableWidget* cw = new app::CableWidget;
			cw->color      = rack->getNextCableColor();
			cw->outputPort = outPort;
			cw->inputPort  = inPort;
			cw->updateCable();     // creates the engine cable from the two ports
			rack->addCable(cw);    // onAdd() registers the plugs
		});
	}
}

// Find the row for portId in the output/input list, focus it and let the screen
// reader announce it (used after connecting/disconnecting a cable).
void AccessibleWindow::focusPortRow(bool isOutput, int portId) {
	HWND lv = isOutput ? listOutput : listInput;
	int count = ListView_GetItemCount(lv);
	for (int i = 0; i < count; i++) {
		if ((int)lvGetParam(lv, i) == portId) {
			lvFocusRow(lv, i);
			return;
		}
	}
}

// ── Actions: disconnect cable ──────────────────────────────────────────────────

void AccessibleWindow::handlePortDelete(bool isOutput) {
	HWND lv  = isOutput ? listOutput : listInput;
	int  row = lvFocused(lv);
	if (row < 0 || !currentModule || !APP || !APP->scene || !APP->scene->rack)
		return;

	int             portId = (int)lvGetParam(lv, row);
	engine::Module* mod    = currentModule;

	// Defer the widget-tree mutation to a safe point (see drainCommands).
	pushCommand([this, mod, portId, isOutput]() {
		app::RackWidget* rack = APP->scene->rack;
		app::ModuleWidget* mw = rack->getModule(mod->id);
		if (!mw)
			return;
		app::PortWidget* pw = isOutput ? mw->getOutput(portId) : mw->getInput(portId);
		if (!pw)
			return;

		auto cables = rack->getCompleteCablesOnPort(pw);
		if (cables.empty()) {
			setStatus("Nessun cavo da scollegare su questa porta.");
		}
		else {
			// Remove every cable on the port, with a single undo action.
			history::ComplexAction* h = new history::ComplexAction;
			h->name = "scollega cavo";
			for (app::CableWidget* cw : cables) {
				history::CableRemove* hr = new history::CableRemove;
				hr->setCable(cw);
				h->push(hr);
				rack->removeCable(cw);
				delete cw;
			}
			APP->history->push(h);
			setStatus("Cavo scollegato.");
		}

		refreshPortView(isOutput);
		focusPortRow(isOutput, portId);
	});
}

// ── Subclass proc (keyboard hub) ─────────────────────────────────────────────

LRESULT CALLBACK AccessibleWindow::ChildSubclassProc(
  HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
  UINT_PTR /*uid*/, DWORD_PTR data) {
	auto* self = reinterpret_cast<AccessibleWindow*>(data);

	// WM_CONTEXTMENU is sent by the system for both the Application key and
	// Shift+F10 — the standard screen-reader shortcut for context menus.
	if (msg == WM_CONTEXTMENU) {
		// Ctrl+Application key opens the module-specific menu (Reaper-style),
		// the plain Application key / Shift+F10 the generic one. Ctrl+Apps still
		// fires WM_CONTEXTMENU, so the modifier is read here from the key state.
		if (self->currentView != CONTEXT_MENU) {
			bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
			if (ctrl)
				self->handleModuleSpecificContextMenuKey();
			else
				self->handleContextMenuKey();
		}
		return 0;
	}

	if (msg == WM_KEYDOWN) {
		bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

		// Ctrl-based clipboard / duplicate shortcuts, only in the RACK list.
		// Mirrors the standard GUI: Ctrl+C copy, Ctrl+V paste, Ctrl+D duplicate,
		// Ctrl+Shift+D duplicate with cables. Without Ctrl these letters fall
		// through to the ListView for type-ahead search.
		if (ctrl && self->currentView == RACK &&
		    (wp == 'C' || wp == 'V' || wp == 'D')) {
			self->handleRackCtrlKey(wp, shift);
			return 0;
		}

		switch (wp) {

			// Global view shortcuts
			case 'R':
				// If already in RACK, treat as explicit refresh request so
				// external changes (module dragged via mouse) become visible.
				if (self->currentView == RACK)
					self->rackDirty = true;
				self->switchView(RACK);
				return 0;

			case 'L':
				self->switchView(LIBRARY);
				return 0;

			case VK_ESCAPE:
				// Cancel Tier B learn mode first, regardless of current view.
				if (self->learningCell) {
					if (APP && APP->event)
						APP->event->setSelectedWidget(nullptr);
					self->learningCell = nullptr;
					self->learningLastText.clear();
					self->setStatus("Apprendimento annullato.");
					return 0;
				}
				if (self->currentView == CONTEXT_MENU) {
					if (self->menuStack.size() > 1) {
						// Pop one level in display menu navigation.
						if (!self->ownedSubmenus.empty()) {
							delete self->ownedSubmenus.back();
							self->ownedSubmenus.pop_back();
						}
						self->menuStack.pop_back();
						self->contextItems = self->menuStack.back();
						ListView_DeleteAllItems(self->listContextMenu);
						for (int i = 0; i < (int)self->contextItems.size(); i++)
							lvAppendRow(self->listContextMenu, self->contextItems[i].label, (LPARAM)i);
						lvFocusRow(self->listContextMenu, 0);
					}
					else {
						self->menuStack.clear();
						self->cleanupCapturedMenu();
						self->switchView(self->previousView);
					}
				}
				else if (self->pendingCable.active) {
					self->pendingCable.active = false;
					self->setStatus("Connessione annullata.");
				}
				else if (self->currentView != RACK) {
					self->switchView(RACK);
				}
				return 0;

			// Context-specific shortcuts
			case VK_RETURN:
				switch (self->currentView) {
					case RACK:    self->handleRackKey(VK_RETURN);      return 0;
					case LIBRARY: self->handleLibraryEnter();           return 0;
					case OUTPUT:  self->handlePortEnter(true);          return 0;
					case INPUT:   self->handlePortEnter(false);         return 0;
					case CONTEXT_MENU: {
						int row = lvFocused(self->listContextMenu);
						if (row >= 0 && row < (int)self->contextItems.size()) {
							auto& item = self->contextItems[row];
							if (item.isSubmenu) {
								// Push a new menu level; stay in CONTEXT_MENU.
								if (item.action)
									item.action();
							}
							else {
								auto action = item.action;
								self->switchView(self->previousView);
								if (action)
									action();
							}
						}
						return 0;
					}
					default: break;
				}
				break;

			case VK_DELETE:
				if (self->currentView == RACK) {
					self->handleRackKey(VK_DELETE);
					return 0;
				}
				else if (self->currentView == OUTPUT) {
					self->handlePortDelete(true);
					return 0;
				}
				else if (self->currentView == INPUT) {
					self->handlePortDelete(false);
					return 0;
				}
				break;

			case VK_BACK:
				// Backspace: remove module in RACK, reset param to default in
				// PARAM, disconnect cable in OUTPUT/INPUT (mirrors Del / the GUI).
				if (self->currentView == RACK) {
					self->handleRackKey(VK_BACK);
					return 0;
				}
				else if (self->currentView == PARAM) {
					self->handleParamKey(VK_BACK);
					return 0;
				}
				else if (self->currentView == OUTPUT) {
					self->handlePortDelete(true);
					return 0;
				}
				else if (self->currentView == INPUT) {
					self->handlePortDelete(false);
					return 0;
				}
				break;

			case 'P':
				if (self->currentView == RACK) {
					self->handleRackKey('P');
					return 0;
				}
				break;
			case 'O':
				if (self->currentView == RACK) {
					self->handleRackKey('O');
					return 0;
				}
				break;
			case 'I':
				if (self->currentView == RACK) {
					self->handleRackKey('I');
					return 0;
				}
				break;
			case 'D':
				if (self->currentView == RACK || self->currentView == PARAM) {
					self->handleDisplayKey();
					return 0;
				}
				break;
			case 'V':
				if (self->currentView == PARAM) {
					self->handleParamKey('V');
					return 0;
				}
				break;

			// Param value adjustment (Left/Right only; Up/Down navigate rows via default)
			case VK_LEFT:
			case VK_RIGHT:
				if (self->currentView == PARAM) {
					self->handleParamKey(wp);
					return 0;
				}
				break;

			case VK_SPACE:
				// Tier B learn mode toggle: Space selects/deselects the learning widget.
				if (self->learningCell && APP && APP->event) {
					if (APP->event->getSelectedWidget() == self->learningCell)
						APP->event->setSelectedWidget(nullptr);
					else
						APP->event->setSelectedWidget(self->learningCell);
					return 0;
				}
				if (self->currentView == PARAM) {
					self->handleParamKey(VK_SPACE);
					return 0;
				}
				break;

			case VK_F1:
				// Open the Rack manual in the system browser (same as F1 in the
				// standard GUI).
				system::openBrowser("https://vcvrack.com/manual/");
				return 0;
		}
	}

	return DefSubclassProc(hwnd, msg, wp, lp);
}

} // namespace accessible
} // namespace rack

#endif // ARCH_WIN
