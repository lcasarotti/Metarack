#include <accessible/AccessibleWindowMac.hpp>

#if defined ARCH_MAC

// Window.hpp defines GLEW_STATIC and includes glew.h (then glfw3.h). It MUST come
// before <Cocoa/Cocoa.h>: AppKit drags in the system GL headers (gltypes.h) via its
// NSOpenGL classes, and glew.h refuses to be preceded by them. With glew first, the
// system GL headers are guarded out and there is no GLhandleARB typedef clash.
#include <window/Window.hpp>

#import <Cocoa/Cocoa.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h> // for glfwGetCocoaWindow()

#include <common.hpp>
#include <context.hpp>
#include <app/Scene.hpp>
#include <app/RackWidget.hpp>
#include <app/RackScrollWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <app/TipWindow.hpp>
#include <plugin/Model.hpp>
#include <engine/Module.hpp>
#include <ui/common.hpp>
#include <math.hpp>
#include <settings.hpp>
#include <logger.hpp>
#include <patch.hpp>
#include <history.hpp>
#include <string.hpp>
#include <system.hpp>
#include <asset.hpp>
#include <library.hpp>

#include <algorithm>
#include <vector>
#include <functional>
#include <string>
#include <thread>

using namespace rack;

// ─────────────────────────────────────────────────────────────────────────────
// macOS / VoiceOver accessibility layer — Cocoa counterpart of the Win32 layer in
// accessible/AccessibleWindow.cpp. A child NSWindow over Rack holds native AppKit
// controls (NSTableView for the lists), which VoiceOver reads for free — the same
// idea as the Win32 layer's native ListView/TreeView that NVDA reads.
//
// File layout: the C++ state and the platform-agnostic logic live in the
// rack::accessible namespace; the AppKit datasource/keyDown glue lives in the
// Objective-C classes below it; the C++ lifecycle (create/destroy) comes last
// because it instantiates those Objective-C classes.
// ─────────────────────────────────────────────────────────────────────────────

namespace rack {
namespace accessible {

AccessibleWindow* AccessibleWindow::instance = nullptr;

// Which list is currently shown. Mirrors the Win32 layer's View enum. Only RACK is
// implemented in this phase; the rest announce a placeholder until their phase lands.
enum AXView { AX_RACK, AX_LIBRARY, AX_PARAM, AX_OUTPUT, AX_INPUT, AX_CONTEXT_MENU };

// One row of the RACK list: either a module or a "[ Free slot ]". freeSlot rows carry
// the grid cell (gridX, gridY) where a new module would land, so Enter on a free slot
// can place a module on that specific row (used from Phase 3 onward).
struct AXRow {
	std::string        label;
	app::ModuleWidget* mw       = nullptr;
	bool               freeSlot = false;
	int                gridX    = 0;
	int                gridY    = 0;
};

// One menu-bar command: the action to run and an optional checkmark-state getter.
// Indexed by an NSMenuItem's tag — the Cocoa analogue of the Win32 MenuCmd table.
struct AXMenuCmd {
	std::function<void()> action;
	std::function<bool()> checked;
};

struct AccessibleWindow::Internal {
	NSWindow*    rackWindow  = nil;   // main Rack window; regains key when toggled off
	NSWindow*    panel       = nil;   // our accessible layer, a child window over Rack
	id           controller  = nil;   // RackAXController* (datasource/delegate), retained
	NSTableView* rackTable   = nil;   // RackAXTableView*, owned by the view hierarchy
	NSTextField* statusLabel = nil;   // status line at the bottom of the panel
	bool         visible     = false;

	// Menu bar (NSApp.mainMenu). menuTarget is the shared action/validation/delegate
	// object; recentMenu/libraryMenu are rebuilt on open via menuNeedsUpdate:.
	id           menuTarget  = nil;   // AXMenuTarget*, retained (items hold weak target refs)
	NSMenu*      recentMenu   = nil;
	NSMenu*      libraryMenu  = nil;
	std::vector<AXMenuCmd> menuCmds;

	AXView          currentView   = AX_RACK;
	engine::Module* currentModule = nullptr;  // module whose detail views are open
	plugin::Model*  selectedModel = nullptr;  // chosen in LIBRARY, awaiting a free slot
	bool            rackDirty     = true;      // rebuild the RACK list on next show

	std::vector<AXRow> rackRows;

	// Mutations queued from Cocoa event handlers, run from drainCommands().
	std::vector<std::function<void()>> commandQueue;
};

// ── Localization ─────────────────────────────────────────────────────────────
// Mirror the Win32 layer's T(en, it). macOS strings are UTF-8 std::string throughout
// (no wide conversion), so this is the only localization primitive needed.
static std::string L(const char* en, const char* it) {
	return (settings::language == "it") ? it : en;
}

// ── VoiceOver announcement ───────────────────────────────────────────────────
// Cocoa equivalent of the Win32 UiaRaiseNotificationEvent: VoiceOver speaks this even
// while mid-utterance.
static void announce(AccessibleWindow* self, const std::string& msg) {
	if (!self || !self->internal || !self->internal->panel)
		return;
	NSString* s = [NSString stringWithUTF8String:msg.c_str()];
	if (!s)
		return;
	NSDictionary* info = @{
		NSAccessibilityAnnouncementKey : s,
		NSAccessibilityPriorityKey     : @(NSAccessibilityPriorityHigh),
	};
	NSAccessibilityPostNotificationWithUserInfo(
	    self->internal->panel, NSAccessibilityAnnouncementRequestedNotification, info);
}

// Set the visible status line and announce it (the Win32 setStatus equivalent).
static void setStatus(AccessibleWindow* self, const std::string& msg) {
	if (self->internal->statusLabel)
		[self->internal->statusLabel setStringValue:[NSString stringWithUTF8String:msg.c_str()]];
	announce(self, msg);
}

static void pushCommand(AccessibleWindow* self, std::function<void()> fn) {
	self->internal->commandQueue.push_back(std::move(fn));
}

// ── Menu bar helpers ─────────────────────────────────────────────────────────
static NSString* nsstr(const std::string& s) {
	NSString* r = [NSString stringWithUTF8String:s.c_str()];
	return r ? r : @"";
}

static void axSep(NSMenu* m) {
	[m addItem:[NSMenuItem separatorItem]];
}

// Functions defined in the second C++ block (after the Objective-C classes) but
// referenced from the menu delegate below; forward-declare them here.
static void rebuildRecentMenu(AccessibleWindow* self);
static void rebuildLibraryMenu(AccessibleWindow* self);
static void reloadRackAfterMutation(AccessibleWindow* self);

// Append a command item. The action and optional checkmark getter are stored in
// menuCmds and reached via the item's tag — the Win32 addMenuCmd equivalent.
static int addCmd(AccessibleWindow* self, NSMenu* menu, const std::string& label,
                  std::function<void()> action, std::function<bool()> checked = nullptr,
                  NSString* keyEquiv = nil, NSEventModifierFlags mask = NSEventModifierFlagCommand) {
	if (!keyEquiv)
		keyEquiv = @"";
	NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:nsstr(label)
	                                            action:@selector(fire:)
	                                     keyEquivalent:keyEquiv];
	[it setTarget:(id) self->internal->menuTarget];
	if ([keyEquiv length] > 0)
		[it setKeyEquivalentModifierMask:mask];
	[it setTag:(NSInteger) self->internal->menuCmds.size()];
	[menu addItem:it];
	[it release];
	self->internal->menuCmds.push_back({std::move(action), std::move(checked)});
	return (int) self->internal->menuCmds.size() - 1;
}

// Append a submenu and return it (retained by its parent item).
static NSMenu* addSub(NSMenu* parent, const std::string& title) {
	NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:nsstr(title) action:nil keyEquivalent:@""];
	NSMenu* sub = [[NSMenu alloc] initWithTitle:nsstr(title)];
	[item setSubmenu:sub];
	[parent addItem:item];
	[item release];
	[sub release];
	return sub;
}

// Run a menu command by tag (called from AXMenuTarget -fire:).
static void menuFire(AccessibleWindow* self, NSInteger tag) {
	if (!self || !self->internal)
		return;
	if (tag < 0 || tag >= (NSInteger) self->internal->menuCmds.size())
		return;
	std::function<void()> act = self->internal->menuCmds[tag].action; // copy: dynamic rebuilds grow the vector
	if (act)
		act();
}

// Set a menu item's checkmark from its getter (called from -validateMenuItem:, the
// Win32 WM_INITMENUPOPUP equivalent — runs each time the menu is about to display).
static void menuValidate(AccessibleWindow* self, NSMenuItem* item) {
	if (!self || !self->internal)
		return;
	NSInteger tag = [item tag];
	if (tag < 0 || tag >= (NSInteger) self->internal->menuCmds.size())
		return;
	auto& chk = self->internal->menuCmds[tag].checked;
	if (chk)
		[item setState:(chk() ? NSControlStateValueOn : NSControlStateValueOff)];
}

// Rebuild the dynamic submenus right before they open (the menuNeedsUpdate: hook).
static void menuNeedsUpdate(AccessibleWindow* self, NSMenu* menu) {
	if (!self || !self->internal)
		return;
	if (menu == self->internal->recentMenu)
		rebuildRecentMenu(self);
	else if (menu == self->internal->libraryMenu)
		rebuildLibraryMenu(self);
}

// ── Rack view ────────────────────────────────────────────────────────────────
// Build the RACK row list. Linear list (NSTableView), so unlike the Win32 icon view
// no pixel positions are needed — but the same ordering (grid y, then x), the same
// "[ Free slot ]" per row, the same "— row X, slot Y" suffix and selection markers.
static void refreshRackView(AccessibleWindow* self,
                            app::ModuleWidget* focusModule = nullptr,
                            int focusRowFallback = -1) {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;
	AccessibleWindow::Internal* in = self->internal;

	// Remember which module is focused so we can restore it after the rebuild.
	app::ModuleWidget* prevMw = focusModule;
	if (!prevMw) {
		NSInteger r = [in->rackTable selectedRow];
		if (r >= 0 && r < (NSInteger) in->rackRows.size())
			prevMw = in->rackRows[r].mw;
	}

	in->rackRows.clear();

	auto modules = APP->scene->rack->getModules();
	std::sort(modules.begin(), modules.end(), [](app::ModuleWidget* a, app::ModuleWidget* b) {
		math::Vec ga = a->getGridPosition();
		math::Vec gb = b->getGridPosition();
		if ((int) ga.y != (int) gb.y)
			return ga.y < gb.y;
		return ga.x < gb.x;
	});

	int  visRow = -1, col = 0;
	int  curGy = 0, rowMaxRight = 0;
	bool inRow = false;

	auto coordSuffix = [](int rowIdx1, int slotIdx1) -> std::string {
		return L(" — row ", " — fila ") + std::to_string(rowIdx1)
		       + L(", slot ", ", slot ") + std::to_string(slotIdx1);
	};
	auto closeRowWithFreeSlot = [&]() {
		if (!inRow)
			return;
		AXRow r;
		r.freeSlot = true;
		r.gridX = rowMaxRight;
		r.gridY = curGy;
		r.label = L("[ Free slot ]", "[ Slot libero ]") + coordSuffix(visRow + 1, col + 1);
		in->rackRows.push_back(r);
	};

	for (app::ModuleWidget* mw : modules) {
		if (!mw || !mw->model)
			continue;
		math::Vec gpos = mw->getGridPosition();
		int gy = (int) gpos.y;
		if (!inRow || gy != curGy) {
			closeRowWithFreeSlot();
			visRow++;
			col = 0;
			curGy = gy;
			rowMaxRight = 0;
			inRow = true;
		}

		AXRow r;
		r.mw = mw;
		r.label = mw->model->name + coordSuffix(visRow + 1, col + 1);
		if (APP->scene->rack->isSelected(mw))
			r.label += L(" — selected", " — selezionato");
		in->rackRows.push_back(r);
		col++;

		int right = (int) gpos.x + (int) mw->getGridSize().x;
		if (right > rowMaxRight)
			rowMaxRight = right;
	}
	closeRowWithFreeSlot();

	// Empty rack: still offer one free slot at the origin so the user can add.
	if (!inRow) {
		AXRow r;
		r.freeSlot = true;
		r.label = L("[ Free slot ]", "[ Slot libero ]") + coordSuffix(1, 1);
		in->rackRows.push_back(r);
	}

	[in->rackTable reloadData];

	// Restore focus: the same module if it still exists, else the fallback row.
	int count = (int) in->rackRows.size();
	int restoreTo = 0;
	bool found = false;
	if (prevMw) {
		for (int i = 0; i < count; i++) {
			if (in->rackRows[i].mw == prevMw) {
				restoreTo = i;
				found = true;
				break;
			}
		}
	}
	if (!found && focusRowFallback >= 0 && count > 0)
		restoreTo = std::min(focusRowFallback, count - 1);
	if (count > 0) {
		[in->rackTable selectRowIndexes:[NSIndexSet indexSetWithIndex:restoreTo]
		           byExtendingSelection:NO];
		[in->rackTable scrollRowToVisible:restoreTo];
	}
}

static const char* axViewName(AXView v) {
	switch (v) {
		case AX_LIBRARY: return "Library";
		case AX_PARAM:   return "Parameters";
		case AX_OUTPUT:  return "Outputs";
		case AX_INPUT:   return "Inputs";
		default:         return "View";
	}
}

// Show a view's control and focus it. Only RACK is wired in this phase; the others
// announce a placeholder so the keys are testable end to end before their phase.
static void switchTo(AccessibleWindow* self, AXView v) {
	AccessibleWindow::Internal* in = self->internal;
	if (v == AX_RACK) {
		in->currentView = AX_RACK;
		[in->rackTable setHidden:NO];
		if (in->rackDirty) {
			refreshRackView(self);
			in->rackDirty = false;
		}
		[in->panel makeFirstResponder:in->rackTable];
		if ([in->rackTable selectedRow] < 0 && !in->rackRows.empty())
			[in->rackTable selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
			           byExtendingSelection:NO];
		NSAccessibilityPostNotification(in->rackTable,
		    NSAccessibilitySelectedRowsChangedNotification);
		return;
	}
	announce(self, std::string(axViewName(v)) + L(": not yet implemented", ": non ancora implementato"));
}

// ── Confirmation dialog (NSAlert) ────────────────────────────────────────────
// Replaces the Win32 MessageBoxW yes/no prompts.
static bool confirm(const std::string& msg) {
	NSAlert* a = [[NSAlert alloc] init];
	[a setMessageText:[NSString stringWithUTF8String:msg.c_str()]];
	[a addButtonWithTitle:[NSString stringWithUTF8String:L("Yes", "Sì").c_str()]];
	[a addButtonWithTitle:[NSString stringWithUTF8String:L("No", "No").c_str()]];
	NSModalResponse r = [a runModal];
	[a release];
	return r == NSAlertFirstButtonReturn;
}

// ── RACK key handlers ────────────────────────────────────────────────────────
static AXRow* focusedRackRow(AccessibleWindow* self) {
	NSInteger row = [self->internal->rackTable selectedRow];
	if (row < 0 || row >= (NSInteger) self->internal->rackRows.size())
		return nullptr;
	return &self->internal->rackRows[row];
}

static void onRackEnter(AccessibleWindow* self) {
	AXRow* r = focusedRackRow(self);
	if (!r)
		return;
	if (r->freeSlot) {
		// Place a queued model (Phase 3) or open the library, mirroring the GUI's
		// double-click-on-empty-slot behaviour.
		if (self->internal->selectedModel) {
			// TODO Phase 3: placeModule(selectedModel, r->gridX, r->gridY).
			switchTo(self, AX_LIBRARY);
		}
		else {
			switchTo(self, AX_LIBRARY);
		}
	}
	else {
		self->internal->currentModule = r->mw->module;
		switchTo(self, AX_PARAM);
	}
}

static void onRackToggleSelect(AccessibleWindow* self) {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;
	AccessibleWindow::Internal* in = self->internal;
	NSInteger row = [in->rackTable selectedRow];
	AXRow* r = focusedRackRow(self);
	if (!r)
		return;
	if (r->freeSlot) {
		setStatus(self, L("Free slot: nothing to select.", "Slot libero: niente da selezionare."));
		return;
	}

	auto* rack = APP->scene->rack;
	bool nowSel = !rack->isSelected(r->mw);
	rack->select(r->mw, nowSel);

	// Add/strip the marker on this one row's label, then re-read it.
	std::string marker = L(" — selected", " — selezionato");
	std::string lbl = r->label;
	if (lbl.size() >= marker.size()
	    && lbl.compare(lbl.size() - marker.size(), marker.size(), marker) == 0)
		lbl.erase(lbl.size() - marker.size());
	if (nowSel)
		lbl += marker;
	r->label = lbl;
	[in->rackTable reloadDataForRowIndexes:[NSIndexSet indexSetWithIndex:row]
	                         columnIndexes:[NSIndexSet indexSetWithIndex:0]];

	std::string sname = r->mw->model ? r->mw->model->name : "?";
	int n = (int) rack->getSelected().size();
	setStatus(self, "\"" + sname + "\""
	          + (nowSel ? L(" selected. ", " selezionato. ") : L(" deselected. ", " deselezionato. "))
	          + std::to_string(n) + L(" modules in selection.", " moduli selezionati."));
}

static void onRackDelete(AccessibleWindow* self) {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;
	AccessibleWindow::Internal* in = self->internal;
	auto* rack = APP->scene->rack;

	// A multi-selection deletes as one undoable action, even if a free slot is focused.
	if (rack->hasSelection()) {
		int n = (int) rack->getSelected().size();
		if (confirm(L("Delete ", "Eliminare ") + std::to_string(n) + L(" modules?", " moduli?"))) {
			pushCommand(self, [self, n]() {
				self->internal->currentModule = nullptr;
				APP->scene->rack->deleteSelectionAction();
				refreshRackView(self, nullptr, 0);
				self->internal->rackDirty = false;
				setStatus(self, std::to_string(n) + L(" modules removed.", " moduli rimossi."));
			});
		}
		return;
	}

	NSInteger row = [in->rackTable selectedRow];
	AXRow* r = focusedRackRow(self);
	if (!r || r->freeSlot)
		return;

	app::ModuleWidget* mw = r->mw;
	std::string sname = mw->model ? mw->model->name : "?";
	if (confirm(L("Remove \"", "Rimuovere \"") + sname + L("\"?", "\"?"))) {
		int rowi = (int) row;
		// Defer: removeAction() tears down the widget and its OpenGL framebuffer,
		// unsafe from a Cocoa event handler; drainCommands() runs it from the main loop.
		pushCommand(self, [self, mw, sname, rowi]() {
			engine::Module* mod = mw->module;
			mw->removeAction();
			if (self->internal->currentModule == mod)
				self->internal->currentModule = nullptr;
			refreshRackView(self, nullptr, rowi - 1);
			self->internal->rackDirty = false;
			setStatus(self, L("Module \"", "Modulo \"") + sname + L("\" removed.", "\" rimosso."));
		});
	}
}

static void onRackPOI(AccessibleWindow* self, char which) {
	AXRow* r = focusedRackRow(self);
	if (!r || r->freeSlot)
		return;
	self->internal->currentModule = r->mw->module;
	switchTo(self, which == 'P' ? AX_PARAM : (which == 'O' ? AX_OUTPUT : AX_INPUT));
}

// ── Show / hide ──────────────────────────────────────────────────────────────
static void setLayerVisible(AccessibleWindow* self, bool show) {
	AccessibleWindow::Internal* in = self->internal;
	if (show) {
		[in->panel setFrame:[in->rackWindow frame] display:YES];
		[in->rackWindow addChildWindow:in->panel ordered:NSWindowAbove];
		[in->panel makeKeyAndOrderFront:nil];
		in->visible = true;
		// Always rebuild on show so the list reflects any changes made in the GUI.
		in->rackDirty = true;
		switchTo(self, in->currentView);
		announce(self, L("Accessible interface", "Interfaccia accessibile"));
	}
	else {
		[in->rackWindow removeChildWindow:in->panel];
		[in->panel orderOut:nil];
		[in->rackWindow makeKeyAndOrderFront:nil];
		in->visible = false;
	}
}

} // namespace accessible
} // namespace rack

// ─────────────────────────────────────────────────────────────────────────────
// AppKit glue
// ─────────────────────────────────────────────────────────────────────────────

// Datasource/delegate for the list tables. Reads rows straight from the C++ state.
@interface RackAXController : NSObject <NSTableViewDataSource, NSTableViewDelegate> {
@public
	rack::accessible::AccessibleWindow* owner;
}
@end

@implementation RackAXController
- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
	if (!owner || !owner->internal)
		return 0;
	return (NSInteger) owner->internal->rackRows.size();
}
- (id)tableView:(NSTableView*)tv objectValueForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
	if (!owner || !owner->internal)
		return @"";
	auto& rows = owner->internal->rackRows;
	if (row < 0 || row >= (NSInteger) rows.size())
		return @"";
	return [NSString stringWithUTF8String:rows[row].label.c_str()];
}
@end

// NSTableView that routes action keys to the C++ handlers — the equivalent of the
// Win32 ChildSubclassProc. Arrow keys fall through to super for native list navigation
// (VoiceOver reads each row as it is selected).
@interface RackAXTableView : NSTableView {
@public
	rack::accessible::AccessibleWindow* owner;
}
@end

@implementation RackAXTableView
- (void)keyDown:(NSEvent*)e {
	using namespace rack::accessible;
	if (!owner || !owner->internal) {
		[super keyDown:e];
		return;
	}
	NSEventModifierFlags m = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
	// Cmd-combos belong to the menu bar (Phase 2); let them through.
	if (m & NSEventModifierFlagCommand) {
		[super keyDown:e];
		return;
	}
	unsigned short kc = e.keyCode;
	NSString* ch = [[e charactersIgnoringModifiers] lowercaseString];

	if (kc == 36 || kc == 76) {          // Return / keypad Enter
		onRackEnter(owner);
		return;
	}
	if (kc == 51 || kc == 117) {         // Backspace / Forward Delete
		onRackDelete(owner);
		return;
	}
	if (kc == 49) {                      // Space
		onRackToggleSelect(owner);
		return;
	}
	if ([ch isEqualToString:@"p"]) { onRackPOI(owner, 'P'); return; }
	if ([ch isEqualToString:@"o"]) { onRackPOI(owner, 'O'); return; }
	if ([ch isEqualToString:@"i"]) { onRackPOI(owner, 'I'); return; }
	if ([ch isEqualToString:@"d"]) {
		announce(owner, L("Display cells: not yet implemented", "Celle display: non ancora implementato"));
		return;
	}
	[super keyDown:e];
}
@end

// Single target for every menu item: routes -fire: to the command's action, sets
// checkmarks in -validateMenuItem:, and rebuilds the dynamic submenus as their
// delegate. Mirrors the Win32 WM_COMMAND / WM_INITMENUPOPUP handling.
@interface AXMenuTarget : NSObject <NSMenuDelegate> {
@public
	rack::accessible::AccessibleWindow* owner;
}
@end

@implementation AXMenuTarget
- (void)fire:(id)sender {
	rack::accessible::menuFire(owner, [(NSMenuItem*) sender tag]);
}
- (BOOL)validateMenuItem:(NSMenuItem*)item {
	rack::accessible::menuValidate(owner, item);
	return YES;
}
- (void)menuNeedsUpdate:(NSMenu*)menu {
	rack::accessible::menuNeedsUpdate(owner, menu);
}
@end

// ─────────────────────────────────────────────────────────────────────────────
// C++ lifecycle (instantiates the Objective-C classes above)
// ─────────────────────────────────────────────────────────────────────────────

namespace rack {
namespace accessible {

// Clear pointers that a patch load / undo / paste may have invalidated, then rebuild
// the RACK list. Mirrors the Win32 reloadRackAfterMutation.
static void reloadRackAfterMutation(AccessibleWindow* self) {
	self->internal->currentModule = nullptr;
	self->internal->rackDirty = true;
	if (self->internal->visible)
		switchTo(self, AX_RACK);
}

// ── Login dialog (NSAlert with email + password fields) ──────────────────────
struct LoginResult {
	bool ok = false;
	std::string email, password;
};
static LoginResult showLoginDialog() {
	LoginResult res;
	NSAlert* a = [[NSAlert alloc] init];
	[a setMessageText:nsstr(L("Sign in to VCV Library", "Accedi alla libreria VCV"))];
	[a addButtonWithTitle:nsstr(L("Sign in", "Accedi"))];
	[a addButtonWithTitle:nsstr(L("Cancel", "Annulla"))];

	NSView* acc = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 56)];
	NSTextField* email = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 30, 320, 24)];
	[[email cell] setPlaceholderString:nsstr(L("Email", "Email"))];
	NSSecureTextField* pass = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
	[[pass cell] setPlaceholderString:nsstr(L("Password", "Password"))];
	[acc addSubview:email];
	[acc addSubview:pass];
	[a setAccessoryView:acc];

	NSModalResponse r = [a runModal];
	if (r == NSAlertFirstButtonReturn) {
		res.ok = true;
		const char* e = [[email stringValue] UTF8String];
		const char* p = [[pass stringValue] UTF8String];
		res.email = e ? e : "";
		res.password = p ? p : "";
	}
	[email release];
	[pass release];
	[acc release];
	[a release];
	return res;
}

// ── Dynamic submenus (rebuilt on open) ───────────────────────────────────────
static void rebuildRecentMenu(AccessibleWindow* self) {
	NSMenu* m = self->internal->recentMenu;
	[m removeAllItems];
	if (settings::recentPatchPaths.empty()) {
		NSMenuItem* it = [[NSMenuItem alloc]
		    initWithTitle:nsstr(L("(no recent patches)", "(nessuna patch recente)"))
		           action:nil keyEquivalent:@""];
		[it setEnabled:NO];
		[m addItem:it];
		[it release];
		return;
	}
	for (const std::string& path : settings::recentPatchPaths) {
		std::string p = path;
		addCmd(self, m, system::getStem(path), [self, p]() {
			pushCommand(self, [self, p]() {
				APP->patch->loadPathDialog(p);
				reloadRackAfterMutation(self);
			});
		});
	}
}

static void rebuildLibraryMenu(AccessibleWindow* self) {
	NSMenu* m = self->internal->libraryMenu;
	[m removeAllItems];
	if (!library::isLoggedIn()) {
		addCmd(self, m, L("Register…", "Registrati…"), []() {
			system::openBrowser("https://vcvrack.com/login");
		});
		addCmd(self, m, L("Sign in…", "Accedi…"), [self]() {
			LoginResult d = showLoginDialog();
			if (!d.ok)
				return;
			std::string email = d.email, password = d.password;
			setStatus(self, L("Signing in…", "Accesso in corso…"));
			std::thread([self, email, password]() {
				library::logIn(email, password);
				library::checkUpdates();
				dispatch_async(dispatch_get_main_queue(), ^{
					setStatus(self, L("Signed in.", "Accesso effettuato."));
				});
			}).detach();
		});
		return;
	}
	addCmd(self, m, L("Sign out", "Esci"), []() {
		library::logOut();
	});
	addCmd(self, m, "Account", []() {
		system::openBrowser("https://vcvrack.com/account");
	});
	addCmd(self, m, L("Browse library", "Sfoglia libreria"), []() {
		system::openBrowser("https://library.vcvrack.com/");
	});
	addCmd(self, m, L("Update all", "Aggiorna tutto"), []() {
		std::thread([]() {
			library::syncUpdates();
		}).detach();
	});
	std::thread([]() {
		library::checkUpdates();
	}).detach();
}

// ── Menu bar ─────────────────────────────────────────────────────────────────
static void buildMenuBar(AccessibleWindow* self) {
	NSMenu* mainMenu = [[NSMenu alloc] init];

	auto fpreset = [&](NSMenu* m, const std::string& label, float* s, float v) {
		addCmd(self, m, label, [s, v]() { *s = v; }, [s, v]() { return *s == v; });
	};

	// ── Application menu (first; its title is replaced by the app name) ─────────
	NSMenuItem* appItem = [[NSMenuItem alloc] init];
	[mainMenu addItem:appItem];
	[appItem release];
	NSMenu* appMenu = [[NSMenu alloc] init];
	[appItem setSubmenu:appMenu];
	[appMenu release];
	addCmd(self, appMenu, "VCVRack.com", []() {
		system::openBrowser("https://vcvrack.com/");
	});
	axSep(appMenu);
	addCmd(self, appMenu, L("Hide Rack", "Nascondi Rack"), []() {
		[NSApp hide:nil];
	}, nullptr, @"h");
	addCmd(self, appMenu, L("Quit Rack", "Esci da Rack"), [self]() {
		pushCommand(self, []() { APP->window->close(); });
	}, nullptr, @"q");

	// ── File ────────────────────────────────────────────────────────────────────
	NSMenu* file = addSub(mainMenu, L("File", "File"));
	addCmd(self, file, L("New", "Nuovo"), [self]() {
		pushCommand(self, [self]() { APP->patch->loadTemplateDialog(); reloadRackAfterMutation(self); });
	}, nullptr, @"n");
	addCmd(self, file, L("Open…", "Apri…"), [self]() {
		pushCommand(self, [self]() { APP->patch->loadDialog(); reloadRackAfterMutation(self); });
	}, nullptr, @"o");
	self->internal->recentMenu = addSub(file, L("Open Recent", "Apri recenti"));
	[self->internal->recentMenu setDelegate:(id) self->internal->menuTarget];
	addCmd(self, file, L("Save", "Salva"), [self]() {
		pushCommand(self, []() { APP->patch->saveDialog(); });
	}, nullptr, @"s");
	addCmd(self, file, L("Save as…", "Salva come…"), [self]() {
		pushCommand(self, []() { APP->patch->saveAsDialog(); });
	}, nullptr, @"s", NSEventModifierFlagCommand | NSEventModifierFlagShift);
	addCmd(self, file, L("Save a copy…", "Salva una copia…"), [self]() {
		pushCommand(self, []() { APP->patch->saveAsDialog(false); });
	});
	addCmd(self, file, L("Revert", "Ripristina"), [self]() {
		pushCommand(self, [self]() { APP->patch->revertDialog(); reloadRackAfterMutation(self); });
	});
	addCmd(self, file, L("Overwrite template", "Sovrascrivi template"), [self]() {
		pushCommand(self, []() { APP->patch->saveTemplateDialog(); });
	});
	axSep(file);
	addCmd(self, file, L("Import selection…", "Importa selezione…"), [self]() {
		pushCommand(self, [self]() { APP->scene->rack->loadSelectionDialog(); reloadRackAfterMutation(self); });
	});

	// ── Edit ────────────────────────────────────────────────────────────────────
	NSMenu* edit = addSub(mainMenu, L("Edit", "Modifica"));
	addCmd(self, edit, L("Undo", "Annulla"), [self]() {
		pushCommand(self, [self]() {
			if (APP->history->canUndo()) { APP->history->undo(); reloadRackAfterMutation(self); }
		});
	}, nullptr, @"z");
	addCmd(self, edit, L("Redo", "Ripristina"), [self]() {
		pushCommand(self, [self]() {
			if (APP->history->canRedo()) { APP->history->redo(); reloadRackAfterMutation(self); }
		});
	}, nullptr, @"z", NSEventModifierFlagCommand | NSEventModifierFlagShift);
	addCmd(self, edit, L("Disconnect all cables", "Scollega tutti i cavi"), [self]() {
		pushCommand(self, []() { APP->patch->disconnectDialog(); });
	});
	axSep(edit);
	addCmd(self, edit, L("Select all", "Seleziona tutto"), [self]() {
		pushCommand(self, [self]() {
			APP->scene->rack->selectAll();
			int n = (int) APP->scene->rack->getSelected().size();
			self->internal->rackDirty = true;
			if (self->internal->visible && self->internal->currentView == AX_RACK)
				switchTo(self, AX_RACK);
			setStatus(self, std::to_string(n) + L(" modules selected.", " moduli selezionati."));
		});
	});
	addCmd(self, edit, L("Deselect", "Deseleziona"), [self]() {
		pushCommand(self, [self]() {
			APP->scene->rack->deselectAll();
			self->internal->rackDirty = true;
			if (self->internal->visible && self->internal->currentView == AX_RACK)
				switchTo(self, AX_RACK);
			setStatus(self, L("Selection cleared.", "Selezione azzerata."));
		});
	});
	addCmd(self, edit, L("Copy selection", "Copia selezione"), [self]() {
		pushCommand(self, []() { APP->scene->rack->copyClipboardSelection(); });
	});
	addCmd(self, edit, L("Paste", "Incolla"), [self]() {
		pushCommand(self, [self]() { APP->scene->rack->pasteClipboardAction(); reloadRackAfterMutation(self); });
	});
	addCmd(self, edit, L("Save selection as…", "Salva selezione come…"), [self]() {
		pushCommand(self, []() { APP->scene->rack->saveSelectionDialog(); });
	});
	addCmd(self, edit, L("Reset selection", "Azzera selezione"), [self]() {
		pushCommand(self, []() { APP->scene->rack->resetSelectionAction(); });
	});
	addCmd(self, edit, L("Randomize selection", "Randomizza selezione"), [self]() {
		pushCommand(self, []() { APP->scene->rack->randomizeSelectionAction(); });
	}, nullptr, @"r");
	addCmd(self, edit, L("Disconnect selection", "Scollega selezione"), [self]() {
		pushCommand(self, []() { APP->scene->rack->disconnectSelectionAction(); });
	});
	addCmd(self, edit, L("Bypass selection", "Bypass selezione"), [self]() {
		pushCommand(self, []() { APP->scene->rack->bypassSelectionAction(!APP->scene->rack->isSelectionBypassed()); });
	}, []() { return APP->scene->rack->isSelectionBypassed(); });

	// ── View ────────────────────────────────────────────────────────────────────
	NSMenu* view = addSub(mainMenu, L("View", "Vista"));
	addCmd(self, view, L("Toggle accessible interface", "Mostra/nascondi interfaccia accessibile"), [self]() {
		setLayerVisible(self, !self->internal->visible);
	}, nullptr, @"a", NSEventModifierFlagCommand | NSEventModifierFlagShift);
	axSep(view);
	addCmd(self, view, L("Fullscreen", "Schermo intero"), [self]() {
		pushCommand(self, []() { APP->window->setFullScreen(!APP->window->isFullScreen()); });
	}, []() { return APP->window->isFullScreen(); });

	NSMenu* zoom = addSub(view, "Zoom");
	struct { const char* l; float v; } zooms[] = {
		{"25%", 0.25f}, {"50%", 0.5f}, {"100%", 1.0f}, {"200%", 2.0f}, {"400%", 4.0f}
	};
	for (auto& z : zooms) {
		float v = z.v;
		addCmd(self, zoom, z.l, [self, v]() {
			pushCommand(self, [v]() { APP->scene->rackScroll->setZoom(v); });
		}, [v]() { return std::abs(APP->scene->rackScroll->getZoom() - v) < 0.01f; });
	}
	addCmd(self, view, L("Fit to screen", "Adatta allo schermo"), [self]() {
		pushCommand(self, []() { APP->scene->rackScroll->zoomToModules(); });
	});

	NSMenu* theme = addSub(view, L("Interface theme", "Tema interfaccia"));
	struct { std::string l; const char* t; } themes[] = {
		{L("Dark", "Scuro"), "dark"}, {L("Light", "Chiaro"), "light"},
		{L("Dark high contrast", "Scuro alto contrasto"), "hcdark"}
	};
	for (auto& t : themes) {
		std::string tn = t.t;
		addCmd(self, theme, t.l, [tn]() {
			settings::uiTheme = tn;
		}, [tn]() { return settings::uiTheme == tn; });
	}

	NSMenu* pixel = addSub(view, L("Pixel ratio", "Rapporto pixel"));
	struct { const char* l; float v; } pixels[] = {
		{"Auto", 0.f}, {"100%", 1.f}, {"150%", 1.5f}, {"200%", 2.f}, {"250%", 2.5f}, {"300%", 3.f}
	};
	for (auto& p : pixels)
		fpreset(pixel, p.l, &settings::pixelRatio, p.v);

	NSMenu* wheel = addSub(view, L("Mouse wheel", "Rotellina del mouse"));
	addCmd(self, wheel, L("Scroll", "Scorri"), []() { settings::mouseWheelZoom = false; },
	       []() { return !settings::mouseWheelZoom; });
	addCmd(self, wheel, "Zoom", []() { settings::mouseWheelZoom = true; },
	       []() { return settings::mouseWheelZoom; });

	addCmd(self, view, L("Show tooltips", "Mostra tooltip"), []() { settings::tooltips ^= true; },
	       []() { return settings::tooltips; });

	NSMenu* opacity = addSub(view, L("Cable opacity", "Opacità cavi"));
	for (int p = 0; p <= 100; p += 25)
		fpreset(opacity, std::to_string(p) + "%", &settings::cableOpacity, p / 100.f);
	NSMenu* tension = addSub(view, L("Cable tension", "Tensione cavi"));
	for (int p = 0; p <= 100; p += 25)
		fpreset(tension, std::to_string(p) + "%", &settings::cableTension, p / 100.f);
	NSMenu* room = addSub(view, L("Room brightness", "Luminosità stanza"));
	for (int p = 50; p <= 200; p += 25)
		fpreset(room, std::to_string(p) + "%", &settings::rackBrightness, p / 100.f);
	NSMenu* halo = addSub(view, L("Light halo", "Bagliore luci"));
	for (int p = 0; p <= 100; p += 25)
		fpreset(halo, std::to_string(p) + "%", &settings::haloBrightness, p / 100.f);

	addCmd(self, view, L("Lock cursor", "Blocca cursore"), []() { settings::allowCursorLock ^= true; },
	       []() { return settings::allowCursorLock; });

	NSMenu* knob = addSub(view, L("Knob mode", "Modalità manopole"));
	struct { std::string l; int v; } knobModes[] = {
		{L("Linear", "Lineare"), settings::KNOB_MODE_LINEAR},
		{L("Rotary absolute", "Rotativa assoluta"), settings::KNOB_MODE_ROTARY_ABSOLUTE},
		{L("Rotary relative", "Rotativa relativa"), settings::KNOB_MODE_ROTARY_RELATIVE},
	};
	for (auto& k : knobModes) {
		int v = k.v;
		addCmd(self, knob, k.l, [v]() { settings::knobMode = (settings::KnobMode) v; },
		       [v]() { return (int) settings::knobMode == v; });
	}
	addCmd(self, view, L("Knob scroll", "Scorrimento manopole"), []() { settings::knobScroll ^= true; },
	       []() { return settings::knobScroll; });
	NSMenu* wheelSens = addSub(view, L("Wheel sensitivity", "Sensibilità rotellina"));
	struct { std::string l; float v; } senss[] = {
		{L("Low", "Bassa"), 0.0005f}, {L("Medium", "Media"), 0.001f},
		{L("High", "Alta"), 0.002f}
	};
	for (auto& s : senss)
		fpreset(wheelSens, s.l, &settings::knobScrollSensitivity, s.v);

	addCmd(self, view, L("Lock modules", "Blocca moduli"), []() { settings::lockModules ^= true; },
	       []() { return settings::lockModules; });
	addCmd(self, view, L("Squeeze modules", "Comprimi moduli"), []() { settings::squeezeModules ^= true; },
	       []() { return settings::squeezeModules; });
	addCmd(self, view, L("Prefer dark panels", "Preferisci pannelli scuri"), []() { settings::preferDarkPanels ^= true; },
	       []() { return settings::preferDarkPanels; });

	// ── Engine ──────────────────────────────────────────────────────────────────
	NSMenu* engine = addSub(mainMenu, L("Engine", "Motore"));
	addCmd(self, engine, L("CPU meter", "Indicatore CPU"), []() { settings::cpuMeter ^= true; },
	       []() { return settings::cpuMeter; });
	NSMenu* srate = addSub(engine, L("Sample rate", "Frequenza di campionamento"));
	addCmd(self, srate, "Auto", []() { settings::sampleRate = 0; }, []() { return settings::sampleRate == 0; });
	float rates[] = {44100.f, 48000.f, 88200.f, 96000.f, 176400.f, 192000.f};
	for (float r : rates) {
		addCmd(self, srate, string::f("%g kHz", r / 1000.f), [r]() { settings::sampleRate = r; },
		       [r]() { return settings::sampleRate == r; });
	}
	NSMenu* threads = addSub(engine, "Thread");
	int cores = system::getLogicalCoreCount() / 2;
	if (cores < 1)
		cores = 1;
	for (int i = 1; i <= 2 * cores; i++) {
		addCmd(self, threads, std::to_string(i), [i]() { settings::threadCount = i; },
		       [i]() { return settings::threadCount == i; });
	}

	// ── Library (rebuilt on open) ────────────────────────────────────────────────
	self->internal->libraryMenu = addSub(mainMenu, L("Library", "Libreria"));
	[self->internal->libraryMenu setDelegate:(id) self->internal->menuTarget];

	// ── Help ──────────────────────────────────────────────────────────────────────
	NSMenu* help = addSub(mainMenu, L("Help", "Aiuto"));
	NSMenu* lang = addSub(help, L("Language", "Lingua"));
	for (const std::string& language : string::getLanguages()) {
		std::string lcode = language;
		addCmd(self, lang, string::translate("language", lcode), [self, lcode]() {
			if (settings::language == lcode)
				return;
			settings::language = lcode;
			if (confirm(L("Restart now to apply the language?", "Riavviare ora per applicare la lingua?")))
				pushCommand(self, []() { APP->window->close(); settings::restart = true; });
		}, [lcode]() { return settings::language == lcode; });
	}
	addCmd(self, help, L("Tips", "Suggerimenti"), [self]() {
		pushCommand(self, []() { APP->scene->addChild(app::tipWindowCreate()); });
	});
	addCmd(self, help, L("Manual", "Manuale"), []() {
		system::openBrowser("https://vcvrack.com/manual");
	});
	addCmd(self, help, L("Support", "Supporto"), []() {
		system::openBrowser("https://vcvrack.com/support");
	});
	axSep(help);
	addCmd(self, help, L("User folder", "Cartella utente"), []() {
		system::openDirectory(asset::user(""));
	});
	addCmd(self, help, "Changelog", []() {
		system::openBrowser("https://github.com/VCVRack/Rack/blob/v2/CHANGELOG.md");
	});
	addCmd(self, help, L("Check for Rack updates", "Controlla aggiornamenti di Rack"), []() {
		std::thread([]() { library::checkAppUpdate(); }).detach();
	});

	[NSApp setMainMenu:mainMenu];
	[mainMenu release];
}

AccessibleWindow* AccessibleWindow::create(void* glfwWindow) {
	if (instance)
		return instance;

	GLFWwindow* win = (GLFWwindow*) glfwWindow;
	NSWindow* rackWindow = win ? glfwGetCocoaWindow(win) : nil;
	if (!rackWindow) {
		WARN("Accessible (macOS): could not resolve the Cocoa window");
		return nullptr;
	}

	AccessibleWindow* self = new AccessibleWindow();
	self->internal = new Internal();
	self->internal->rackWindow = rackWindow;

	NSRect frame = [rackWindow frame];
	CGFloat w = frame.size.width, h = frame.size.height;
	NSWindow* panel = [[NSWindow alloc]
	    initWithContentRect:NSMakeRect(0, 0, w, h)
	              styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
	                backing:NSBackingStoreBuffered
	                  defer:NO];
	[panel setTitle:@"Rack"];
	[panel setReleasedWhenClosed:NO];
	NSView* content = [panel contentView];
	[content setAutoresizesSubviews:YES];
	self->internal->panel = panel;

	RackAXController* controller = [[RackAXController alloc] init];
	controller->owner = self;
	self->internal->controller = controller; // retained (table holds only a weak ref)

	// Status line at the bottom; built without 10.12+ conveniences (10.9 target).
	NSTextField* status = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, w, 22)];
	[status setBezeled:NO];
	[status setDrawsBackground:NO];
	[status setEditable:NO];
	[status setSelectable:NO];
	[status setStringValue:@""];
	[status setAutoresizingMask:(NSViewWidthSizable | NSViewMaxYMargin)];
	[content addSubview:status];
	[status release];
	self->internal->statusLabel = status;

	// RACK table inside a scroll view, filling the area above the status line.
	NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 22, w, h - 22)];
	[scroll setHasVerticalScroller:YES];
	[scroll setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

	RackAXTableView* table = [[RackAXTableView alloc]
	    initWithFrame:NSMakeRect(0, 0, w, h - 22)];
	table->owner = self;
	NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"name"];
	[col setWidth:w > 80 ? w - 40 : 800];
	[col setEditable:NO];
	[table addTableColumn:col];
	[col release];
	[table setHeaderView:nil];
	[table setAllowsMultipleSelection:NO];
	[table setAllowsEmptySelection:YES];
	[table setColumnAutoresizingStyle:NSTableViewUniformColumnAutoresizingStyle];
	[table setDataSource:controller];
	[table setDelegate:controller];
	[scroll setDocumentView:table];
	[table release];
	[content addSubview:scroll];
	[scroll release];
	self->internal->rackTable = table;

	instance = self;

	// Native menu bar (App/File/Edit/View/Engine/Library/Help). Its key equivalents
	// provide the global shortcuts (⌘N/⌘S/⌘Z…) and the toggle (⇧⌘A), so no event
	// monitor is needed. GLFW left NSApp without a menu (GLFW_COCOA_MENUBAR = FALSE).
	AXMenuTarget* menuTarget = [[AXMenuTarget alloc] init];
	menuTarget->owner = self;
	self->internal->menuTarget = menuTarget;
	buildMenuBar(self);

	INFO("Accessible (macOS) window created");
	return self;
}

AccessibleWindow::~AccessibleWindow() {
	if (internal) {
		[NSApp setMainMenu:nil];
		if (internal->panel) {
			[internal->panel orderOut:nil];
			[internal->panel release];
		}
		if (internal->controller)
			[(id) internal->controller release];
		if (internal->menuTarget)
			[(id) internal->menuTarget release];
		delete internal;
		internal = nullptr;
	}
	if (instance == this)
		instance = nullptr;
}

void AccessibleWindow::drainCommands() {
	if (!internal)
		return;
	std::vector<std::function<void()>> q;
	q.swap(internal->commandQueue);
	for (auto& fn : q)
		fn();
}

bool AccessibleWindow::isVisible() {
	return internal && internal->visible;
}

} // namespace accessible
} // namespace rack

#endif // ARCH_MAC
