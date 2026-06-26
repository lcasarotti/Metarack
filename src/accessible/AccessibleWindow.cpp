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
#include <app/ParamWidget.hpp>
#include <app/Switch.hpp>
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
#include <keyboard.hpp>
#include <app/RackScrollWidget.hpp>
#include <app/TipWindow.hpp>
#include <ui/common.hpp>
#include <app/LedDisplay.hpp>
#include <ui/MenuItem.hpp>
#include <ui/MenuOverlay.hpp>
#include <widget/event.hpp>

#include <algorithm>
#include <map>
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
// One-shot timer that releases a momentary button after a single Space press.
// The high window must outlast a few audio blocks so the module's edge detector
// reliably samples the pulse before it drops back to rest.
static const UINT_PTR TIMER_MOMENTARY = 2;
static const UINT     MOMENTARY_MS    = 80;
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

// Returns the Italian string when settings::language == "it", English otherwise.
static const wchar_t* T(const wchar_t* en, const wchar_t* it) {
	return settings::language == "it" ? it : en;
}
static std::string Ts(const char* en, const char* it) {
	return settings::language == "it" ? it : en;
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

// Pixel top-left of a grid cell, mirroring ModuleWidget::setGridPosition().
// Inserts target a cell (carried by the activated free slot) rather than always
// the end of row 0, so modules land on whichever row the user is on.
static math::Vec gridToPixel(int gridX, int gridY) {
	return math::Vec(gridX, gridY) * app::RACK_GRID_SIZE + app::RACK_OFFSET;
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

AccessibleWindow* AccessibleWindow::create(HWND owner) {
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
	self->rackHwnd = owner;
	instance = self;

	// MetaRack's only window: a normal top-level window (own Alt+Tab / taskbar entry,
	// no owner) shown unconditionally by onCreate(). The Rack GLFW window is hidden at
	// startup (see standalone.cpp), so this is the sole UI the user ever sees.
	HWND hwnd = CreateWindowExW(
	              0,
	              WND_CLASS,
	              L"MetaRack",
	              WS_OVERLAPPEDWINDOW,
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

// Custom message posted by the background login thread when it finishes.
#define WM_LOGIN_DONE (WM_USER + 1)

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
			if (self && LOWORD(wp) != WA_INACTIVE)
				SetFocus(self->activeControl());
			return 0;
		case WM_SIZE:
			if (self)
				self->onSize();
			return 0;
		case WM_TIMER:
			if (self && wp == TIMER_ID)
				self->onTimer();
			else if (self && wp == TIMER_MOMENTARY)
				self->onMomentaryRelease();
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
		case WM_LOGIN_DONE:
			// Background login thread finished — update status bar.
			if (self) {
				if (library::isLoggedIn())
					self->setStatus(Ts("Signed in to VCV Library.", "Accesso alla libreria VCV effettuato."));
				else {
					std::string err = library::loginStatus;
					self->setStatus(err.empty() ? Ts("Sign-in failed.", "Accesso fallito.") : err);
				}
			}
			return 0;
		case WM_CLOSE:
			// MetaRack's only window: closing it (X / Alt+F4) quits the app. This sets
			// glfwSetWindowShouldClose on the hidden Rack window, so APP->window->run()
			// returns and standalone.cpp deletes this window. Same path as File→Quit.
			APP->window->close();
			return 0;
		case WM_DESTROY:
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
	statusBar = CreateWindowExW(0, STATUSCLASSNAME, T(L"Ready.", L"Pronto."),
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

	// Rack ListView — icon view (not report) so the modules form a 2D spatial grid
	// that mirrors the physical rack: items are positioned manually in
	// refreshRackView() from each module's grid coordinate, so Left/Right move
	// within a row and Up/Down between rows. LVS_AUTOARRANGE is deliberately NOT
	// set: it would reflow items by window width and clobber our positions.
	DWORD rackStyle = WS_CHILD | WS_BORDER | WS_TABSTOP | LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS;
	listRack = CreateWindowExW(0, WC_LISTVIEWW, L"",
	                           rackStyle | WS_VISIBLE,
	                           0, 0, w, listH,
	                           hwnd, (HMENU)(INT_PTR)ID_RACK, hInst, nullptr);

	// Library TreeView
	treeLibrary = CreateWindowExW(0, WC_TREEVIEWW, L"",
	                              WS_CHILD | WS_BORDER | WS_TABSTOP |
	                              TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
	                              0, 0, w, listH,
	                              hwnd, (HMENU)(INT_PTR)ID_LIBRARY, hInst, nullptr);

	// Param ListView
	listParam = CreateWindowExW(0, WC_LISTVIEWW, T(L"Parameters", L"Parametri"),
	                            lvStyle,
	                            0, 0, w, listH,
	                            hwnd, (HMENU)(INT_PTR)ID_PARAM, hInst, nullptr);
	ListView_SetExtendedListViewStyle(listParam, lvEx);
	lvAddColumn(listParam, 0, T(L"Parameter", L"Parametro"), 250);
	lvAddColumn(listParam, 1, T(L"Value", L"Valore"), 210);

	// Output ListView
	listOutput = CreateWindowExW(0, WC_LISTVIEWW, T(L"Outputs", L"Uscite"),
	                             lvStyle,
	                             0, 0, w, listH,
	                             hwnd, (HMENU)(INT_PTR)ID_OUTPUT, hInst, nullptr);
	ListView_SetExtendedListViewStyle(listOutput, lvEx);
	lvAddColumn(listOutput, 0, L"Output", 250);
	lvAddColumn(listOutput, 1, T(L"State", L"Stato"), 210);

	// Input ListView
	listInput = CreateWindowExW(0, WC_LISTVIEWW, T(L"Inputs", L"Ingressi"),
	                            lvStyle,
	                            0, 0, w, listH,
	                            hwnd, (HMENU)(INT_PTR)ID_INPUT, hInst, nullptr);
	ListView_SetExtendedListViewStyle(listInput, lvEx);
	lvAddColumn(listInput, 0, L"Input", 250);
	lvAddColumn(listInput, 1, T(L"State", L"Stato"), 210);

	// Context-menu ListView (single column, initially hidden)
	listContextMenu = CreateWindowExW(0, WC_LISTVIEWW, L"",
	                                  lvStyle,
	                                  0, 0, w, listH,
	                                  hwnd, (HMENU)(INT_PTR)ID_CONTEXT_MENU, hInst, nullptr);
	ListView_SetExtendedListViewStyle(listContextMenu, lvEx);
	lvAddColumn(listContextMenu, 0, T(L"Action", L"Azione"), w - 4);

	// Subclass all controls for keyboard interception
	SetWindowSubclass(listRack,        ChildSubclassProc, 0, (DWORD_PTR)this);
	SetWindowSubclass(treeLibrary,     ChildSubclassProc, 1, (DWORD_PTR)this);
	SetWindowSubclass(listParam,       ChildSubclassProc, 2, (DWORD_PTR)this);
	SetWindowSubclass(listOutput,      ChildSubclassProc, 3, (DWORD_PTR)this);
	SetWindowSubclass(listInput,       ChildSubclassProc, 4, (DWORD_PTR)this);
	SetWindowSubclass(listContextMenu, ChildSubclassProc, 5, (DWORD_PTR)this);

	SetTimer(hwnd, TIMER_ID, TIMER_MS, nullptr);

	// Populate the rack list, then show the window: it's MetaRack's sole UI, so it
	// comes up unconditionally and takes focus straight away.
	refreshRackView();
	rackDirty = false;

	ShowWindow(hwnd, SW_SHOW);
	SetForegroundWindow(hwnd);
	SetFocus(activeControl());
}

// HWND of the control backing the active View — the one that should take focus.
HWND AccessibleWindow::activeControl() {
	HWND views[] = { listRack, treeLibrary, listParam,
	                 listOutput, listInput, listContextMenu
	               };
	return views[(int)currentView];
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
				setStatus(Ts("Value updated: ", "Valore aggiornato: ") + toUtf8(newText) + ".");
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

	items.push_back({T(L"Reset parameters", L"Azzera parametri"), [this, mw]() {
		pushCommand([mw]() {
			mw->resetAction();
		});
	}});

	items.push_back({T(L"Randomize parameters", L"Randomizza parametri"), [this, mw]() {
		pushCommand([mw]() {
			mw->randomizeAction();
		});
	}});

	items.push_back({T(L"Disconnect cables", L"Disconnetti cavi"), [this, mw]() {
		pushCommand([mw]() {
			mw->disconnectAction();
		});
	}});

	std::wstring bypassLabel = bypassed ? T(L"Bypass: disable", L"Bypass: disattiva") : T(L"Bypass: enable", L"Bypass: attiva");
	items.push_back({bypassLabel, [this, mw, bypassed]() {
		pushCommand([mw, bypassed]() {
			mw->bypassAction(!bypassed);
		});
	}});

	items.push_back({T(L"Duplicate (no cables)", L"Duplica (senza cavi)"), [this, mw]() {
		pushCommand([this, mw]() {
			std::string sname = mw->model ? mw->model->name : "?";
			mw->cloneAction(false);
			// cloneAction() inserts a new ModuleWidget into the rack, but only the
			// OpenGL view (redrawn every frame) reflects it automatically. The
			// accessible RACK list is rebuilt lazily, so refresh it here — otherwise
			// the duplicate stays invisible in the list until the next rebuild.
			refreshRackView();
			rackDirty = false;
			setStatus(Ts("Module \"", "Modulo \"") + sname + Ts("\" duplicated.", "\" duplicato."));
		});
	}});

	items.push_back({T(L"Duplicate with cables", L"Duplica con cavi"), [this, mw]() {
		pushCommand([this, mw]() {
			std::string sname = mw->model ? mw->model->name : "?";
			mw->cloneAction(true);
			// See note above: refresh the accessible RACK list so the clone appears.
			refreshRackView();
			rackDirty = false;
			setStatus(Ts("Module \"", "Modulo \"") + sname + Ts("\" duplicated (with cables).", "\" duplicato (con cavi)."));
		});
	}});

	items.push_back({T(L"Delete", L"Elimina"), [this, mw]() {
		std::string  sname = mw->model ? mw->model->name : "?";
		std::wstring name  = toWide(sname);
		if (MessageBoxW(hwnd,
		                (T(L"Remove \"", L"Rimuovere \"") + name + L"\"?").c_str(),
		                T(L"Confirm", L"Conferma"), MB_YESNO | MB_ICONQUESTION) == IDYES) {
			// Remember the deleted module's row so focus lands on the previous
			// module (row - 1), not row 0.
			int row = lvFocused(listRack);
			pushCommand([this, mw, sname, row]() {
				cleanupCapturedMenu();
				engine::Module* mod = mw->module;
				mw->removeAction();
				if (currentModule == mod) {
					currentModule   = nullptr;
					lastParamModule = nullptr;
				}
				refreshRackView(nullptr, row - 1);
				rackDirty = false;
				setStatus(Ts("Module \"", "Modulo \"") + sname + Ts("\" removed.", "\" rimosso."));
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
	writeWStr(T(L"Cancel", L"Annulla"));
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

// ── Login dialog ─────────────────────────────────────────────────────────────

struct LoginDlgData {
	std::wstring email;
	std::wstring password;
	bool ok = false;
};

static INT_PTR CALLBACK loginDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
		case WM_INITDIALOG: {
			SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
			SetDlgItemTextW(dlg, 1001, T(L"Email:", L"Email:"));
			SetDlgItemTextW(dlg, 1003, T(L"Password:", L"Password:"));
			SetFocus(GetDlgItem(dlg, 1002));
			return FALSE;
		}
		case WM_COMMAND: {
			auto* d = reinterpret_cast<LoginDlgData*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
			if (LOWORD(wp) == IDOK) {
				int elen = GetWindowTextLengthW(GetDlgItem(dlg, 1002));
				d->email.resize(elen);
				if (elen > 0)
					GetWindowTextW(GetDlgItem(dlg, 1002), &d->email[0], elen + 1);

				int plen = GetWindowTextLengthW(GetDlgItem(dlg, 1004));
				d->password.resize(plen);
				if (plen > 0)
					GetWindowTextW(GetDlgItem(dlg, 1004), &d->password[0], plen + 1);

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

// Builds a DLGTEMPLATE in memory for the login dialog: email label + edit,
// password label + edit (ES_PASSWORD), Sign in + Cancel buttons.
static std::vector<BYTE> buildLoginDlgTemplate(const std::wstring& title) {
	std::vector<BYTE> buf;
	buf.reserve(768);

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

	// DLGTEMPLATE header — 6 controls
	writeD(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
	writeD(0);
	writeW(6);         // cdit
	writeW(0); writeW(0); writeW(220); writeW(100);
	writeW(0);         // no menu
	writeW(0);         // default window class
	writeWStr(title);
	writeW(8);         // font point size
	writeWStr(L"MS Shell Dlg");

	// Item 1: Email label (id 1001)
	align4();
	writeD(WS_CHILD | WS_VISIBLE | SS_LEFT);
	writeD(0);
	writeW(7); writeW(7); writeW(206); writeW(10);
	writeW(1001);
	writeW(0xFFFF); writeW(0x0082);
	writeWStr(T(L"Email:", L"Email:"));
	writeW(0);

	// Item 2: Email edit (id 1002)
	align4();
	writeD(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL);
	writeD(0);
	writeW(7); writeW(19); writeW(206); writeW(14);
	writeW(1002);
	writeW(0xFFFF); writeW(0x0081);
	writeWStr(L"");
	writeW(0);

	// Item 3: Password label (id 1003)
	align4();
	writeD(WS_CHILD | WS_VISIBLE | SS_LEFT);
	writeD(0);
	writeW(7); writeW(38); writeW(206); writeW(10);
	writeW(1003);
	writeW(0xFFFF); writeW(0x0082);
	writeWStr(T(L"Password:", L"Password:"));
	writeW(0);

	// Item 4: Password edit (id 1004, ES_PASSWORD)
	align4();
	writeD(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD);
	writeD(0);
	writeW(7); writeW(50); writeW(206); writeW(14);
	writeW(1004);
	writeW(0xFFFF); writeW(0x0081);
	writeWStr(L"");
	writeW(0);

	// Item 5: Sign in button (IDOK)
	align4();
	writeD(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON);
	writeD(0);
	writeW(60); writeW(72); writeW(50); writeW(14);
	writeW((WORD)IDOK);
	writeW(0xFFFF); writeW(0x0080);
	writeWStr(T(L"Sign in", L"Accedi"));
	writeW(0);

	// Item 6: Cancel button (IDCANCEL)
	align4();
	writeD(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON);
	writeD(0);
	writeW(116); writeW(72); writeW(50); writeW(14);
	writeW((WORD)IDCANCEL);
	writeW(0xFFFF); writeW(0x0080);
	writeWStr(T(L"Cancel", L"Annulla"));
	writeW(0);

	return buf;
}

static LoginDlgData showLoginDialog(HWND parent) {
	LoginDlgData d;
	auto tmpl = buildLoginDlgTemplate(T(L"Sign in to VCV Library", L"Accedi alla libreria VCV"));
	HINSTANCE hInst = (HINSTANCE)GetModuleHandleW(nullptr);
	DialogBoxIndirectParamW(hInst,
	                        reinterpret_cast<LPCDLGTEMPLATEW>(tmpl.data()),
	                        parent, loginDlgProc, (LPARAM)&d);
	return d;
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

	items.push_back({T(L"Set value…", L"Imposta valore…"), [this, pq, row]() {
		std::wstring paramName = toWide(pq->name);
		std::wstring current   = toWide(pq->getDisplayValueString());
		std::wstring prompt    = T(L"Value for «", L"Valore per «") + paramName +
		                         T(L"»:\n(e.g.: 440, C4, log2(8), dbtogain(-6))",
		                           L"»:\n(Es: 440, C4, log2(8), dbtogain(-6))");

		std::wstring text = showInputDialog(hwnd, T(L"Set value", L"Imposta valore"), prompt, current);
		if (text.empty())
			return;

		pq->setDisplayValueString(toUtf8(text));

		if (row >= 0) {
			std::wstring valW = toWide(pq->getDisplayValueString() + pq->getUnit());
			lvSetSubtext(listParam, row, 1, valW);
			lvFocusRow(listParam, row);
		}
	}});

	items.push_back({T(L"Reset to default", L"Azzera al valore predefinito"), [this, pq, row]() {
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
		setStatus(Ts("No specific options for this module.", "Nessuna opzione specifica per questo modulo."));
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
			label += T(L" (unavailable)", L" (non disponibile)");
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
				setStatus(Ts("Error: unrecognized menu structure.", "Errore: struttura del menu non riconosciuta."));
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
	setStatus(Ts("Learning — press the MIDI control. Space = toggle. Esc = cancel.", "In apprendimento — premi il controllo MIDI. Spazio = toggle. Esc = annulla."));
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
	lastDisplayCellRow = 0;
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
		setStatus(Ts("No clickable displays for this module.", "Nessun display cliccabile per questo modulo."));
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
		AppendMenuW(popupRecent, MF_STRING | MF_GRAYED, 0, T(L"(no recent patches)", L"(nessuna patch recente)"));
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
		addMenuCmd(popupLibrary, T(L"Register…", L"Registrati…"), []() {
			system::openBrowser("https://vcvrack.com/login");
		});
		addMenuCmd(popupLibrary, T(L"Sign in…", L"Accedi…"), [this]() {
			LoginDlgData d = showLoginDialog(hwnd);
			if (!d.ok)
				return;
			std::string email    = toUtf8(d.email);
			std::string password = toUtf8(d.password);
			setStatus(Ts("Signing in…", "Accesso in corso…"));
			HWND h = hwnd;
			std::thread([email, password, h]() {
				library::logIn(email, password);
				library::checkUpdates();
				PostMessageW(h, WM_LOGIN_DONE, 0, 0);
			}).detach();
		});
		return;
	}
	addMenuCmd(popupLibrary, T(L"Sign out", L"Esci"), []() {
		library::logOut();
	});
	addMenuCmd(popupLibrary, L"Account", []() {
		system::openBrowser("https://vcvrack.com/account");
	});
	addMenuCmd(popupLibrary, T(L"Browse library", L"Sfoglia libreria"), []() {
		system::openBrowser("https://library.vcvrack.com/");
	});
	addMenuCmd(popupLibrary, T(L"Update all", L"Aggiorna tutto"), []() {
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
	addMenuCmd(file, T(L"New", L"Nuovo"), [this]() {
		pushCommand([this]() {
			APP->patch->loadTemplateDialog();
			reloadRackAfterMutation();
		});
	});
	addMenuCmd(file, T(L"Open…", L"Apri…"), [this]() {
		pushCommand([this]() {
			APP->patch->loadDialog();
			reloadRackAfterMutation();
		});
	});
	popupRecent = sub(file, T(L"Open Recent", L"Apri recenti"));   // filled in WM_INITMENUPOPUP
	addMenuCmd(file, T(L"Save", L"Salva"), [this]() {
		pushCommand([]() {
			APP->patch->saveDialog();
		});
	});
	addMenuCmd(file, T(L"Save as…", L"Salva come…"), [this]() {
		pushCommand([]() {
			APP->patch->saveAsDialog();
		});
	});
	addMenuCmd(file, T(L"Save a copy…", L"Salva una copia…"), [this]() {
		pushCommand([]() {
			APP->patch->saveAsDialog(false);
		});
	});
	addMenuCmd(file, T(L"Revert", L"Ripristina"), [this]() {
		pushCommand([this]() {
			APP->patch->revertDialog();
			reloadRackAfterMutation();
		});
	});
	addMenuCmd(file, T(L"Overwrite template", L"Sovrascrivi template"), [this]() {
		pushCommand([]() {
			APP->patch->saveTemplateDialog();
		});
	});
	sep(file);
	addMenuCmd(file, T(L"Import selection…", L"Importa selezione…"), [this]() {
		pushCommand([this]() {
			APP->scene->rack->loadSelectionDialog();
			reloadRackAfterMutation();
		});
	});
	sep(file);
	addMenuCmd(file, T(L"Exit", L"Esci"), [this]() {
		pushCommand([]() {
			APP->window->close();
		});
	});

	// ── Edit ──────────────────────────────────────────────────────────────────
	HMENU edit = sub(menuBar, T(L"&Edit", L"&Modifica"));
	addMenuCmd(edit, T(L"Undo", L"Annulla"), [this]() {
		pushCommand([this]() {
			if (APP->history->canUndo()) {
				APP->history->undo();
				reloadRackAfterMutation();
			}
		});
	});
	addMenuCmd(edit, T(L"Redo", L"Ripristina"), [this]() {
		pushCommand([this]() {
			if (APP->history->canRedo()) {
				APP->history->redo();
				reloadRackAfterMutation();
			}
		});
	});
	addMenuCmd(edit, T(L"Disconnect all cables", L"Scollega tutti i cavi"), [this]() {
		pushCommand([]() {
			APP->patch->disconnectDialog();
		});
	});
	sep(edit);
	addMenuCmd(edit, T(L"Select all", L"Seleziona tutto"), [this]() {
		pushCommand([this]() {
			APP->scene->rack->selectAll();
			int n = (int)APP->scene->rack->getSelected().size();
			// Re-render so every module shows the selection marker. If not currently in
			// RACK, mark dirty so the markers are built when the user switches back.
			if (currentView == RACK) {
				refreshRackView();
				rackDirty = false;
			}
			else {
				rackDirty = true;
			}
			setStatus(std::to_string(n) + Ts(" modules selected.", " moduli selezionati."));
		});
	});
	addMenuCmd(edit, T(L"Deselect", L"Deseleziona"), [this]() {
		pushCommand([this]() {
			APP->scene->rack->deselectAll();
			if (currentView == RACK) {
				refreshRackView();
				rackDirty = false;
			}
			else {
				rackDirty = true;
			}
			setStatus(Ts("Selection cleared.", "Selezione azzerata."));
		});
	});
	addMenuCmd(edit, T(L"Copy selection", L"Copia selezione"), [this]() {
		pushCommand([]() {
			APP->scene->rack->copyClipboardSelection();
		});
	});
	addMenuCmd(edit, T(L"Paste", L"Incolla"), [this]() {
		pushCommand([this]() {
			APP->scene->rack->pasteClipboardAction();
			reloadRackAfterMutation();
		});
	});
	addMenuCmd(edit, T(L"Save selection as…", L"Salva selezione come…"), [this]() {
		pushCommand([]() {
			APP->scene->rack->saveSelectionDialog();
		});
	});
	addMenuCmd(edit, T(L"Reset selection", L"Azzera selezione"), [this]() {
		pushCommand([]() {
			APP->scene->rack->resetSelectionAction();
		});
	});
	addMenuCmd(edit, T(L"Randomize selection", L"Randomizza selezione"), [this]() {
		pushCommand([]() {
			APP->scene->rack->randomizeSelectionAction();
		});
	});
	addMenuCmd(edit, T(L"Disconnect selection", L"Scollega selezione"), [this]() {
		pushCommand([]() {
			APP->scene->rack->disconnectSelectionAction();
		});
	});
	addMenuCmd(edit, T(L"Bypass selection", L"Bypass selezione"), [this]() {
		pushCommand([]() {
			APP->scene->rack->bypassSelectionAction(!APP->scene->rack->isSelectionBypassed());
		});
	}, []() {
		return APP->scene->rack->isSelectionBypassed();
	});

	// ── View ──────────────────────────────────────────────────────────────────
	HMENU view = sub(menuBar, T(L"&View", L"&Vista"));
	addMenuCmd(view, T(L"Fullscreen", L"Schermo intero"), [this]() {
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
	addMenuCmd(view, T(L"Fit to screen", L"Adatta allo schermo"), [this]() {
		pushCommand([]() {
			APP->scene->rackScroll->zoomToModules();
		});
	});

	HMENU theme = sub(view, T(L"Interface theme", L"Tema interfaccia"));
	struct {
		const wchar_t* l;
		const char* t;
	} themes[] = {
		{T(L"Dark", L"Scuro"), "dark"}, {T(L"Light", L"Chiaro"), "light"}, {T(L"Dark high contrast", L"Scuro alto contrasto"), "hcdark"}
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

	HMENU pixel = sub(view, T(L"Pixel ratio", L"Rapporto pixel"));
	struct {
		const wchar_t* l;
		float v;
	} pixels[] = {
		{L"Auto", 0.f}, {L"100%", 1.f}, {L"150%", 1.5f}, {L"200%", 2.f}, {L"250%", 2.5f}, {L"300%", 3.f}
	};
	for (auto& p : pixels)
		fpreset(pixel, p.l, &settings::pixelRatio, p.v);

	HMENU wheel = sub(view, T(L"Mouse wheel", L"Rotellina del mouse"));
	addMenuCmd(wheel, T(L"Scroll", L"Scorri"), []() {
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

	addMenuCmd(view, T(L"Show tooltips", L"Mostra tooltip"), []() {
		settings::tooltips ^= true;
	},
	[]() {
		return settings::tooltips;
	});

	HMENU opacity = sub(view, T(L"Cable opacity", L"Opacità cavi"));
	for (int p = 0; p <= 100; p += 25)
		fpreset(opacity, toWide(std::to_string(p) + "%").c_str(), &settings::cableOpacity, p / 100.f);
	HMENU tension = sub(view, T(L"Cable tension", L"Tensione cavi"));
	for (int p = 0; p <= 100; p += 25)
		fpreset(tension, toWide(std::to_string(p) + "%").c_str(), &settings::cableTension, p / 100.f);
	HMENU room = sub(view, T(L"Room brightness", L"Luminosità stanza"));
	for (int p = 50; p <= 200; p += 25)
		fpreset(room, toWide(std::to_string(p) + "%").c_str(), &settings::rackBrightness, p / 100.f);
	HMENU halo = sub(view, T(L"Light halo", L"Bagliore luci"));
	for (int p = 0; p <= 100; p += 25)
		fpreset(halo, toWide(std::to_string(p) + "%").c_str(), &settings::haloBrightness, p / 100.f);

	addMenuCmd(view, T(L"Lock cursor", L"Blocca cursore"), []() {
		settings::allowCursorLock ^= true;
	},
	[]() {
		return settings::allowCursorLock;
	});

	HMENU knob = sub(view, T(L"Knob mode", L"Modalità manopole"));
	struct {
		const wchar_t* l;
		int v;
	} knobModes[] = {
		{T(L"Linear", L"Lineare"), settings::KNOB_MODE_LINEAR},
		{T(L"Rotary absolute", L"Rotativa assoluta"), settings::KNOB_MODE_ROTARY_ABSOLUTE},
		{T(L"Rotary relative", L"Rotativa relativa"), settings::KNOB_MODE_ROTARY_RELATIVE},
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
	addMenuCmd(view, T(L"Knob scroll", L"Scorrimento manopole"), []() {
		settings::knobScroll ^= true;
	},
	[]() {
		return settings::knobScroll;
	});
	HMENU wheelSens = sub(view, T(L"Wheel sensitivity", L"Sensibilità rotellina"));
	struct {
		const wchar_t* l;
		float v;
	} senss[] = {
		{T(L"Low", L"Bassa"), 0.0005f}, {T(L"Medium", L"Media"), 0.001f}, {T(L"High", L"Alta"), 0.002f}
	};
	for (auto& s : senss)
		fpreset(wheelSens, s.l, &settings::knobScrollSensitivity, s.v);

	addMenuCmd(view, T(L"Lock modules", L"Blocca moduli"), []() {
		settings::lockModules ^= true;
	},
	[]() {
		return settings::lockModules;
	});
	addMenuCmd(view, T(L"Squeeze modules", L"Comprimi moduli"), []() {
		settings::squeezeModules ^= true;
	},
	[]() {
		return settings::squeezeModules;
	});
	addMenuCmd(view, T(L"Prefer dark panels", L"Preferisci pannelli scuri"), []() {
		settings::preferDarkPanels ^= true;
	},
	[]() {
		return settings::preferDarkPanels;
	});

	// ── Engine ────────────────────────────────────────────────────────────────
	HMENU engine = sub(menuBar, T(L"E&ngine", L"M&otore"));
	addMenuCmd(engine, T(L"CPU meter", L"Indicatore CPU"), []() {
		settings::cpuMeter ^= true;
	},
	[]() {
		return settings::cpuMeter;
	});
	HMENU srate = sub(engine, T(L"Sample rate", L"Frequenza di campionamento"));
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
	popupLibrary = sub(menuBar, T(L"&Library", L"&Libreria"));   // filled in WM_INITMENUPOPUP

	// ── Help ──────────────────────────────────────────────────────────────────
	HMENU help = sub(menuBar, T(L"&Help", L"&Aiuto"));
	HMENU lang = sub(help, T(L"Language", L"Lingua"));
	for (const std::string& language : string::getLanguages()) {
		std::string l = language;
		addMenuCmd(lang, toWide(string::translate("language", l)), [this, l]() {
			if (settings::language == l)
				return;
			settings::language = l;
			if (MessageBoxW(hwnd, T(L"Restart now to apply the language?", L"Riavviare ora per applicare la lingua?"),
			                T(L"Language", L"Lingua"), MB_YESNO | MB_ICONQUESTION) == IDYES)
				pushCommand([]() {
				APP->window->close();
				settings::restart = true;
			});
		}, [l]() {
			return settings::language == l;
		});
	}
	addMenuCmd(help, T(L"Tips", L"Suggerimenti"), [this]() {
		pushCommand([]() {
			APP->scene->addChild(app::tipWindowCreate());
		});
	});
	addMenuCmd(help, T(L"Manual", L"Manuale"), []() {
		system::openBrowser("https://vcvrack.com/manual");
	});
	addMenuCmd(help, T(L"Support", L"Supporto"), []() {
		system::openBrowser("https://vcvrack.com/support");
	});
	addMenuCmd(help, L"VCVRack.com", []() {
		system::openBrowser("https://vcvrack.com/");
	});
	sep(help);
	addMenuCmd(help, T(L"User folder", L"Cartella utente"), []() {
		system::openDirectory(asset::user(""));
	});
	addMenuCmd(help, L"Changelog", []() {
		system::openBrowser("https://github.com/VCVRack/Rack/blob/v2/CHANGELOG.md");
	});
	addMenuCmd(help, T(L"Check for Rack updates", L"Controlla aggiornamenti di Rack"), []() {
		std::thread([]() {
			library::checkAppUpdate();
		}).detach();
	});
}

// ── Rack view ────────────────────────────────────────────────────────────────

void AccessibleWindow::refreshRackView(app::ModuleWidget* focusModule, int focusRowFallback) {
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

	// Order modules the way they sit in the physical rack: top-to-bottom by row
	// (grid y), then left-to-right within a row (grid x). This same ordering also
	// drives the visual grid positions below, so the list mirrors the real layout.
	auto modules = APP->scene->rack->getModules();
	std::sort(modules.begin(), modules.end(), [](app::ModuleWidget * a, app::ModuleWidget * b) {
		math::Vec ga = a->getGridPosition();
		math::Vec gb = b->getGridPosition();
		if ((int)ga.y != (int)gb.y)
			return ga.y < gb.y;
		return ga.x < gb.x;
	});

	// Icon spacing defines our grid step: place item (col, visRow) at
	// (col*stepX, visRow*stepY) so the control's own cell size lines everything up.
	DWORD spacing = ListView_GetItemSpacing(lv, FALSE);
	int stepX = LOWORD(spacing);
	int stepY = HIWORD(spacing);

	freeSlotTargets.clear();

	// Walk the sorted modules, emitting one visual row per distinct grid y. Each
	// row is closed with its own "[ Free slot ]" (lParam == 0) placed just past the
	// rightmost module, whose grid target we remember so the user can add a module
	// to that specific row.
	int  visRow  = -1, col = 0; // ++ on first row → 0
	int  curGy   = 0;     // grid y of the row being built
	int  rowMaxRight = 0; // grid x just past the rightmost module of that row
	bool inRow   = false;

	// Spatial position spoken by NVDA, appended to each item's label. Sequential
	// 1-based position within the row (not HP), e.g. " — row 1, slot 3".
	auto coordSuffix = [](int rowIdx1, int slotIdx1) -> std::wstring {
		std::wstring s = T(L" — row ", L" — fila ");
		s += std::to_wstring(rowIdx1);
		s += T(L", slot ", L", slot ");
		s += std::to_wstring(slotIdx1);
		return s;
	};

	auto closeRowWithFreeSlot = [&]() {
		if (!inRow)
			return;
		std::wstring label = T(L"[ Free slot ]", L"[ Slot libero ]") + coordSuffix(visRow + 1, col + 1);
		int item = lvAppendRow(lv, label, 0);
		ListView_SetItemPosition(lv, item, col * stepX, visRow * stepY);
		freeSlotTargets.push_back({item, rowMaxRight, curGy});
	};

	for (app::ModuleWidget* mw : modules) {
		if (!mw || !mw->model)
			continue;
		math::Vec gpos = mw->getGridPosition();
		int gy = (int)gpos.y;
		if (!inRow || gy != curGy) {
			closeRowWithFreeSlot();   // finish the previous row first
			visRow++;
			col = 0;
			curGy = gy;
			rowMaxRight = 0;
			inRow = true;
		}

		std::wstring label = toWide(mw->model->name) + coordSuffix(visRow + 1, col + 1);
		// Multi-selection marker: appended (not prefixed) so NVDA reads it while
		// arrowing and so the module name stays first for type-ahead search.
		if (APP->scene->rack->isSelected(mw))
			label += T(L" — selected", L" — selezionato");
		int item = lvAppendRow(lv, label, (LPARAM)mw);
		ListView_SetItemPosition(lv, item, col * stepX, visRow * stepY);
		col++;

		int right = (int)gpos.x + (int)mw->getGridSize().x;
		if (right > rowMaxRight)
			rowMaxRight = right;
	}
	closeRowWithFreeSlot();   // last row

	// Empty rack: still offer one free slot at the origin so the user can add.
	if (!inRow) {
		std::wstring label = T(L"[ Free slot ]", L"[ Slot libero ]") + coordSuffix(1, 1);
		int item = lvAppendRow(lv, label, 0);
		ListView_SetItemPosition(lv, item, 0, 0);
		freeSlotTargets.push_back({item, 0, 0});
	}

	// Restore focus: find the item with the same lParam, or default to row 0
	int count = ListView_GetItemCount(lv);
	int restoreTo = 0;
	bool found = false;
	if (prevFocusedLp != -1) {
		for (int i = 0; i < count; i++) {
			if (lvGetParam(lv, i) == prevFocusedLp) {
				restoreTo = i;
				found = true;
				break;
			}
		}
	}
	// Previously-focused item is gone (e.g. just deleted): land on the neighbouring
	// slot at the same index rather than snapping back to the first module.
	if (!found && focusRowFallback >= 0 && count > 0)
		restoreTo = std::min(focusRowFallback, count - 1);
	if (count > 0) {
		ListView_SetItemState(lv, restoreTo, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
		ListView_EnsureVisible(lv, restoreTo, FALSE);
	}
}

// ── Library view ─────────────────────────────────────────────────────────────

void AccessibleWindow::refreshLibraryView() {
	TreeView_DeleteAllItems(treeLibrary);

	// Group models by brand name, merging plugins that share the same brand.
	// std::map keeps brands in alphabetical order automatically.
	std::map<std::wstring, std::vector<std::pair<std::wstring, plugin::Model*>>> byBrand;
	for (plugin::Plugin* plug : plugin::plugins) {
		if (!plug)
			continue;
		std::wstring brand = toWide(plug->getBrand());
		for (plugin::Model* model : plug->models) {
			if (!model || model->hidden)
				continue;
			byBrand[brand].push_back({toWide(model->name), model});
		}
	}

	for (auto& kv : byBrand) {
		// Sort models alphabetically within each brand.
		std::sort(kv.second.begin(), kv.second.end(),
		          [](const std::pair<std::wstring, plugin::Model*>& a,
		const std::pair<std::wstring, plugin::Model*>& b) {
			return a.first < b.first;
		});

		std::wstring brand = kv.first;
		TVINSERTSTRUCTW tvis  = {};
		tvis.hParent          = TVI_ROOT;
		tvis.hInsertAfter     = TVI_LAST;
		tvis.item.mask        = TVIF_TEXT | TVIF_PARAM;
		tvis.item.pszText     = const_cast<wchar_t*>(brand.data());
		tvis.item.lParam      = 0;
		HTREEITEM hPlug = TreeView_InsertItem(treeLibrary, &tvis);

		for (auto& mp : kv.second) {
			std::wstring name = mp.first;
			TVINSERTSTRUCTW mvis  = {};
			mvis.hParent          = hPlug;
			mvis.hInsertAfter     = TVI_LAST;
			mvis.item.mask        = TVIF_TEXT | TVIF_PARAM;
			mvis.item.pszText     = const_cast<wchar_t*>(name.data());
			mvis.item.lParam      = (LPARAM)mp.second;
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
		std::string portName = info ? info->getName() : (Ts("Port ", "Porta ") + std::to_string(i));

		// Determine cable status
		std::string status = Ts("free", "libero");
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

bool AccessibleWindow::freeSlotTargetForItem(int item, int& gridX, int& gridY) {
	for (const FreeSlotTarget& f : freeSlotTargets) {
		if (f.item == item) {
			gridX = f.gridX;
			gridY = f.gridY;
			return true;
		}
	}
	gridX = 0;
	gridY = 0;
	return false;
}

// Ctrl+Enter: drop the focused module onto a brand-new row, one grid row below the
// lowest existing module (left edge). This is the keyboard counterpart of dragging
// a module down past the bottom row in the standard GUI — it's how the user grows
// the patch into multiple rows. Undoable via history::ModuleMove.
void AccessibleWindow::moveFocusedModuleToNewRow() {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;
	int row = lvFocused(listRack);
	if (row < 0)
		return;
	LPARAM lp = lvGetParam(listRack, row);
	if (lp == 0) {
		setStatus(Ts("Free slot: no module to move.", "Slot libero: nessun modulo da spostare."));
		return;
	}
	auto* mw = reinterpret_cast<app::ModuleWidget*>(lp);

	// Defer the widget-tree mutation to the safe drain point (see drainCommands).
	pushCommand([this, mw]() {
		app::RackWidget* rack = APP->scene->rack;

		// New row = one below the lowest existing module's grid row.
		bool any = false;
		int maxGy = 0;
		for (app::ModuleWidget* m : rack->getModules()) {
			int gy = (int)m->getGridPosition().y;
			if (!any || gy > maxGy) {
				maxGy = gy;
				any = true;
			}
		}
		if (!any)
			return;
		int newGy = maxGy + 1;

		math::Vec oldPos = mw->box.pos;
		math::Vec target = gridToPixel(0, newGy);
		// The new row is empty, so requestModulePos succeeds; force-place as a guard.
		if (!rack->requestModulePos(mw, target))
			rack->setModulePosForce(mw, target);

		// Make the move undoable, like a GUI drag.
		history::ModuleMove* h = new history::ModuleMove;
		h->moduleId = mw->module->id;
		h->oldPos   = oldPos;
		h->newPos   = mw->box.pos;
		APP->history->push(h);

		refreshRackView(mw);
		rackDirty = false;
		setStatus(Ts("Module moved to a new row.", "Modulo spostato su una nuova fila."));
	});
}

void AccessibleWindow::placeModule(plugin::Model* model, int gridX, int gridY) {
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

	APP->scene->rack->setModulePosNearest(mw, gridToPixel(gridX, gridY));
	APP->scene->rack->addModule(mw);

	// Load the module's default preset, like the native browser does.
	mw->loadTemplate();

	// Register an undo action so Ctrl+Z removes the module (matches native add).
	history::ModuleAdd* ha = new history::ModuleAdd;
	ha->setModule(mw);
	APP->history->push(ha);

	setStatus(Ts("Module \"", "Modulo \"") + model->name + Ts("\" added.", "\" aggiunto."));
	// Keep focus on the inserted module's row (not on the new free slot) so the
	// user gets immediate confirmation of what was added.
	refreshRackView(mw);
	rackDirty = false;
}

void AccessibleWindow::pasteModuleFromClipboard(int gridX, int gridY) {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;

	std::string clip = getClipboardTextUtf8();
	if (clip.empty()) {
		setStatus(Ts("Clipboard is empty.", "Appunti vuoti."));
		return;
	}

	json_error_t error;
	json_t* moduleJ = json_loads(clip.c_str(), 0, &error);
	if (!moduleJ) {
		setStatus(Ts("Clipboard: no valid module.", "Appunti: nessun modulo valido."));
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
		setStatus(Ts("Clipboard: unknown module.", "Appunti: modulo non riconosciuto."));
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

	APP->scene->rack->setModulePosNearest(mw, gridToPixel(gridX, gridY));
	APP->scene->rack->addModule(mw);

	history::ModuleAdd* ha = new history::ModuleAdd;
	ha->setModule(mw);
	APP->history->push(ha);

	setStatus(Ts("Module \"", "Modulo \"") + model->name + Ts("\" pasted.", "\" incollato."));
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
		if (vk == 'V') {
			int gx = 0, gy = 0;
			freeSlotTargetForItem(row, gx, gy);
			pushCommand([this, gx, gy]() {
				pasteModuleFromClipboard(gx, gy);
			});
		}
		return;
	}

	auto* mw = reinterpret_cast<app::ModuleWidget*>(lp);
	std::string sname = mw->model ? mw->model->name : "?";

	switch (vk) {
		case 'C':
			// Copy the focused module's preset to the clipboard.
			pushCommand([this, mw, sname]() {
				mw->copyClipboard();
				setStatus(Ts("Module \"", "Modulo \"") + sname + Ts("\" copied.", "\" copiato."));
			});
			break;

		case 'V':
			// Paste a copied preset onto the focused module, in place (same as
			// Ctrl+V over an existing module in the standard GUI).
			pushCommand([this, mw, sname]() {
				if (mw->pasteClipboardAction())
					setStatus(Ts("Preset pasted onto \"", "Preset incollato su \"") + sname + "\".");
				else
					setStatus(Ts("Cannot paste preset.", "Impossibile incollare il preset."));
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
				          ? Ts("Module \"", "Modulo \"") + sname + Ts("\" duplicated (with cables).", "\" duplicato (con cavi).")
				          : Ts("Module \"", "Modulo \"") + sname + Ts("\" duplicated.", "\" duplicato."));
			});
			break;
	}
}

void AccessibleWindow::toggleRackSelection() {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;
	int row = lvFocused(listRack);
	if (row < 0)
		return;
	LPARAM lp = lvGetParam(listRack, row);
	if (lp == 0) {
		// A free slot holds no module; nothing to add to the selection.
		setStatus(Ts("Free slot: nothing to select.", "Slot libero: niente da selezionare."));
		return;
	}

	auto* mw   = reinterpret_cast<app::ModuleWidget*>(lp);
	auto* rack = APP->scene->rack;
	bool  nowSel = !rack->isSelected(mw);
	rack->select(mw, nowSel);

	// Update just this one row's label in place (add or strip the marker), rather
	// than rebuilding the whole list, then re-fire the focus event so NVDA reads the
	// updated label. This is the reliable feedback channel — status messages can be
	// dropped while NVDA is mid-speech.
	const std::wstring marker = T(L" — selected", L" — selezionato");
	wchar_t buf[512] = {};
	ListView_GetItemText(listRack, row, 0, buf, 512);
	std::wstring label = buf;
	if (label.size() >= marker.size()
	    && label.compare(label.size() - marker.size(), marker.size(), marker) == 0)
		label.erase(label.size() - marker.size());
	if (nowSel)
		label += marker;
	lvSetSubtext(listRack, row, 0, label);
	NotifyWinEvent(EVENT_OBJECT_FOCUS, listRack, OBJID_CLIENT, row + 1);

	int n = (int)rack->getSelected().size();
	std::string sname = mw->model ? mw->model->name : "?";
	setStatus("\"" + sname + "\""
	          + (nowSel ? Ts(" selected. ", " selezionato. ") : Ts(" deselected. ", " deselezionato. "))
	          + std::to_string(n) + Ts(" modules in selection.", " moduli selezionati."));
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
				// Add on the row whose free slot is focused (captured by value so the
				// deferred command isn't affected by a later list rebuild).
				int gx = 0, gy = 0;
				freeSlotTargetForItem(row, gx, gy);
				pushCommand([this, model, gx, gy]() {
					placeModule(model, gx, gy);
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

		// If a multi-selection is active, delete the whole selection in one undoable
		// action (mirrors deleting a marquee selection in the standard GUI). This
		// takes priority even when the focused row is a free slot.
		if (APP->scene->rack->hasSelection()) {
			int n = (int)APP->scene->rack->getSelected().size();
			if (MessageBoxW(hwnd,
			                (T(L"Delete ", L"Eliminare ") + std::to_wstring(n)
			                 + T(L" modules?", L" moduli?")).c_str(),
			                T(L"Confirm", L"Conferma"), MB_YESNO | MB_ICONQUESTION) == IDYES) {
				pushCommand([this, n]() {
					cleanupCapturedMenu();
					// currentModule may be among the deleted set; clear it to avoid a
					// dangling pointer in the PARAM view.
					currentModule   = nullptr;
					lastParamModule = nullptr;
					APP->scene->rack->deleteSelectionAction();
					// The deleted modules may span several rows; land focus on the
					// first remaining module rather than guessing a neighbour.
					refreshRackView(nullptr, 0);
					rackDirty = false;
					setStatus(std::to_string(n) + Ts(" modules removed.", " moduli rimossi."));
				});
			}
			return;
		}

		if (lp == 0)
			return;

		auto* mw = reinterpret_cast<app::ModuleWidget*>(lp);
		std::string  sname = mw->model ? mw->model->name : "?";
		std::wstring name  = toWide(sname);
		if (MessageBoxW(hwnd,
		                (T(L"Remove \"", L"Rimuovere \"") + name + L"\"?").c_str(),
		                T(L"Confirm", L"Conferma"), MB_YESNO | MB_ICONQUESTION) == IDYES) {
			// Defer the deletion: removeAction() deletes the widget and its
			// OpenGL framebuffer, which is unsafe from the message-pump
			// reentrancy point this handler can run in. drainCommands() runs it
			// from the main loop right after glfwPollEvents() instead.
			pushCommand([this, mw, sname, row]() {
				cleanupCapturedMenu();
				engine::Module* mod = mw->module;
				mw->removeAction();
				if (currentModule == mod) {
					currentModule   = nullptr;
					lastParamModule = nullptr;
				}
				// Focus the module before the deleted one (row - 1) instead of
				// snapping back to the first module.
				refreshRackView(nullptr, row - 1);
				rackDirty = false;
				setStatus(Ts("Module \"", "Modulo \"") + sname + Ts("\" removed.", "\" rimosso."));
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
	setStatus("\"" + selectedModel->name + "\" " + Ts("selected — go to [ Free slot ] and press Enter.", "selezionato — vai su [ Slot libero ] e premi Invio."));
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
	else if (vk == 'V' || vk == VK_RETURN) {
		std::wstring prompt = T(L"Value for «", L"Valore per «") + toWide(pq->name) +
		                      T(L"»:\n(e.g.: 440, C4, log2(8), dbtogain(-6))",
		                        L"»:\n(Es: 440, C4, log2(8), dbtogain(-6))");
		std::wstring text = showInputDialog(hwnd, T(L"Set value", L"Imposta valore"),
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
			// A momentary button (e.g. a sequencer's Run) is driven by the module's
			// edge detector: the on-screen widget sets the param high on press and
			// low on release, and the module flips its internal state on the rising
			// edge. Emulating that needs a full press-and-release, so a single Space
			// pulses the param high now and a one-shot timer drops it back to rest a
			// moment later — long enough for the audio thread to sample the pulse.
			// Without the delayed release the param would latch high, and it would
			// take a second Space just to release it before a third could toggle
			// again: exactly the two-press divergence we are fixing here. Latching
			// switches (configSwitch) fall through to the increment-and-wrap below,
			// which already changes state in a single press.
			if (isMomentaryParam(paramId)) {
				pq->setValue(pq->maxValue);
				momentaryModule  = currentModule;
				momentaryParamId = paramId;
				SetTimer(hwnd, TIMER_MOMENTARY, MOMENTARY_MS, nullptr);
				std::wstring valW = toWide(pq->getDisplayValueString() + pq->getUnit());
				lvSetSubtext(listParam, row, 1, valW);
				return;
			}
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

	// Update only the visible value cell — do NOT re-fire EVENT_OBJECT_FOCUS here,
	// as continuous NVDA announcements would drown out the synthesizer audio.
	std::wstring valW = toWide(pq->getDisplayValueString() + pq->getUnit());
	lvSetSubtext(listParam, row, 1, valW);
}

bool AccessibleWindow::isMomentaryParam(int paramId) {
	if (!currentModule || !APP || !APP->scene || !APP->scene->rack)
		return false;
	app::ModuleWidget* mw = APP->scene->rack->getModule(currentModule->id);
	if (!mw)
		return false;
	// momentary lives on the app::Switch widget, not on the ParamQuantity, so we
	// have to reach the on-screen widget to learn the button's behaviour.
	auto* sw = dynamic_cast<app::Switch*>(mw->getParam(paramId));
	return sw && sw->momentary;
}

void AccessibleWindow::onMomentaryRelease() {
	KillTimer(hwnd, TIMER_MOMENTARY);
	// Re-validate against the engine: the module could have been removed during
	// the brief high window, leaving momentaryModule dangling.
	if (momentaryModule && APP && APP->engine &&
	    APP->engine->getModule(momentaryModule->id) == momentaryModule) {
		if (engine::ParamQuantity* pq = momentaryModule->getParamQuantity(momentaryParamId))
			pq->setValue(pq->minValue);
	}
	momentaryModule  = nullptr;
	momentaryParamId = -1;
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
		setStatus(Ts("Connecting from \"", "Connessione da \"") + portName +
		          Ts("\" on ", "\" di ") + modName +
		          Ts(" started. Select destination port (Esc to cancel).", " avviata. Seleziona porta di destinazione (Esc per annullare)."));
		switchView(RACK);
	}
	else {
		// Complete connection
		if (pendingCable.type == portType) {
			setStatus(Ts("Incompatible port: an output must connect to an input.", "Porta incompatibile: un output deve collegarsi a un input."));
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
		setStatus(Ts("Connected: ", "Connesso: ") + outName + " → " + inName + ".");
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
			setStatus(Ts("No cable to disconnect on this port.", "Nessun cavo da scollegare su questa porta."));
		}
		else {
			// Remove every cable on the port, with a single undo action.
			history::ComplexAction* h = new history::ComplexAction;
			h->name = Ts("disconnect cable", "scollega cavo");
			for (app::CableWidget* cw : cables) {
				history::CableRemove* hr = new history::CableRemove;
				hr->setCable(cw);
				h->push(hr);
				rack->removeCable(cw);
				delete cw;
			}
			APP->history->push(h);
			setStatus(Ts("Cable disconnected.", "Cavo scollegato."));
		}

		refreshPortView(isOutput);
		focusPortRow(isOutput, portId);
	});
}

// ── Subclass proc (keyboard hub) ─────────────────────────────────────────────

int AccessibleWindow::midiKeyForVk(WPARAM vk) {
	// VK -> GLFW key code, restricted to the keys the keyboard MIDI driver maps to
	// a note/octave (see deviceInfos in src/keyboard.cpp). For letters/digits the
	// Win32 VK code already equals the GLFW key code; OEM and numpad keys differ.
	static const std::map<WPARAM, int> map = {
		// QWERTY — octave controls
		{ VK_OEM_3, GLFW_KEY_GRAVE_ACCENT },  // octave down
		{ '1',      GLFW_KEY_1 },             // octave up
		// QWERTY — lower row
		{ 'Z', GLFW_KEY_Z }, { 'S', GLFW_KEY_S }, { 'X', GLFW_KEY_X },
		{ 'D', GLFW_KEY_D }, { 'C', GLFW_KEY_C }, { 'V', GLFW_KEY_V },
		{ 'G', GLFW_KEY_G }, { 'B', GLFW_KEY_B }, { 'H', GLFW_KEY_H },
		{ 'N', GLFW_KEY_N }, { 'J', GLFW_KEY_J }, { 'M', GLFW_KEY_M },
		{ VK_OEM_COMMA, GLFW_KEY_COMMA }, { 'L', GLFW_KEY_L },
		{ VK_OEM_PERIOD, GLFW_KEY_PERIOD }, { VK_OEM_1, GLFW_KEY_SEMICOLON },
		{ VK_OEM_2, GLFW_KEY_SLASH },
		// QWERTY — upper row
		{ 'Q', GLFW_KEY_Q }, { '2', GLFW_KEY_2 }, { 'W', GLFW_KEY_W },
		{ '3', GLFW_KEY_3 }, { 'E', GLFW_KEY_E }, { 'R', GLFW_KEY_R },
		{ '5', GLFW_KEY_5 }, { 'T', GLFW_KEY_T }, { '6', GLFW_KEY_6 },
		{ 'Y', GLFW_KEY_Y }, { '7', GLFW_KEY_7 }, { 'U', GLFW_KEY_U },
		{ 'I', GLFW_KEY_I }, { '9', GLFW_KEY_9 }, { 'O', GLFW_KEY_O },
		{ '0', GLFW_KEY_0 }, { 'P', GLFW_KEY_P },
		{ VK_OEM_4, GLFW_KEY_LEFT_BRACKET }, { VK_OEM_PLUS, GLFW_KEY_EQUAL },
		{ VK_OEM_6, GLFW_KEY_RIGHT_BRACKET },
		// Numpad layout
		{ VK_DIVIDE, GLFW_KEY_KP_DIVIDE }, { VK_MULTIPLY, GLFW_KEY_KP_MULTIPLY },
		{ VK_NUMPAD0, GLFW_KEY_KP_0 }, { VK_DECIMAL, GLFW_KEY_KP_DECIMAL },
		{ VK_NUMPAD1, GLFW_KEY_KP_1 }, { VK_NUMPAD2, GLFW_KEY_KP_2 },
		{ VK_NUMPAD3, GLFW_KEY_KP_3 }, { VK_NUMPAD4, GLFW_KEY_KP_4 },
		{ VK_NUMPAD5, GLFW_KEY_KP_5 }, { VK_NUMPAD6, GLFW_KEY_KP_6 },
		{ VK_ADD, GLFW_KEY_KP_ADD }, { VK_NUMPAD7, GLFW_KEY_KP_7 },
		{ VK_NUMPAD8, GLFW_KEY_KP_8 }, { VK_NUMPAD9, GLFW_KEY_KP_9 },
	};
	auto it = map.find(vk);
	return it != map.end() ? it->second : 0;
}

void AccessibleWindow::toggleMidiKeyboard() {
	midiKeyboardMode = !midiKeyboardMode;
	if (!midiKeyboardMode) {
		// Release anything still held so notes don't stick after leaving the mode.
		for (int g : heldMidiKeys)
			keyboard::release(g);
		heldMidiKeys.clear();
		swallowNextChar = false;
	}
	setStatus(midiKeyboardMode
	          ? Ts("MIDI keyboard on.", "Tastiera MIDI attivata.")
	          : Ts("MIDI keyboard off.", "Tastiera MIDI disattivata."));
}

LRESULT CALLBACK AccessibleWindow::ChildSubclassProc(
  HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
  UINT_PTR /*uid*/, DWORD_PTR data) {
	auto* self = reinterpret_cast<AccessibleWindow*>(data);

	// Shift+K toggles the computer-keyboard MIDI mode (in either state).
	if (msg == WM_KEYDOWN && wp == 'K'
	    && (GetKeyState(VK_SHIFT) & 0x8000)
	    && !(GetKeyState(VK_CONTROL) & 0x8000)) {
		self->toggleMidiKeyboard();
		return 0;
	}

	// While MIDI mode is on, route note/octave keys to Rack's keyboard MIDI driver.
	// Non-note keys (and any key with a modifier) fall through to normal handling,
	// so arrows/Tab/Esc/Enter and the Ctrl-/Shift- shortcuts still navigate.
	if (self->midiKeyboardMode) {
		bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
		bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;

		if (msg == WM_KEYDOWN) {
			int g = midiKeyForVk(wp);
			if (g && !ctrl && !shift && !alt) {
				if (!(lp & (1 << 30))) {              // bit 30 set = auto-repeat
					keyboard::press(g);
					self->heldMidiKeys.push_back(g);
				}
				self->swallowNextChar = true;         // eat the trailing WM_CHAR
				return 0;                             // consume: no nav, no typeahead
			}
		}
		else if (msg == WM_KEYUP) {
			int g = midiKeyForVk(wp);
			auto& h = self->heldMidiKeys;
			auto it = std::find(h.begin(), h.end(), g);
			if (g && it != h.end()) {                 // release only notes we pressed
				keyboard::release(g);
				h.erase(it);
				return 0;
			}
		}
		else if ((msg == WM_CHAR || msg == WM_SYSCHAR) && self->swallowNextChar) {
			self->swallowNextChar = false;            // suppress the note's typeahead
			return 0;
		}
	}

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

		// Global Ctrl shortcuts (work from any view).
		if (ctrl) {
			if (wp == 'N') {
				self->pushCommand([self]() {
					APP->patch->loadTemplateDialog();
					self->reloadRackAfterMutation();
				});
				return 0;
			}
			if (wp == 'O') {
				if (shift) {
					self->pushCommand([self]() {
						APP->patch->revertDialog();
						self->reloadRackAfterMutation();
					});
				}
				else {
					self->pushCommand([self]() {
						APP->patch->loadDialog();
						self->reloadRackAfterMutation();
					});
				}
				return 0;
			}
			if (wp == 'Q') {
				self->pushCommand([]() {
					APP->window->close();
				});
				return 0;
			}
			if (wp == 'R') {
				self->pushCommand([self]() {
					auto* rack = APP->scene->rack;
					if (rack->hasSelection()) {
						int n = (int)rack->getSelected().size();
						rack->randomizeSelectionAction();
						self->setStatus(std::to_string(n) + Ts(" modules randomized.", " moduli randomizzati."));
					}
					else {
						self->setStatus(Ts("No selection to randomize.", "Nessuna selezione da randomizzare."));
					}
				});
				return 0;
			}
			if (wp == 'S') {
				if (shift)
					self->pushCommand([]() {
					APP->patch->saveAsDialog();
				});
				else
					self->pushCommand([]() {
					APP->patch->saveDialog();
				});
				return 0;
			}
			if (wp == 'Z') {
				if (shift) {
					self->pushCommand([self]() {
						if (APP->history->canRedo()) {
							APP->history->redo();
							self->reloadRackAfterMutation();
						}
					});
				}
				else {
					self->pushCommand([self]() {
						if (APP->history->canUndo()) {
							APP->history->undo();
							self->reloadRackAfterMutation();
						}
					});
				}
				return 0;
			}
		}

		// Ctrl-based clipboard / duplicate shortcuts, only in the RACK list.
		// Mirrors the standard GUI: Ctrl+C copy, Ctrl+V paste, Ctrl+D duplicate,
		// Ctrl+Shift+D duplicate with cables. Without Ctrl these letters fall
		// through to the ListView for type-ahead search.
		if (ctrl && self->currentView == RACK &&
		    (wp == 'C' || wp == 'V' || wp == 'D')) {
			self->handleRackCtrlKey(wp, shift);
			return 0;
		}

		// Ctrl+Enter in RACK: move the focused module to a new row (grows the patch
		// into multiple rows). Must be caught before the plain VK_RETURN below, which
		// would otherwise open the module's PARAM view.
		if (ctrl && self->currentView == RACK && wp == VK_RETURN) {
			self->moveFocusedModuleToNewRow();
			return 0;
		}

		switch (wp) {

			case VK_ESCAPE:
				// Cancel Tier B learn mode first, regardless of current view.
				if (self->learningCell) {
					if (APP && APP->event)
						APP->event->setSelectedWidget(nullptr);
					self->learningCell = nullptr;
					self->learningLastText.clear();
					self->setStatus(Ts("Learn mode cancelled.", "Apprendimento annullato."));
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
					self->setStatus(Ts("Connection cancelled.", "Connessione annullata."));
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
					case PARAM:   self->handleParamKey(VK_RETURN);     return 0;
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
								// If we are inside the Tier-A options for a display cell
								// (menuStack level ≥1 and displayCells populated), re-open
								// the display list after the action so the user can change
								// other settings without pressing D again.
								bool inDisplayOptions = !self->displayCells.empty() && self->menuStack.size() >= 2;
								if (inDisplayOptions) {
									int returnRow = self->lastDisplayCellRow;
									if (action)
										action();  // doAction + cleanupCapturedMenu
									// currentModule and previousView are not cleared by cleanup.
									if (APP && APP->scene && APP->scene->rack && self->currentModule) {
										app::ModuleWidget* mw2 = APP->scene->rack->getModule(self->currentModule->id);
										if (mw2) {
											self->collectDisplayCells(mw2);
											if (!self->displayCells.empty()) {
												std::vector<ContextMenuItem> cells;
												for (auto& dc : self->displayCells) {
													app::LedDisplayChoice* ch = dc.choice;
													std::wstring lbl = dc.label;
													cells.push_back({lbl, [self, ch, lbl]() {
														self->openDisplayCell({ch, lbl});
													}, false});
												}
												self->menuStack.push_back(cells);
												self->contextItems = cells;
												ListView_DeleteAllItems(self->listContextMenu);
												for (int i = 0; i < (int)self->contextItems.size(); i++)
													lvAppendRow(self->listContextMenu, self->contextItems[i].label, (LPARAM)i);
												self->switchView(CONTEXT_MENU);
												// Restore focus to the cell that opened this submenu.
												int clamp = (int)self->contextItems.size() - 1;
												lvFocusRow(self->listContextMenu, returnRow > clamp ? clamp : returnRow, true);
											}
										}
									}
								}
								else {
									// Opening a display cell (level 0→1): save row so we can
									// restore focus when returning from the submenu.
									if (!self->displayCells.empty() && self->menuStack.size() == 1)
										self->lastDisplayCellRow = row;
									self->switchView(self->previousView);
									if (action)
										action();
								}
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
					// Shift+Backspace: clear the multi-selection quickly (no deletion).
					if (shift) {
						auto* rack = APP->scene->rack;
						if (rack->hasSelection()) {
							rack->deselectAll();
							self->refreshRackView();
							self->rackDirty = false;
							self->setStatus(Ts("Selection cleared.", "Selezione azzerata."));
						}
						else {
							self->setStatus(Ts("No selection.", "Nessuna selezione."));
						}
						return 0;
					}
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

			case 'R':
				// Shift+R: go to RACK view (Ctrl+R is now randomize selection).
				if (shift) {
					if (self->currentView == RACK)
						self->rackDirty = true;
					self->switchView(RACK);
					return 0;
				}
				break;
			case 'L':
				// Shift+L: go to LIBRARY view (Ctrl+L freed for future use).
				if (shift) {
					self->switchView(LIBRARY);
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
				// In RACK, Space toggles the focused module in/out of the selection.
				if (self->currentView == RACK) {
					self->toggleRackSelection();
					return 0;
				}
				break;

			case VK_TAB: {
				// Cycle between the three module-detail views without going back to
				// the rack: Tab goes PARAM→OUTPUT→INPUT→PARAM, Shift+Tab reverses.
				// Only active when a module is already open (currentModule set).
				static const View tabCycle[] = { PARAM, OUTPUT, INPUT };
				for (int i = 0; i < 3; i++) {
					if (self->currentView == tabCycle[i]) {
						if (!self->currentModule)
							return 0;
						int next = shift ? (i + 2) % 3 : (i + 1) % 3;
						self->switchView(tabCycle[next]);
						return 0;
					}
				}
				break;
			}

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
