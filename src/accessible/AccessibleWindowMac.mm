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
#include <app/ParamWidget.hpp>
#include <app/Switch.hpp>
#include <app/PortWidget.hpp>
#include <app/CableWidget.hpp>
#include <app/SvgPanel.hpp>
#include <app/LedDisplay.hpp>
#include <app/TipWindow.hpp>
#include <ui/MenuOverlay.hpp>
#include <widget/event.hpp>
#include <plugin/Model.hpp>
#include <plugin/Plugin.hpp>
#include <plugin.hpp>
#include <engine/Module.hpp>
#include <engine/ParamQuantity.hpp>
#include <engine/Port.hpp>
#include <engine/PortInfo.hpp>
#include <engine/Engine.hpp>
#include <app/common.hpp>
#include <ui/common.hpp>
#include <ui/Menu.hpp>
#include <ui/MenuItem.hpp>
#include <window/Svg.hpp>
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
#include <map>
#include <regex>
#include <vector>
#include <functional>
#include <string>
#include <thread>

using namespace rack;

// One node of the LIBRARY tree (NSOutlineView). A brand node has model == nullptr and
// a children array of model nodes; a model node carries its plugin::Model* and no
// children. Declared up here (interface + implementation) so the C++ key handlers in
// the first namespace block can read its @public ivars by pointer. The node owns its
// label and children and releases them in dealloc (the project is non-ARC).
@interface AXLibNode : NSObject {
@public
	NSString*           label;
	rack::plugin::Model* model;     // nil for brand nodes
	NSMutableArray*     children;   // AXLibNode* models; nil for model nodes
}
@end

@implementation AXLibNode
- (void)dealloc {
	[label release];
	[children release];
	[super dealloc];
}
@end

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
	// Position in the 2D spatial grid that mirrors the physical rack. Left/Right move
	// within a visual row (col), Up/Down between rows (visRow) — see navigateRack.
	int                visRow   = 0;
	int                col      = 0;
};

// One menu-bar command: the action to run and an optional checkmark-state getter.
// Indexed by an NSMenuItem's tag — the Cocoa analogue of the Win32 MenuCmd table.
struct AXMenuCmd {
	std::function<void()> action;
	std::function<bool()> checked;
};

// One row of the CONTEXT_MENU list. isSubmenu items push a new menu level on Enter
// (instead of returning to the previous view); the Cocoa analogue of the Win32
// ContextMenuItem.
struct AXContextItem {
	std::string           label;
	std::function<void()> action;
	bool                  isSubmenu;

	AXContextItem(std::string l, std::function<void()> a, bool sub = false)
		: label(std::move(l)), action(std::move(a)), isSubmenu(sub) {}
};

// One clickable on-panel display cell (LedDisplayChoice) collected by the D key.
struct AXDisplayCell {
	app::LedDisplayChoice* choice;
	std::string            label;
};

struct AccessibleWindow::Internal {
	NSWindow*    rackWindow  = nil;   // main Rack window; regains key when toggled off
	NSWindow*    panel       = nil;   // our accessible layer, a child window over Rack
	id           controller  = nil;   // RackAXController* (datasource/delegate), retained
	NSTableView*   rackTable     = nil; // RackAXTableView*, owned by the view hierarchy
	NSOutlineView* libraryOutline = nil; // RackAXOutlineView*, owned by the view hierarchy
	NSTableView*   paramTable    = nil; // RackAXParamTableView* (2 columns), owned by the hierarchy
	NSTextField*   statusLabel   = nil; // status line at the bottom of the panel
	bool           visible       = false;

	// Hosted in a DAW (see AccessibleWindow::create). drainTarget/drainTimer replace the
	// standalone's run loop: Window::step() calls drainCommands() every frame, but inside
	// a plugin there is no Rack loop at all, so a timer on the host's run loop does it.
	bool           pluginMode    = false;
	id             drainTarget   = nil;  // AXDrainTimerTarget*, retained
	NSTimer*       drainTimer    = nil;  // retained; invalidated in the destructor
	id             keyObserver   = nil;  // NSWindowDidBecomeKey token, retained
	id             keyMonitor    = nil;  // local key monitor token, retained (hosted only)

	// Menu row (hosted only). menuTree is the SAME tree the standalone gives to NSApp,
	// kept here as pure data and never shown: the row's buttons read their items from it.
	NSMenu*        menuTree      = nil;  // retained; owned here
	NSView*        menuRow       = nil;  // owned by the view hierarchy
	id             menuButtonTarget = nil; // AXMenuButtonTarget*, retained

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

	// 2D-navigation index over rackRows, rebuilt by refreshRackView. rackRows is laid out
	// by visual row (visRow) then column (col), each row contiguous, so rackRowStart[vr]
	// is the flat index of that row's first cell and rackRowLen[vr] its cell count. Lets
	// navigateRack jump Up/Down/Left/Right without rescanning the list.
	std::vector<int> rackRowStart;
	std::vector<int> rackRowLen;

	// PARAM view. paramRows maps each table row to a param id (params with an empty
	// name are skipped). lastParamModule guards the rebuild: the list is only
	// repopulated when the focused module changes, so revisits keep focus/scroll.
	std::vector<int> paramRows;
	engine::Module*  lastParamModule = nullptr;

	// Momentary pulse (the Cocoa replacement for the Win32 SetTimer release): a single
	// Space on a momentary button sets the param high now; drainCommands() drops it
	// back to rest once momentaryReleaseTime passes — long enough for the audio thread
	// to sample the rising edge. minValue is restored only after re-validating the
	// module against the engine (it may have been removed during the brief high window).
	engine::Module* momentaryModule     = nullptr;
	int             momentaryParamId    = -1;
	double          momentaryReleaseTime = 0.0;

	// OUTPUT / INPUT views. One 2-column NSTableView each (Port / Cable). Every port is
	// a row, so the row index is the port id — no row vector, status read live.
	NSTableView* outputTable = nil;  // RackAXPortTableView*, isOutput = YES
	NSTableView* inputTable  = nil;  // RackAXPortTableView*, isOutput = NO

	// Two-step cable connection: the first Enter on a port arms this; the second Enter
	// on a compatible port (any module) completes it. Reset when the patch mutates or
	// the armed module is deleted, so the captured pointer can't dangle.
	struct {
		int             type   = 0;        // engine::Port::OUTPUT / INPUT
		engine::Module* module = nullptr;
		int             portId = -1;
		bool            active = false;
	} pendingCable;

	// CONTEXT_MENU view. contextItems is the level currently shown; menuStack holds the
	// nested levels (level 0 = the menu the trigger opened). ownedRootMenu / ownedSubmenus
	// are detached Rack ui::Menus built to read a module's appendContextMenu(); freed by
	// cleanupContextMenu(). previousView is the view to return to when the menu closes.
	NSTableView*                            contextTable = nil;  // RackAXContextTableView*
	AXView                                  previousView = AX_RACK;
	std::vector<AXContextItem>              contextItems;
	std::vector<std::vector<AXContextItem>> menuStack;
	ui::Menu*                               ownedRootMenu = nullptr;
	std::vector<ui::Menu*>                  ownedSubmenus;

	// Display-cell navigation (D key). displayCells are the focused module's clickable
	// LedDisplayChoice cells; capturedOverlay is a Tier-A MenuOverlay pulled out of the
	// scene into the accessible list (removed on cleanup); learningCell is an active
	// Tier-B MIDI-learn target polled in drainCommands; lastDisplayCellRow restores focus
	// when re-opening the cell list after picking a Tier-A option.
	std::vector<AXDisplayCell> displayCells;
	ui::MenuOverlay*           capturedOverlay   = nullptr;
	app::LedDisplayChoice*     learningCell      = nullptr;
	std::string                learningLastText;
	int                        lastDisplayCellRow = 0;

	// LIBRARY tree (brand → models). Built lazily on first show, like the Win32
	// libraryLoaded flag; libraryRoots is an NSArray of AXLibNode* brands, retained.
	NSMutableArray* libraryRoots  = nil;
	bool            libraryLoaded = false;

	// Mutations queued from Cocoa event handlers, run from drainCommands().
	std::vector<std::function<void()>> commandQueue;
};

// Height of the panel's menu row (hosted only) and of the status line at the bottom.
// Everything between the two belongs to the view currently shown.
static const CGFloat kMenuRowH = 30;
static const CGFloat kStatusH  = 22;

// Height available to the list of the current view. Hosted, the menu row takes a strip off
// the top: the standalone has no row, because its menus live in the system menu bar.
static CGFloat contentH(AccessibleWindow* self, CGFloat h) {
	return h - kStatusH - (self->internal->pluginMode ? kMenuRowH : 0);
}


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

// Alphabetical order as a reader expects it, not as bytes happen to fall.
//
// std::string's operator< is ORDINAL: it compares byte by byte, so every uppercase letter
// sorts before every lowercase one ("Zebra" before "apple") and accented names land after all
// of ASCII. In the library that means brands like "cf", "impromptu" or "voxglitch" pile up
// after Z instead of sitting among the others.
//
// localizedStandardCompare: is the comparison the Finder uses: case- and accent-aware by the
// user's locale, and digit-aware, so "Filter 2" precedes "Filter 10" instead of following it.
// Twin of the Win32 wlessCI (CompareStringEx with LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS).
static bool lessCI(const std::string& a, const std::string& b) {
	return [nsstr(a) localizedStandardCompare:nsstr(b)] == NSOrderedAscending;
}

// Comparator form, for the brand map. Names that compare equal — "4ms" and "4MS" — collapse
// onto one key, which is exactly the brand merging the tree wants.
struct LessCI {
	bool operator()(const std::string& a, const std::string& b) const {
		return lessCI(a, b);
	}
};

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
		r.visRow = visRow;
		r.col = col;
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
		r.visRow = visRow;
		r.col = col;
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

	// Build the 2D-navigation index. rackRows is already ordered by visRow then col and
	// each visual row is contiguous, so the first row carrying a given visRow marks its
	// start and every row of that visRow adds to its length.
	in->rackRowStart.clear();
	in->rackRowLen.clear();
	for (int i = 0; i < (int) in->rackRows.size(); i++) {
		int vr = in->rackRows[i].visRow;
		if ((int) in->rackRowStart.size() <= vr) {
			in->rackRowStart.resize(vr + 1, i);
			in->rackRowLen.resize(vr + 1, 0);
		}
		in->rackRowLen[vr]++;
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
		// A delete is gated by an NSAlert (confirm()); when that app-modal alert closes,
		// AppKit hands key-window status back to the main window (the GLFW rackWindow),
		// not to our child panel. The panel then shows the selection but isn't key, so
		// keystrokes hit the GLFW window and get rejected with the system beep — until
		// the user leaves and re-enters the app. So re-assert the panel as key window
		// first, then (since reloadData destroyed the row the VoiceOver cursor sat on,
		// and selecting a row doesn't move the cursor) restore focus the same way
		// switchTo does: first responder + the selection-changed notification.
		if (in->currentView == AX_RACK) {
			[in->panel makeKeyWindow];
			[in->panel makeFirstResponder:in->rackTable];
			NSAccessibilityPostNotification(in->rackTable,
			    NSAccessibilitySelectedRowsChangedNotification);
		}
	}
}

// ── Library view ─────────────────────────────────────────────────────────────
// Build the LIBRARY tree from plugin::plugins: one brand node per plugin, one model
// node per visible model. Mirrors the Win32 refreshLibraryView (TreeView). The result
// (libraryRoots) is read back by the NSOutlineView datasource on the controller.
static void refreshLibraryView(AccessibleWindow* self) {
	AccessibleWindow::Internal* in = self->internal;

	// Group models by brand name, merging plugins that share the same brand. The map keeps
	// brands ordered by LessCI — alphabetically as a person reads, not by byte value.
	std::map<std::string, std::vector<plugin::Model*>, LessCI> byBrand;
	for (plugin::Plugin* plug : plugin::plugins) {
		if (!plug)
			continue;
		for (plugin::Model* model : plug->models) {
			if (!model || model->hidden)
				continue;
			byBrand[plug->getBrand()].push_back(model);
		}
	}

	NSMutableArray* roots = [[NSMutableArray alloc] init];
	for (auto& kv : byBrand) {
		// Sort models alphabetically within each brand.
		std::sort(kv.second.begin(), kv.second.end(), [](plugin::Model* a, plugin::Model* b) {
			return lessCI(a->name, b->name);
		});
		AXLibNode* brand = [[AXLibNode alloc] init];
		brand->label = [nsstr(kv.first) retain];
		brand->model = nullptr;
		brand->children = [[NSMutableArray alloc] init];
		for (plugin::Model* model : kv.second) {
			AXLibNode* node = [[AXLibNode alloc] init];
			node->label = [nsstr(model->name) retain];
			node->model = model;
			node->children = nil;
			[brand->children addObject:node];
			[node release];
		}
		[roots addObject:brand];
		[brand release];
	}

	if (in->libraryRoots)
		[in->libraryRoots release];
	in->libraryRoots = roots; // retained
	[in->libraryOutline reloadData];
}

// Pixel top-left of a grid cell, mirroring ModuleWidget::setGridPosition() — the same
// helper as the Win32 layer's gridToPixel. Lets a placed module land on whichever grid
// cell the activated free slot carries.
static math::Vec gridToPixel(int gridX, int gridY) {
	return math::Vec(gridX, gridY) * app::RACK_GRID_SIZE + app::RACK_OFFSET;
}

// Create a module from a model and drop it at a grid cell. Mirrors the Win32 placeModule:
// register with the engine BEFORE building the widget (the native browser's order — the
// engine must own the module or ~ModuleWidget aborts), then widget + default preset +
// an undoable ModuleAdd so Cmd+Z removes it.
static void placeModule(AccessibleWindow* self, plugin::Model* model, int gridX, int gridY) {
	if (!model || !APP || !APP->scene || !APP->scene->rack)
		return;

	engine::Module* m = model->createModule();
	if (!m)
		return;
	APP->engine->addModule(m);

	app::ModuleWidget* mw = model->createModuleWidget(m);
	if (!mw) {
		APP->engine->removeModule(m);
		delete m;
		return;
	}

	APP->scene->rack->setModulePosNearest(mw, gridToPixel(gridX, gridY));
	APP->scene->rack->addModule(mw);
	mw->loadTemplate();

	history::ModuleAdd* ha = new history::ModuleAdd;
	ha->setModule(mw);
	APP->history->push(ha);

	// Keep focus on the inserted module's row (not the new free slot) for confirmation.
	refreshRackView(self, mw);
	self->internal->rackDirty = false;
	setStatus(self, L("Module \"", "Modulo \"") + model->name + L("\" added.", "\" aggiunto."));
}

// ── Param view ───────────────────────────────────────────────────────────────
// How long a momentary button is held high before drainCommands() releases it.
static const double MOMENTARY_SEC = 0.08;

// Build the PARAM row list for currentModule: one row per param with a non-empty
// name. Rows store only the param id; the datasource reads name + value live from
// the ParamQuantity, so re-reads (after an edit) always reflect the engine.
static void refreshParamView(AccessibleWindow* self) {
	AccessibleWindow::Internal* in = self->internal;
	in->paramRows.clear();
	if (engine::Module* m = in->currentModule) {
		for (int i = 0; i < m->getNumParams(); i++) {
			engine::ParamQuantity* pq = m->getParamQuantity(i);
			if (!pq || pq->name.empty())
				continue;
			in->paramRows.push_back(i);
		}
	}
	[in->paramTable reloadData];
}

// ── Port views ───────────────────────────────────────────────────────────────
// Cable status of one port: "free", "→ RemoteModule", or "connected" if the remote
// has no model. Reaches the live PortWidget to query its cables. Mirrors the status
// column the Win32 refreshPortView builds. Only the first cable is reported (parity
// with Win32; an output can carry several).
static std::string portSvgId(app::ModuleWidget* mw, app::PortWidget* pw); // fwd

// Human-readable name of the port at a cable's remote end: the module's port name, or the
// panel SVG placement id when the port has no name of its own. Mirrors remotePortName() in
// the Win32 AccessibleWindow.cpp.
static std::string remotePortName(app::PortWidget* remote) {
	if (!remote || !remote->module || !APP || !APP->scene || !APP->scene->rack)
		return "";
	engine::Module* m = remote->module;
	bool isOut = (remote->type == engine::Port::OUTPUT);
	engine::PortInfo* info = isOut ? m->getOutputInfo(remote->portId)
	                              : m->getInputInfo(remote->portId);
	std::string name = info ? info->getName() : "";
	if (info && info->name.empty()) {
		app::ModuleWidget* rmw = APP->scene->rack->getModule(m->id);
		std::string svgId = portSvgId(rmw, remote);
		if (!svgId.empty())
			name = svgId;
	}
	return name;
}

static std::string portStatusString(engine::Module* mod, bool isOutput, int portId) {
	std::string status = L("free", "libero");
	if (!mod || !APP || !APP->scene || !APP->scene->rack)
		return status;
	app::ModuleWidget* mw = APP->scene->rack->getModule(mod->id);
	if (!mw)
		return status;
	app::PortWidget* pw = isOutput ? mw->getOutput(portId) : mw->getInput(portId);
	if (!pw)
		return status;
	auto cables = APP->scene->rack->getCompleteCablesOnPort(pw);
	if (cables.empty())
		return status;
	app::CableWidget* cw = cables[0];
	app::PortWidget* remote = isOutput ? cw->inputPort : cw->outputPort;
	if (remote && remote->module && remote->module->model) {
		status = "→ " + remote->module->model->name;
		// Append the remote port's own name so the user hears both the module and WHICH of
		// its ports the cable reaches (e.g. "→ VCF, cutoff CV"). Comma, not colon: it reads
		// better through the speech synth.
		std::string rpn = remotePortName(remote);
		if (!rpn.empty())
			status += ", " + rpn;
	}
	else if (remote)
		status = L("connected", "connesso");
	return status;
}

// Rebuild a port list. Rows are read live from currentModule, so this is just a
// reload; the caller handles focus.
static void refreshPortView(AccessibleWindow* self, bool isOutput) {
	NSTableView* t = isOutput ? self->internal->outputTable : self->internal->inputTable;
	[t reloadData];
}

static std::string axViewName(AXView v) {
	switch (v) {
		case AX_RACK:    return L("Rack", "Rack");
		case AX_LIBRARY: return L("Library", "Libreria");
		case AX_PARAM:   return L("Parameters", "Parametri");
		case AX_OUTPUT:  return L("Outputs", "Uscite");
		case AX_INPUT:   return L("Inputs", "Ingressi");
		default:         return L("View", "Vista");
	}
}

// Hide every view's scroll view; the caller then unhides the one it shows. Keeping
// this in one place means each new view only has to reveal itself.
static void hideAllViews(AccessibleWindow::Internal* in) {
	[[in->rackTable enclosingScrollView] setHidden:YES];
	[[in->libraryOutline enclosingScrollView] setHidden:YES];
	[[in->paramTable enclosingScrollView] setHidden:YES];
	[[in->outputTable enclosingScrollView] setHidden:YES];
	[[in->inputTable enclosingScrollView] setHidden:YES];
	[[in->contextTable enclosingScrollView] setHidden:YES];
}

// The control that backs a view — the one that must hold first responder whenever the
// panel has key. Twin of the Win32 layer's activeControl().
static NSView* activeControl(AccessibleWindow::Internal* in) {
	switch (in->currentView) {
		case AX_RACK:         return in->rackTable;
		case AX_LIBRARY:      return in->libraryOutline;
		case AX_PARAM:        return in->paramTable;
		case AX_OUTPUT:       return in->outputTable;
		case AX_INPUT:        return in->inputTable;
		case AX_CONTEXT_MENU: return in->contextTable;
	}
	return in->rackTable;
}

// Put first responder back on the active view's control. Called whenever the panel becomes
// the key window: a window that has key but whose first responder is the window itself
// sends its key events up to NSApp, and inside a DAW that means the host's own key handling
// swallows them. Standalone this never bites, because nothing else in the process competes
// for the keyboard.
static void focusActiveControl(AccessibleWindow* self) {
	AccessibleWindow::Internal* in = self->internal;
	NSView* control = activeControl(in);
	if (!control || !in->panel)
		return;
	if ([in->panel firstResponder] != (NSResponder*) control)
		[in->panel makeFirstResponder:control];
}

// Show a view's control and focus it. RACK, LIBRARY and PARAM are wired; OUTPUT/INPUT
// and CONTEXT_MENU still announce a placeholder until their phase lands.
static void switchTo(AccessibleWindow* self, AXView v) {
	AccessibleWindow::Internal* in = self->internal;
	if (v == AX_RACK) {
		in->currentView = AX_RACK;
		hideAllViews(in);
		[[in->rackTable enclosingScrollView] setHidden:NO];
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
	if (v == AX_LIBRARY) {
		in->currentView = AX_LIBRARY;
		hideAllViews(in);
		[[in->libraryOutline enclosingScrollView] setHidden:NO];
		if (!in->libraryLoaded) {
			refreshLibraryView(self);
			in->libraryLoaded = true;
		}
		[in->panel makeFirstResponder:in->libraryOutline];
		if ([in->libraryOutline selectedRow] < 0 && [in->libraryOutline numberOfRows] > 0)
			[in->libraryOutline selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
			               byExtendingSelection:NO];
		NSAccessibilityPostNotification(in->libraryOutline,
		    NSAccessibilitySelectedRowsChangedNotification);
		return;
	}
	if (v == AX_PARAM) {
		in->currentView = AX_PARAM;
		hideAllViews(in);
		[[in->paramTable enclosingScrollView] setHidden:NO];
		// Rebuild only when the target module changed, so revisits keep focus/scroll.
		if (in->currentModule != in->lastParamModule) {
			refreshParamView(self);
			in->lastParamModule = in->currentModule;
		}
		[in->panel makeFirstResponder:in->paramTable];
		if ([in->paramTable selectedRow] < 0 && !in->paramRows.empty())
			[in->paramTable selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
			           byExtendingSelection:NO];
		// Announce the view name so Tab-cycling between PARAM/OUTPUT/INPUT keeps the user
		// oriented. VoiceOver pronounces the selection-changed read before any announcement
		// posted around it, so the row is read first and the view name follows. Trying to
		// reorder them (deferring the read) makes the announcement get dropped during fast
		// cycling, so we accept hearing it after the row read — at least it's guaranteed.
		NSAccessibilityPostNotification(in->paramTable,
		    NSAccessibilitySelectedRowsChangedNotification);
		announce(self, axViewName(v));
		return;
	}
	if (v == AX_OUTPUT || v == AX_INPUT) {
		bool isOutput = (v == AX_OUTPUT);
		in->currentView = v;
		hideAllViews(in);
		NSTableView* t = isOutput ? in->outputTable : in->inputTable;
		[[t enclosingScrollView] setHidden:NO];
		// Always rebuild: cable state may have changed in the GUI or from a connection.
		refreshPortView(self, isOutput);
		[in->panel makeFirstResponder:t];
		if ([t selectedRow] < 0 && [t numberOfRows] > 0)
			[t selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
		// Announce the view name so Tab-cycling between PARAM/OUTPUT/INPUT keeps the user
		// oriented. VoiceOver pronounces the selection-changed read before any announcement
		// posted around it, so the row is read first and the view name follows. Trying to
		// reorder them (deferring the read) makes the announcement get dropped during fast
		// cycling, so we accept hearing it after the row read — at least it's guaranteed.
		NSAccessibilityPostNotification(t, NSAccessibilitySelectedRowsChangedNotification);
		announce(self, axViewName(v));
		return;
	}
	if (v == AX_CONTEXT_MENU) {
		// contextItems is already populated by showContextMenu() before this call.
		in->currentView = AX_CONTEXT_MENU;
		hideAllViews(in);
		[[in->contextTable enclosingScrollView] setHidden:NO];
		[in->contextTable reloadData];
		[in->panel makeFirstResponder:in->contextTable];
		if ([in->contextTable numberOfRows] > 0)
			[in->contextTable selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
			              byExtendingSelection:NO];
		NSAccessibilityPostNotification(in->contextTable,
		    NSAccessibilitySelectedRowsChangedNotification);
		return;
	}
	announce(self, axViewName(v) + L(": not yet implemented", ": non ancora implementato"));
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

// Modal single-line text prompt (the Win32 showInputDialog equivalent). Returns the
// entered text, or "" if cancelled. The accessory text field is the initial first
// responder so VoiceOver lands in it and the user can type immediately.
static std::string showInputDialog(const std::string& title, const std::string& prompt,
                                   const std::string& initial) {
	NSAlert* a = [[NSAlert alloc] init];
	[a setMessageText:nsstr(title)];
	[a setInformativeText:nsstr(prompt)];
	[a addButtonWithTitle:nsstr(L("OK", "OK"))];
	[a addButtonWithTitle:nsstr(L("Cancel", "Annulla"))];

	NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
	[input setStringValue:nsstr(initial)];
	[a setAccessoryView:input];
	[[a window] setInitialFirstResponder:input];

	NSModalResponse r = [a runModal];
	std::string result;
	if (r == NSAlertFirstButtonReturn) {
		const char* s = [[input stringValue] UTF8String];
		result = s ? s : "";
	}
	[input release];
	[a release];
	return result;
}

// ── RACK key handlers ────────────────────────────────────────────────────────
static AXRow* focusedRackRow(AccessibleWindow* self) {
	NSInteger row = [self->internal->rackTable selectedRow];
	if (row < 0 || row >= (NSInteger) self->internal->rackRows.size())
		return nullptr;
	return &self->internal->rackRows[row];
}

// 2D arrow navigation over the flat rack table, mirroring the Win32 icon view: Left/Right
// (dx ±1) move within a visual row, Up/Down (dy ±1) between rows. NSTableView is a 1D list,
// so we map the move through the rackRowStart/rackRowLen index and reselect the target row,
// then re-post the selection so VoiceOver reads it. Up/Down clamp the column to the target
// row's width (rows can differ in length); Left/Right stay put at a row edge.
static void navigateRack(AccessibleWindow* self, int dx, int dy) {
	AccessibleWindow::Internal* in = self->internal;
	NSInteger sel = [in->rackTable selectedRow];
	if (sel < 0 || sel >= (NSInteger) in->rackRows.size())
		return;
	int nRows = (int) in->rackRowStart.size();
	if (nRows == 0)
		return;

	int tr = in->rackRows[sel].visRow + dy;
	int tc = in->rackRows[sel].col + dx;
	if (tr < 0 || tr >= nRows)
		return;                       // no row above/below: stay where we are
	int len = in->rackRowLen[tr];
	if (dy != 0) {                    // Up/Down: keep the column but clamp to the row's width
		if (tc > len - 1)
			tc = len - 1;
		if (tc < 0)
			tc = 0;
	}
	else if (tc < 0 || tc >= len) {   // Left/Right: don't wrap past the row's edges
		return;
	}

	int target = in->rackRowStart[tr] + tc;
	[in->rackTable selectRowIndexes:[NSIndexSet indexSetWithIndex:target] byExtendingSelection:NO];
	[in->rackTable scrollRowToVisible:target];
	NSAccessibilityPostNotification(in->rackTable, NSAccessibilitySelectedRowsChangedNotification);
}

// Ctrl+Enter: drop the focused module onto a brand-new row, one grid row below the lowest
// existing module (left edge). Keyboard counterpart of dragging a module past the bottom
// row in the GUI — this is how the user grows the patch into multiple rows. Undoable via
// history::ModuleMove. Ported from the Win32 moveFocusedModuleToNewRow.
static void moveFocusedModuleToNewRow(AccessibleWindow* self) {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;
	AXRow* r = focusedRackRow(self);
	if (!r)
		return;
	if (r->freeSlot) {
		setStatus(self, L("Free slot: no module to move.", "Slot libero: nessun modulo da spostare."));
		return;
	}
	app::ModuleWidget* mw = r->mw;

	// Defer the widget-tree mutation to the safe drain point (see drainCommands).
	pushCommand(self, [self, mw]() {
		app::RackWidget* rack = APP->scene->rack;

		// New row = one grid row below the lowest existing module.
		bool any = false;
		int  maxGy = 0;
		for (app::ModuleWidget* m : rack->getModules()) {
			int gy = (int) m->getGridPosition().y;
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

		refreshRackView(self, mw);
		self->internal->rackDirty = false;
		setStatus(self, L("Module moved to a new row.", "Modulo spostato su una nuova fila."));
	});
}

static void onRackEnter(AccessibleWindow* self) {
	AXRow* r = focusedRackRow(self);
	if (!r)
		return;
	if (r->freeSlot) {
		// Place a queued model on this row's free slot, otherwise open the library —
		// mirroring the GUI's double-click-on-empty-slot behaviour.
		if (self->internal->selectedModel) {
			plugin::Model* model = self->internal->selectedModel;
			self->internal->selectedModel = nullptr;
			// Capture the slot's grid cell by value so a later list rebuild can't move it.
			int gx = r->gridX, gy = r->gridY;
			// Defer: placeModule mutates the widget tree (unsafe from a Cocoa event
			// handler); drainCommands() runs it from the main loop.
			pushCommand(self, [self, model, gx, gy]() {
				placeModule(self, model, gx, gy);
			});
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

// Library Enter: a brand node toggles expand/collapse; a model node is queued as the
// pending selection and returns to RACK, where Enter on a free slot drops it. Mirrors
// the Win32 handleLibraryEnter.
static void onLibraryEnter(AccessibleWindow* self) {
	NSOutlineView* ov = self->internal->libraryOutline;
	NSInteger row = [ov selectedRow];
	if (row < 0)
		return;
	AXLibNode* node = [ov itemAtRow:row];
	if (!node)
		return;
	if (!node->model) {
		if ([ov isItemExpanded:node])
			[ov collapseItem:node];
		else
			[ov expandItem:node];
		return;
	}
	self->internal->selectedModel = node->model;
	switchTo(self, AX_RACK);
	setStatus(self, "\"" + node->model->name + "\" "
	          + L("selected — go to [ Free slot ] and press Enter.",
	              "selezionato — vai su [ Slot libero ] e premi Invio."));
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
				self->internal->lastParamModule = nullptr;
				self->internal->pendingCable.active = false;
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
			if (self->internal->lastParamModule == mod)
				self->internal->lastParamModule = nullptr;
			if (self->internal->pendingCable.module == mod)
				self->internal->pendingCable.active = false;
			refreshRackView(self, nullptr, rowi - 1);
			self->internal->rackDirty = false;
			setStatus(self, L("Module \"", "Modulo \"") + sname + L("\" removed.", "\" rimosso."));
		});
	}
}

// Jump straight to one of the three module-detail views (F2 / F3 / F4).
//
// From RACK it opens the focused module; from a detail view it re-targets the module
// already open, so the three keys double as a direct jump between the lists without walking
// the Tab cycle. Mirrors the Win32 switchToDetailView.
static void switchToDetailView(AccessibleWindow* self, AXView v) {
	AccessibleWindow::Internal* in = self->internal;
	if (!APP || !APP->scene || !APP->scene->rack)
		return;
	if (in->currentView == AX_RACK) {
		AXRow* r = focusedRackRow(self);
		if (!r || r->freeSlot)   // free slot: no module to open
			return;
		in->currentModule = r->mw->module;
	}
	else if (in->currentView != AX_PARAM && in->currentView != AX_OUTPUT
	         && in->currentView != AX_INPUT) {
		return;
	}
	if (!in->currentModule)
		return;
	switchTo(self, v);
}

// ── PARAM key handlers ───────────────────────────────────────────────────────
// The ParamQuantity behind the focused row, or nullptr. Fills *outRow with the row.
static engine::ParamQuantity* focusedParam(AccessibleWindow* self, int* outRow) {
	AccessibleWindow::Internal* in = self->internal;
	NSInteger row = [in->paramTable selectedRow];
	if (row < 0 || row >= (NSInteger) in->paramRows.size() || !in->currentModule)
		return nullptr;
	if (outRow)
		*outRow = (int) row;
	return in->currentModule->getParamQuantity(in->paramRows[row]);
}

// True if the param's on-screen widget is a momentary Switch. The momentary flag
// lives on app::Switch, not the ParamQuantity, so we reach the live widget. Mirrors
// the Win32 isMomentaryParam.
static bool isMomentaryParam(AccessibleWindow* self, int paramId) {
	AccessibleWindow::Internal* in = self->internal;
	if (!in->currentModule || !APP || !APP->scene || !APP->scene->rack)
		return false;
	app::ModuleWidget* mw = APP->scene->rack->getModule(in->currentModule->id);
	if (!mw)
		return false;
	auto* sw = dynamic_cast<app::Switch*>(mw->getParam(paramId));
	return sw && sw->momentary;
}

// One PARAM action. 'L'/'R' step left/right (cmd = fine, shift = coarse, both =
// very fine), 'B' resets to default, 'V' opens the value dialog, 'S' (Space) toggles
// a snap switch / pulses a momentary one. Param edits don't touch the widget tree or
// GL, so — like the Win32 handler — they run synchronously here, not via the queue.
static void onParamKey(AccessibleWindow* self, char which, bool cmd, bool shift) {
	int row = -1;
	engine::ParamQuantity* pq = focusedParam(self, &row);
	if (!pq)
		return;

	if (which == 'B') {                       // Backspace → reset to default
		pq->reset();
	}
	else if (which == 'V') {                  // V / Enter → value dialog
		std::string prompt = L("Value for «", "Valore per «") + pq->name
		                     + L("»:\n(e.g.: 440, C4, log2(8), dbtogain(-6))",
		                         "»:\n(Es: 440, C4, log2(8), dbtogain(-6))");
		std::string text = showInputDialog(L("Set value", "Imposta valore"), prompt,
		                                   pq->getDisplayValueString());
		if (!text.empty())
			pq->setDisplayValueString(text);

		// showInputDialog runs an app-modal NSAlert; when it closes, AppKit hands
		// key-window status back to the GLFW rackWindow, not our child panel — the
		// same issue fixed in rebuildRackView after the delete confirm(). Without
		// this, the VoiceOver cursor stays in the main Rack window and subsequent
		// keystrokes beep. Re-assert the panel as key window, restore first
		// responder, refresh the value cell, and move the VoiceOver cursor back to
		// the param row (covers both the value-set and cancel paths).
		AccessibleWindow::Internal* in = self->internal;
		[in->panel makeKeyWindow];
		[in->panel makeFirstResponder:in->paramTable];
		[in->paramTable reloadDataForRowIndexes:[NSIndexSet indexSetWithIndex:row]
		                          columnIndexes:[NSIndexSet indexSetWithIndex:1]];
		NSAccessibilityPostNotification(in->paramTable,
		    NSAccessibilitySelectedRowsChangedNotification);
		announce(self, pq->getDisplayValueString() + pq->getUnit());
		return;
	}
	else if (which == 'S') {                  // Space → toggle switch / pulse momentary
		if (!pq->snapEnabled)                 // only meaningful for switches
			return;
		int paramId = self->internal->paramRows[row];
		if (isMomentaryParam(self, paramId)) {
			pq->setValue(pq->maxValue);
			self->internal->momentaryModule = self->internal->currentModule;
			self->internal->momentaryParamId = paramId;
			self->internal->momentaryReleaseTime = system::getTime() + MOMENTARY_SEC;
		}
		else {
			float next = std::round(pq->getValue()) + 1.f;
			if (next > pq->maxValue)
				next = pq->minValue;
			pq->setValue(next);
		}
	}
	else {                                    // Left / Right → step
		float cur  = pq->getValue();
		float step = (pq->maxValue - pq->minValue) / 100.f;
		if (cmd && shift)
			step *= (1.f / 100.f);            // very fine
		else if (cmd)
			step *= (1.f / 10.f);             // fine
		else if (shift)
			step *= 4.f;                      // coarse
		float next = (which == 'R') ? cur + step : cur - step;
		if (pq->snapEnabled)
			next = std::round(next);
		next = math::clamp(next, pq->minValue, pq->maxValue);
		pq->setValue(next);
	}

	// Re-read just this row's value cell and speak the new value. These are discrete
	// key presses (not a periodic refresh), so the announcement won't drown the audio.
	[self->internal->paramTable reloadDataForRowIndexes:[NSIndexSet indexSetWithIndex:row]
	                                      columnIndexes:[NSIndexSet indexSetWithIndex:1]];
	announce(self, pq->getDisplayValueString() + pq->getUnit());
}

// ── PORT key handlers ────────────────────────────────────────────────────────
// Cancel an armed connection if one is pending; returns true if it did. Called from
// every view's Escape so the user can always back out of a two-step connection.
static bool cancelPendingCable(AccessibleWindow* self) {
	if (!self->internal->pendingCable.active)
		return false;
	self->internal->pendingCable.active = false;
	setStatus(self, L("Connection cancelled.", "Connessione annullata."));
	return true;
}

// Focus the row for portId (row index == portId) and let VoiceOver read it. Used
// after a connect/disconnect to confirm the new state.
static void focusPortRow(AccessibleWindow* self, bool isOutput, int portId) {
	NSTableView* t = isOutput ? self->internal->outputTable : self->internal->inputTable;
	if (portId >= 0 && portId < [t numberOfRows]) {
		[t selectRowIndexes:[NSIndexSet indexSetWithIndex:portId] byExtendingSelection:NO];
		[t scrollRowToVisible:portId];
	}
}

// Recover a semantic name for a port the module never named.
//
// Some modules only call config(...) and never configInput()/configOutput(), so PortInfo::name
// stays empty and getName() falls back to a bare "#N" — useless to a screen reader. The panel
// labels are vector outlines, not machine-readable text. But many plugins place their ports
// with the SvgHelper convention panelHelper.findNamed("position_input"), so the panel SVG
// contains a shape whose id IS the port's name and whose bounds-center is exactly the port's
// center. We recover that id at runtime by matching the PortWidget's center against the
// panel's NanoSVG shapes.
//
// Returns the raw id (per the chosen design — no prettifying), or "" when there is no
// confidently-semantic match, in which case the caller keeps the "#N" placeholder. Mirrors
// portSvgId() in the Win32 AccessibleWindow.cpp.
static std::string portSvgId(app::ModuleWidget* mw, app::PortWidget* pw) {
	if (!mw || !pw)
		return "";
	// getPanel() returns the base widget; both SvgPanel and ThemedSvgPanel keep the
	// currently-shown document in SvgPanel::svg, so a base-class cast is enough.
	auto* panel = dynamic_cast<app::SvgPanel*>(mw->getPanel());
	if (!panel || !panel->svg || !panel->svg->handle)
		return "";

	// The port's center (in ModuleWidget coords) coincides with the placement shape's
	// bounds-center (in SVG coords) 1:1, because that is precisely how
	// createInputCentered(findNamed(...)) positioned it. So the nearest shape within a hair's
	// tolerance is the placement marker.
	math::Vec c = pw->box.getCenter();
	NSVGshape* best = NULL;
	float bestDist = 4.0f; // squared px; the match is essentially exact
	for (NSVGshape* sh = panel->svg->handle->shapes; sh; sh = sh->next) {
		if (!sh->id[0])
			continue;
		float cx = (sh->bounds[0] + sh->bounds[2]) * 0.5f;
		float cy = (sh->bounds[1] + sh->bounds[3]) * 0.5f;
		float dx = cx - c.x, dy = cy - c.y;
		float d = dx * dx + dy * dy;
		if (d < bestDist) {
			bestDist = d;
			best = sh;
		}
	}
	if (!best)
		return "";

	std::string id = best->id;
	// Reject auto-generated editor ids (Inkscape/Illustrator): path1234, rect5, g12, tspan3,
	// circle2, … They carry no meaning, so per the chosen design we fall back to "#N" rather
	// than read them aloud.
	static const std::regex autoId(
	    "^(path|rect|g|tspan|text|use|circle|ellipse|line|polyline|polygon|xml|svg)[-_]?[0-9]+$",
	    std::regex::icase);
	if (std::regex_match(id, autoId))
		return "";
	return id;
}


// Enter on a port: a two-step connection. The first Enter arms pendingCable and drops
// back to RACK so the user can navigate to the other module's port; the second Enter
// on a compatible port completes it. Mirrors the Win32 handlePortEnter — in particular
// the cable is built the native way (set both PortWidgets + updateCable + addCable) so
// onAdd() registers the plug widgets; hand-rolling the engine::Cable skips the plugs and
// the cable later asserts when its module is removed.
static void onPortEnter(AccessibleWindow* self, bool isOutput) {
	AccessibleWindow::Internal* in = self->internal;
	NSTableView* t = isOutput ? in->outputTable : in->inputTable;
	NSInteger row = [t selectedRow];
	if (row < 0 || !in->currentModule || !APP || !APP->engine || !APP->scene || !APP->scene->rack)
		return;

	int portId   = (int) row;   // row index == port id
	int portType = isOutput ? engine::Port::OUTPUT : engine::Port::INPUT;

	if (!in->pendingCable.active) {
		in->pendingCable.type   = portType;
		in->pendingCable.module = in->currentModule;
		in->pendingCable.portId = portId;
		in->pendingCable.active = true;
		engine::PortInfo* info = isOutput ? in->currentModule->getOutputInfo(portId)
		                                  : in->currentModule->getInputInfo(portId);
		std::string portName = info ? info->getName() : "";
		// Match the semantic-name fallback used by the port list (see the OUTPUT/INPUT
		// datasource) so the status message reads the same expressive name.
		if (info && info->name.empty() && APP && APP->scene && APP->scene->rack) {
			app::ModuleWidget* pmw = APP->scene->rack->getModule(in->currentModule->id);
			app::PortWidget* ppw = pmw ? (isOutput ? pmw->getOutput(portId) : pmw->getInput(portId)) : NULL;
			std::string svgId = portSvgId(pmw, ppw);
			if (!svgId.empty())
				portName = svgId;
		}
		std::string modName  = in->currentModule->model ? in->currentModule->model->name : "?";
		setStatus(self, L("Connecting from \"", "Connessione da \"") + portName
		          + L("\" on ", "\" di ") + modName
		          + L(" started. Select destination port (Esc to cancel).",
		              " avviata. Seleziona porta di destinazione (Esc per annullare)."));
		switchTo(self, AX_RACK);
		return;
	}

	// Complete: the two ports must be of opposite type.
	if (in->pendingCable.type == portType) {
		setStatus(self, L("Incompatible port: an output must connect to an input.",
		                  "Porta incompatibile: un output deve collegarsi a un input."));
		return;
	}

	engine::Module* outMod; int outId;
	engine::Module* inMod;  int inId;
	if (in->pendingCable.type == engine::Port::OUTPUT) {
		outMod = in->pendingCable.module; outId = in->pendingCable.portId;
		inMod  = in->currentModule;       inId  = portId;
	}
	else {
		inMod  = in->pendingCable.module; inId  = in->pendingCable.portId;
		outMod = in->currentModule;       outId = portId;
	}
	in->pendingCable.active = false;

	std::string outName = outMod->model ? outMod->model->name : "?";
	std::string inName  = inMod->model  ? inMod->model->name  : "?";
	setStatus(self, L("Connected: ", "Connesso: ") + outName + " → " + inName + ".");
	switchTo(self, AX_RACK);

	// Defer the widget-tree mutation to the safe drain point.
	pushCommand(self, [outMod, outId, inMod, inId]() {
		app::RackWidget* rack = APP->scene->rack;
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
		cw->updateCable();   // creates the engine cable from the two ports
		rack->addCable(cw);  // onAdd() registers the plug widgets
	});
}

// Delete/Backspace on a port: remove every cable on it as one undoable action.
static void onPortDelete(AccessibleWindow* self, bool isOutput) {
	AccessibleWindow::Internal* in = self->internal;
	NSTableView* t = isOutput ? in->outputTable : in->inputTable;
	NSInteger row = [t selectedRow];
	if (row < 0 || !in->currentModule || !APP || !APP->scene || !APP->scene->rack)
		return;

	int portId = (int) row;
	engine::Module* mod = in->currentModule;

	pushCommand(self, [self, mod, portId, isOutput]() {
		app::RackWidget* rack = APP->scene->rack;
		app::ModuleWidget* mw = rack->getModule(mod->id);
		if (!mw)
			return;
		app::PortWidget* pw = isOutput ? mw->getOutput(portId) : mw->getInput(portId);
		if (!pw)
			return;

		auto cables = rack->getCompleteCablesOnPort(pw);
		if (cables.empty()) {
			setStatus(self, L("No cable to disconnect on this port.",
			                  "Nessun cavo da scollegare su questa porta."));
		}
		else {
			history::ComplexAction* h = new history::ComplexAction;
			h->name = L("disconnect cable", "scollega cavo");
			for (app::CableWidget* cw : cables) {
				history::CableRemove* hr = new history::CableRemove;
				hr->setCable(cw);
				h->push(hr);
				rack->removeCable(cw);
				delete cw;
			}
			APP->history->push(h);
			setStatus(self, L("Cable disconnected.", "Cavo scollegato."));
		}

		refreshPortView(self, isOutput);
		focusPortRow(self, isOutput, portId);
	});
}

// ── CONTEXT_MENU view ────────────────────────────────────────────────────────
// Free the detached Rack menus built to read a module's appendContextMenu(), and clear
// the navigation stack. (Display-cell / overlay capture is Phase 7.)
static void cleanupContextMenu(AccessibleWindow* self) {
	AccessibleWindow::Internal* in = self->internal;
	for (ui::Menu* sub : in->ownedSubmenus)
		delete sub;
	in->ownedSubmenus.clear();
	if (in->capturedOverlay) {
		if (APP && APP->scene)
			APP->scene->removeChild(in->capturedOverlay);
		delete in->capturedOverlay;
		in->capturedOverlay = nullptr;
	}
	if (in->ownedRootMenu) {
		delete in->ownedRootMenu;
		in->ownedRootMenu = nullptr;
	}
	in->menuStack.clear();
	in->displayCells.clear();
	in->learningCell = nullptr;
	in->learningLastText.clear();
	in->lastDisplayCellRow = 0;
}

// Show a freshly built menu level as the CONTEXT_MENU view, remembering the view to
// return to. Mirrors the Win32 showContextMenu.
static void showContextMenu(AccessibleWindow* self, std::vector<AXContextItem> items) {
	self->internal->previousView = self->internal->currentView;
	self->internal->contextItems = std::move(items);
	switchTo(self, AX_CONTEXT_MENU);
}

// Reload the context list with the current level and focus row 0 (used after a submenu
// push/pop without leaving CONTEXT_MENU).
static void reloadContextLevel(AccessibleWindow* self) {
	AccessibleWindow::Internal* in = self->internal;
	[in->contextTable reloadData];
	if (!in->contextItems.empty())
		[in->contextTable selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
		             byExtendingSelection:NO];
	NSAccessibilityPostNotification(in->contextTable,
	    NSAccessibilitySelectedRowsChangedNotification);
}

// Convert a Rack ui::Menu into context items. Leaf items call doAction(false) then
// clean up; submenu items push a new level lazily (createChildMenu is called when the
// user enters the submenu, not when the parent is built). Mirrors buildItemsFromMenu.
static std::vector<AXContextItem> buildItemsFromMenu(AccessibleWindow* self, ui::Menu* menu) {
	std::vector<AXContextItem> items;
	for (widget::Widget* w : menu->children) {
		auto* mi = dynamic_cast<ui::MenuItem*>(w);
		if (!mi)
			continue;

		std::string label = mi->text;
		if (!mi->rightText.empty()) {
			if (mi->rightText.find(CHECKMARK_STRING) != std::string::npos)
				label += " ✓";          // ✓
			else if (mi->rightText.find(RIGHT_ARROW) != std::string::npos)
				label += " ▸";          // ▸
			else
				label += "  " + mi->rightText;
		}

		if (mi->disabled) {
			label += L(" (unavailable)", " (non disponibile)");
			items.push_back({label, []() {}, false});
			continue;
		}

		// Probe for a submenu; delete the probe (a fresh one is built on demand).
		ui::Menu* probe = mi->createChildMenu();
		bool hasSub = (probe != nullptr);
		delete probe;

		if (hasSub) {
			items.push_back({label, [self, mi]() {
				ui::Menu* sub = mi->createChildMenu();
				if (!sub)
					return;
				self->internal->ownedSubmenus.push_back(sub);
				auto subItems = buildItemsFromMenu(self, sub);
				self->internal->menuStack.push_back(subItems);
				self->internal->contextItems = subItems;
				reloadContextLevel(self);
			}, true});
		}
		else {
			items.push_back({label, [self, mi]() {
				mi->doAction(false);
				cleanupContextMenu(self);
			}, false});
		}
	}
	return items;
}

// ── Menu row (hosted only) ───────────────────────────────────────────────────
//
// Dentro una DAW la menu bar di sistema appartiene all'host: non possiamo prendercela (vedi
// buildMenuTree / create). Il rimedio è portare gli stessi menu DENTRO il pannello, come una
// riga di pulsanti — File, Modifica, Vista, Motore, Libreria, Aiuto — che aprono il loro
// contenuto nella lista CONTEXT_MENU, cioè lo stesso elenco leggibile che già serve i menu
// contestuali dei moduli. Nessuna voce viene riscritta: l'albero è quello di buildMenuTree.

// Convert one level of the menu tree into context-list rows. Twin of buildItemsFromMenu,
// which does the same job for Rack's own ui::Menu.
static std::vector<AXContextItem> itemsFromNSMenu(AccessibleWindow* self, NSMenu* menu) {
	AccessibleWindow::Internal* in = self->internal;

	// I due livelli dinamici si ricostruiscono all'apertura. Nello standalone se ne occupa
	// menuNeedsUpdate:, il delegate dell'NSMenu; qui il menu non si apre MAI (è solo dato),
	// quindi quel gancio non scatta e la ricostruzione va fatta a mano.
	if (menu == in->recentMenu)
		rebuildRecentMenu(self);
	else if (menu == in->libraryMenu)
		rebuildLibraryMenu(self);

	std::vector<AXContextItem> items;
	for (NSMenuItem* item in [menu itemArray]) {
		if ([item isSeparatorItem])
			continue;
		NSString* title = [item title];
		if (!title || ![title length])
			continue;
		std::string label = [title UTF8String];

		if (NSMenu* sub = [item submenu]) {
			// Livello annidato: stessa meccanica dei sottomenu di Rack — si costruisce
			// quando l'utente ci entra, non prima.
			items.push_back({label + " ▸", [self, sub]() {
				auto subItems = itemsFromNSMenu(self, sub);
				self->internal->menuStack.push_back(subItems);
				self->internal->contextItems = subItems;
				reloadContextLevel(self);
			}, true});
			continue;
		}

		// Voce non attivabile (il segnaposto "nessuna patch recente", per esempio): non ha
		// azione. Il suo tag vale 0 come per qualunque NSMenuItem appena creato, quindi
		// senza questo controllo sarebbe indistinguibile dal comando di indice 0 e
		// premerla lo eseguirebbe. La mostriamo come riga inerte.
		if (![item action]) {
			items.push_back({label, []() {}, false});
			continue;
		}

		// Foglia: il tag è l'indice in menuCmds, dove buildMenuTree ha messo azione e
		// stato della spunta. Leggiamo la spunta ORA, come farebbe validateMenuItem:
		// prima di mostrare il menu.
		NSInteger tag = [item tag];
		if (tag < 0 || tag >= (NSInteger) in->menuCmds.size())
			continue;
		auto& checked = in->menuCmds[tag].checked;
		if (checked && checked())
			label += " ✓";
		items.push_back({label, [self, tag]() {
			cleanupContextMenu(self);
			menuFire(self, tag);
		}, false});
	}
	return items;
}


// Open one top-level menu as a CONTEXT_MENU level (a button in the row was activated).
static void openTopMenu(AccessibleWindow* self, int index) {
	AccessibleWindow::Internal* in = self->internal;
	if (!in->menuTree)
		return;
	NSArray* tops = [in->menuTree itemArray];
	if (index < 0 || index >= (int) [tops count])
		return;
	NSMenu* sub = [[tops objectAtIndex:index] submenu];
	if (!sub)
		return;

	// Se un menu è già aperto, il ritorno resta la vista da cui si è partiti: altrimenti
	// Esc riporterebbe al menu precedente e non si uscirebbe più.
	AXView back = (in->currentView == AX_CONTEXT_MENU) ? in->previousView : in->currentView;
	cleanupContextMenu(self);
	auto items = itemsFromNSMenu(self, sub);
	if (items.empty())
		return;
	in->menuStack.push_back(items);   // livello 0: da qui Esc chiude il menu
	in->contextItems = items;
	in->previousView = back;
	switchTo(self, AX_CONTEXT_MENU);
}


// Move focus to a button of the menu row, wrapping around at both ends.
static void focusMenuButton(AccessibleWindow* self, int index) {
	AccessibleWindow::Internal* in = self->internal;
	if (!in->menuRow)
		return;
	NSArray* buttons = [in->menuRow subviews];
	int count = (int) [buttons count];
	if (count == 0)
		return;
	index = ((index % count) + count) % count;
	[in->panel makeFirstResponder:[buttons objectAtIndex:index]];
}


// Shift+M from any view: enter the menu row. Twin del "Alt" che su Windows entra nella menu
// bar vera — lì è il sistema a gestirlo, qui la riga è nostra e la scorciatoia va scritta.
static void onMenuRowKey(AccessibleWindow* self) {
	if (!self->internal->menuRow)
		return;
	focusMenuButton(self, 0);
	setStatus(self, L("Menu row. Left and Right to move, Return to open, Escape to leave.",
	                  "Riga di menù. Sinistra e Destra per spostarti, Invio per aprire, Esc per uscire."));
}


// Standard module context menu (Reset/Randomize/Disconnect/Bypass/Duplicate×2/Delete).
static void buildModuleContextMenu(AccessibleWindow* self, app::ModuleWidget* mw) {
	if (!mw || !mw->module)
		return;
	bool bypassed = mw->module->isBypassed();
	std::vector<AXContextItem> items;

	items.push_back({L("Reset parameters", "Azzera parametri"), [self, mw]() {
		pushCommand(self, [mw]() { mw->resetAction(); });
	}, false});
	items.push_back({L("Randomize parameters", "Randomizza parametri"), [self, mw]() {
		pushCommand(self, [mw]() { mw->randomizeAction(); });
	}, false});
	items.push_back({L("Disconnect cables", "Disconnetti cavi"), [self, mw]() {
		pushCommand(self, [mw]() { mw->disconnectAction(); });
	}, false});
	items.push_back({bypassed ? L("Bypass: disable", "Bypass: disattiva")
	                          : L("Bypass: enable", "Bypass: attiva"), [self, mw, bypassed]() {
		pushCommand(self, [mw, bypassed]() { mw->bypassAction(!bypassed); });
	}, false});
	items.push_back({L("Duplicate (no cables)", "Duplica (senza cavi)"), [self, mw]() {
		pushCommand(self, [self, mw]() {
			std::string sname = mw->model ? mw->model->name : "?";
			mw->cloneAction(false);
			// cloneAction inserts a ModuleWidget; the lazy RACK list must be refreshed
			// here or the duplicate stays invisible until the next rebuild.
			refreshRackView(self);
			self->internal->rackDirty = false;
			setStatus(self, L("Module \"", "Modulo \"") + sname + L("\" duplicated.", "\" duplicato."));
		});
	}, false});
	items.push_back({L("Duplicate with cables", "Duplica con cavi"), [self, mw]() {
		pushCommand(self, [self, mw]() {
			std::string sname = mw->model ? mw->model->name : "?";
			mw->cloneAction(true);
			refreshRackView(self);
			self->internal->rackDirty = false;
			setStatus(self, L("Module \"", "Modulo \"") + sname + L("\" duplicated (with cables).", "\" duplicato (con cavi)."));
		});
	}, false});
	items.push_back({L("Delete", "Elimina"), [self, mw]() {
		std::string sname = mw->model ? mw->model->name : "?";
		if (!confirm(L("Remove \"", "Rimuovere \"") + sname + L("\"?", "\"?")))
			return;
		// previousView is RACK here, so the rack table is shown and its selection is the
		// focused module; remember its row to land focus on the previous slot.
		int rowi = (int) [self->internal->rackTable selectedRow];
		pushCommand(self, [self, mw, sname, rowi]() {
			engine::Module* mod = mw->module;
			mw->removeAction();
			if (self->internal->currentModule == mod)
				self->internal->currentModule = nullptr;
			if (self->internal->lastParamModule == mod)
				self->internal->lastParamModule = nullptr;
			if (self->internal->pendingCable.module == mod)
				self->internal->pendingCable.active = false;
			refreshRackView(self, nullptr, rowi - 1);
			self->internal->rackDirty = false;
			setStatus(self, L("Module \"", "Modulo \"") + sname + L("\" removed.", "\" rimosso."));
		});
	}, false});

	showContextMenu(self, std::move(items));
}

// The module's own options (its appendContextMenu override), read into a detached menu
// and shown as a navigable list. Mirrors buildModuleSpecificContextMenu.
static void buildModuleSpecificContextMenu(AccessibleWindow* self, app::ModuleWidget* mw) {
	if (!mw || !mw->module)
		return;
	cleanupContextMenu(self);

	ui::Menu* extra = new ui::Menu;
	mw->appendContextMenu(extra);
	auto items = buildItemsFromMenu(self, extra);
	if (items.empty()) {
		delete extra;
		setStatus(self, L("No specific options for this module.",
		                  "Nessuna opzione specifica per questo modulo."));
		return;
	}
	// Keep the detached menu alive while navigating: the item lambdas hold pointers into
	// its children, and submenus are built on demand. Freed by cleanupContextMenu().
	self->internal->ownedRootMenu = extra;
	self->internal->menuStack.push_back(items);   // level 0
	showContextMenu(self, items);
}

// Param context menu (Set value… / Reset to default) from the PARAM view.
static void buildParamContextMenu(AccessibleWindow* self, int paramId) {
	if (!self->internal->currentModule)
		return;
	engine::ParamQuantity* pq = self->internal->currentModule->getParamQuantity(paramId);
	if (!pq)
		return;
	std::vector<AXContextItem> items;

	items.push_back({L("Set value…", "Imposta valore…"), [self, pq]() {
		std::string prompt = L("Value for «", "Valore per «") + pq->name
		                     + L("»:\n(e.g.: 440, C4, log2(8), dbtogain(-6))",
		                         "»:\n(Es: 440, C4, log2(8), dbtogain(-6))");
		std::string text = showInputDialog(L("Set value", "Imposta valore"), prompt,
		                                   pq->getDisplayValueString());
		if (text.empty())
			return;
		pq->setDisplayValueString(text);
		[self->internal->paramTable reloadData];
		announce(self, pq->getDisplayValueString() + pq->getUnit());
	}, false});
	items.push_back({L("Reset to default", "Azzera al valore predefinito"), [self, pq]() {
		pq->reset();
		[self->internal->paramTable reloadData];
		announce(self, pq->getDisplayValueString() + pq->getUnit());
	}, false});

	showContextMenu(self, std::move(items));
}

// Defined in the display-cell block below, but referenced from onContextEnter.
static void collectDisplayCells(AccessibleWindow* self, app::ModuleWidget* mw);
static void openDisplayCell(AccessibleWindow* self, app::LedDisplayChoice* choice);

// Enter on a context row: a submenu pushes a new level; a leaf returns to the previous
// view and fires its action. The item is copied first because a submenu push reassigns
// contextItems mid-call.
static void onContextEnter(AccessibleWindow* self) {
	AccessibleWindow::Internal* in = self->internal;
	NSInteger row = [in->contextTable selectedRow];
	if (row < 0 || row >= (NSInteger) in->contextItems.size())
		return;
	AXContextItem item = in->contextItems[row];

	if (item.isSubmenu) {
		if (item.action)
			item.action();   // pushes a new level
		return;
	}

	// Inside a Tier-A display submenu (level ≥1 of a display flow): run the option, then
	// re-open the display-cell list so the user can change other display settings without
	// pressing D again. The option's action calls cleanupContextMenu (clearing displayCells
	// and menuStack), so we re-collect and rebuild afterwards.
	bool inDisplayOptions = !in->displayCells.empty() && in->menuStack.size() >= 2;
	if (inDisplayOptions) {
		int returnRow = in->lastDisplayCellRow;
		if (item.action)
			item.action();
		// currentModule and previousView survive cleanup.
		if (in->currentModule && APP && APP->scene && APP->scene->rack) {
			app::ModuleWidget* mw = APP->scene->rack->getModule(in->currentModule->id);
			if (mw) {
				collectDisplayCells(self, mw);
				if (!in->displayCells.empty()) {
					std::vector<AXContextItem> cells;
					for (auto& dc : in->displayCells) {
						app::LedDisplayChoice* ch = dc.choice;
						cells.push_back({dc.label, [self, ch]() { openDisplayCell(self, ch); }, false});
					}
					in->menuStack.push_back(cells);
					in->contextItems = cells;
					[in->contextTable reloadData];
					int clampRow = std::min(returnRow, (int) cells.size() - 1);
					if (clampRow < 0)
						clampRow = 0;
					[in->contextTable selectRowIndexes:[NSIndexSet indexSetWithIndex:clampRow]
					                byExtendingSelection:NO];
					[in->contextTable scrollRowToVisible:clampRow];
					NSAccessibilityPostNotification(in->contextTable,
					    NSAccessibilitySelectedRowsChangedNotification);
				}
			}
		}
		return;
	}

	// Opening a display cell at level 0: remember the row so focus can return to it.
	if (!in->displayCells.empty() && in->menuStack.size() == 1)
		in->lastDisplayCellRow = (int) row;
	switchTo(self, in->previousView);
	if (item.action)
		item.action();
}

// Escape in the context menu: pop one submenu level, or close the menu and return.
static void onContextEsc(AccessibleWindow* self) {
	AccessibleWindow::Internal* in = self->internal;
	if (in->menuStack.size() > 1) {
		if (!in->ownedSubmenus.empty()) {
			delete in->ownedSubmenus.back();
			in->ownedSubmenus.pop_back();
		}
		in->menuStack.pop_back();
		in->contextItems = in->menuStack.back();
		reloadContextLevel(self);
	}
	else {
		AXView prev = in->previousView;
		cleanupContextMenu(self);
		switchTo(self, prev);
	}
}

// Generic context-menu trigger (Cmd+M): the module menu in RACK, the param menu in
// PARAM. No-op elsewhere (mirrors the Win32 handleContextMenuKey).
static void onContextMenuKey(AccessibleWindow* self) {
	AccessibleWindow::Internal* in = self->internal;
	if (in->currentView == AX_RACK) {
		AXRow* r = focusedRackRow(self);
		if (!r || r->freeSlot)
			return;
		buildModuleContextMenu(self, r->mw);
	}
	else if (in->currentView == AX_PARAM) {
		NSInteger row = [in->paramTable selectedRow];
		if (row < 0 || row >= (NSInteger) in->paramRows.size() || !in->currentModule)
			return;
		buildParamContextMenu(self, in->paramRows[row]);
	}
}

// Module-specific trigger (Cmd+Shift+M): the focused module in RACK, otherwise the
// currentModule whose detail view is open. Mirrors handleModuleSpecificContextMenuKey.
static void onModuleSpecificContextMenuKey(AccessibleWindow* self) {
	AccessibleWindow::Internal* in = self->internal;
	app::ModuleWidget* mw = nullptr;
	if (in->currentView == AX_RACK) {
		AXRow* r = focusedRackRow(self);
		if (!r || r->freeSlot)
			return;
		mw = r->mw;
	}
	else if (in->currentModule && APP && APP->scene && APP->scene->rack) {
		mw = APP->scene->rack->getModule(in->currentModule->id);
	}
	if (!mw)
		return;
	buildModuleSpecificContextMenu(self, mw);
}

// ── Display cells (D key) — Tier A menus + Tier B MIDI learn ──────────────────
// Walk a widget subtree collecting LedDisplayChoice cells; don't recurse into one (its
// children are rendering details). step() refreshes the cell's text first.
static void collectDisplayCellsRec(widget::Widget* w, std::vector<AXDisplayCell>& out) {
	if (auto* dc = dynamic_cast<app::LedDisplayChoice*>(w)) {
		dc->step();
		out.push_back({dc, dc->text.empty() ? "(display)" : dc->text});
		return;
	}
	for (widget::Widget* child : w->children)
		collectDisplayCellsRec(child, out);
}

static void collectDisplayCells(AccessibleWindow* self, app::ModuleWidget* mw) {
	self->internal->displayCells.clear();
	if (!mw)
		return;
	for (widget::Widget* child : mw->children)
		collectDisplayCellsRec(child, self->internal->displayCells);
}

// Fire a display cell's click, then fork: if a MenuOverlay appeared (Tier A), capture it
// and show its items in the accessible CONTEXT_MENU; otherwise (Tier B) enter MIDI-learn
// mode on the widget directly. Mirrors the Win32 openDisplayCell.
static void openDisplayCell(AccessibleWindow* self, app::LedDisplayChoice* choice) {
	if (!APP || !APP->scene || !choice)
		return;

	widget::Widget* lastBefore = APP->scene->children.empty() ? nullptr : APP->scene->children.back();
	widget::Widget::ActionEvent eAction;
	choice->onAction(eAction);
	widget::Widget* lastAfter = APP->scene->children.empty() ? nullptr : APP->scene->children.back();

	if (lastAfter && lastAfter != lastBefore) {
		auto* overlay = dynamic_cast<ui::MenuOverlay*>(lastAfter);
		if (overlay) {
			self->internal->capturedOverlay = overlay;
			ui::Menu* menu = nullptr;
			for (widget::Widget* child : overlay->children) {
				menu = dynamic_cast<ui::Menu*>(child);
				if (menu)
					break;
			}
			if (menu) {
				auto items = buildItemsFromMenu(self, menu);
				self->internal->menuStack.push_back(items);
				self->internal->contextItems = items;
				// previousView was set when the cell list opened (D key); keep it — just
				// re-reveal CONTEXT_MENU with the new level.
				switchTo(self, AX_CONTEXT_MENU);
			}
			else {
				APP->scene->removeChild(overlay);
				delete overlay;
				self->internal->capturedOverlay = nullptr;
				setStatus(self, L("Error: unrecognized menu structure.",
				                  "Errore: struttura del menu non riconosciuta."));
			}
			return;
		}
	}

	// Tier B: no overlay appeared — enter MIDI-learn mode on the widget.
	if (!APP->event)
		return;
	APP->event->setSelectedWidget(choice);
	self->internal->learningCell = choice;
	self->internal->learningLastText = choice->text;
	setStatus(self, L("Learning — press the MIDI control. Space = toggle. Esc = cancel.",
	                  "In apprendimento — premi il controllo MIDI. Spazio = toggle. Esc = annulla."));
}

// D key: open the focused module's clickable-display list as a context menu. Works from
// RACK (acts on the focused module, like P/O/I) and from PARAM (uses currentModule).
static void onDisplayKey(AccessibleWindow* self) {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;
	AccessibleWindow::Internal* in = self->internal;

	if (in->currentView == AX_RACK) {
		AXRow* r = focusedRackRow(self);
		if (!r || r->freeSlot)
			return;
		in->currentModule = r->mw->module;
	}
	if (!in->currentModule)
		return;
	app::ModuleWidget* mw = APP->scene->rack->getModule(in->currentModule->id);
	if (!mw)
		return;

	cleanupContextMenu(self);
	collectDisplayCells(self, mw);
	if (in->displayCells.empty()) {
		setStatus(self, L("No clickable displays for this module.",
		                  "Nessun display cliccabile per questo modulo."));
		return;
	}

	std::vector<AXContextItem> items;
	for (auto& cell : in->displayCells) {
		app::LedDisplayChoice* choice = cell.choice;
		items.push_back({cell.label, [self, choice]() { openDisplayCell(self, choice); }, false});
	}
	in->menuStack.push_back(items);   // level 0 = display-cell list
	showContextMenu(self, items);     // sets previousView = RACK / PARAM
}

// Cancel an active Tier-B learn mode (Esc from any view); returns true if it did.
static bool cancelLearnMode(AccessibleWindow* self) {
	if (!self->internal->learningCell)
		return false;
	if (APP && APP->event)
		APP->event->setSelectedWidget(nullptr);
	self->internal->learningCell = nullptr;
	self->internal->learningLastText.clear();
	setStatus(self, L("Learn mode cancelled.", "Apprendimento annullato."));
	return true;
}

// Toggle the learn target's selection (Space in learn mode); returns true if handled.
static bool toggleLearnSelect(AccessibleWindow* self) {
	if (!self->internal->learningCell || !APP || !APP->event)
		return false;
	if (APP->event->getSelectedWidget() == self->internal->learningCell)
		APP->event->setSelectedWidget(nullptr);
	else
		APP->event->setSelectedWidget(self->internal->learningCell);
	return true;
}

// ── Show / hide ──────────────────────────────────────────────────────────────
static void setLayerVisible(AccessibleWindow* self, bool show) {
	AccessibleWindow::Internal* in = self->internal;
	// Hosted, this is not the user's standalone preference: don't overwrite it.
	if (!in->pluginMode)
		settings::accessibleLayerVisible = show;
	if (show) {
		[in->panel setFrame:[in->rackWindow frame] display:YES];
		// Standalone: the panel is a child window laid over the Rack window, so the two
		// travel together. Hosted: the Rack window is hidden and must stay hidden, and a
		// child window is ordered out with its parent — so the panel stands on its own,
		// as an ordinary window of the host's process. That is also what makes Command+`
		// (Move focus to next window) a working route back to the DAW.
		if (!in->pluginMode)
			[in->rackWindow addChildWindow:in->panel ordered:NSWindowAbove];
		[in->panel makeKeyAndOrderFront:nil];
		in->visible = true;
		// Always rebuild on show so the list reflects any changes made in the GUI.
		in->rackDirty = true;
		switchTo(self, in->currentView);
		announce(self, L("Accessible interface", "Interfaccia accessibile"));
	}
	else {
		if (!in->pluginMode)
			[in->rackWindow removeChildWindow:in->panel];
		[in->panel orderOut:nil];
		// Standalone: hand key back to Rack. Hosted: Rack's window is hidden, so handing
		// key to it would strand the user on nothing; AppKit gives the host's own window
		// back on its own.
		if (!in->pluginMode)
			[in->rackWindow makeKeyAndOrderFront:nil];
		in->visible = false;
	}
}

// Keys that work from every view. Returns true when the key was consumed here.
//
// The function keys carry NO CHARACTER, which is the whole reason they were chosen: the bare
// letters they replaced (I/O/P for the three detail views, D for the displays) collided with
// the lists' first-letter type-ahead — pressing "o" both opened the output list and jumped
// the rack selection to a module starting with "o". Every letter is now free for type-ahead
// again. Same map as the Win32 layer, so the two platforms are learnt once.
//
// NB su macOS i tasti funzione arrivano all'applicazione solo se in Impostazioni di Sistema
// è attivo "Usa i tasti F1, F2 ecc. come tasti funzione standard"; altrimenti vanno tenuti
// premuti insieme a Fn. Su Windows il problema non esiste.
static bool handleGlobalKey(AccessibleWindow* self, NSEvent* e) {
	AccessibleWindow::Internal* in = self->internal;
	NSEventModifierFlags m = [e modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask;
	const bool shiftOnly = (m & NSEventModifierFlagShift)
	                       && !(m & (NSEventModifierFlagCommand | NSEventModifierFlagControl
	                                 | NSEventModifierFlagOption));
	const bool noMods = !(m & (NSEventModifierFlagShift | NSEventModifierFlagCommand
	                           | NSEventModifierFlagControl | NSEventModifierFlagOption));

	if (shiftOnly) {
		NSString* ch = [[e charactersIgnoringModifiers] lowercaseString];
		if ([ch isEqualToString:@"m"]) {
			// Standalone non c'è riga: i menu stanno nella menu bar di sistema, che ha già
			// le sue vie d'accesso. Lì Shift+M non è nostro e va lasciato passare.
			if (!in->menuRow)
				return false;
			onMenuRowKey(self);
			return true;
		}
		if ([ch isEqualToString:@"d"]) {
			// Display cliccabili del modulo a fuoco (RACK) o di quello già aperto
			// (PARAM/OUTPUT/INPUT): onDisplayKey gestisce entrambi i casi.
			if (in->currentView == AX_CONTEXT_MENU)
				return false;
			onDisplayKey(self);
			return true;
		}
	}

	if (noMods) {
		switch ([e keyCode]) {
			case 122:   // F1 → manuale, come nella GUI standard di Rack
				system::openBrowser("https://vcvrack.com/manual/");
				return true;
			case 120:   // F2 → ingressi
				switchToDetailView(self, AX_INPUT);
				return true;
			case 99:    // F3 → uscite
				switchToDetailView(self, AX_OUTPUT);
				return true;
			case 118:   // F4 → parametri
				switchToDetailView(self, AX_PARAM);
				return true;
			case 96:    // F5 → menu contestuale specifico del modulo (come Cmd+Shift+M)
				if (in->currentView == AX_CONTEXT_MENU)
					return false;
				onModuleSpecificContextMenuKey(self);
				return true;
		}
	}
	return false;
}


// True when a key event belongs to the accessible layer rather than to the host.
//
// Deliberately narrow, and the narrowness is the safety: it demands that OUR panel is the key
// window and that the focus sits on one of OUR lists (or on the menu row). Anything else — a
// modal alert, a text field, the host's own windows — falls through untouched. Cmd, Control
// and Option combos fall through too: those belong to the host's menus and to VoiceOver,
// which owns Control+Option.
static bool panelOwnsKeyEvent(AccessibleWindow* self, NSEvent* e) {
	AccessibleWindow::Internal* in = self->internal;
	if (!in->pluginMode || !in->panel || ![in->panel isKeyWindow])
		return false;

	NSEventModifierFlags m = [e modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask;
	if (m & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption))
		return false;

	NSResponder* fr = [in->panel firstResponder];
	if (!fr)
		return false;
	NSResponder* ours[] = { in->rackTable, in->libraryOutline, in->paramTable,
	                        in->outputTable, in->inputTable, in->contextTable
	                      };
	for (NSResponder* r : ours) {
		if (r && fr == r)
			return true;
	}
	// La riga di menù: i pulsanti sono figli di menuRow.
	if (in->menuRow && [fr isKindOfClass:[NSView class]]
	    && [(NSView*) fr superview] == in->menuRow)
		return true;
	return false;
}


} // namespace accessible
} // namespace rack

// ─────────────────────────────────────────────────────────────────────────────
// AppKit glue
// ─────────────────────────────────────────────────────────────────────────────

// Datasource/delegate for both the RACK table and the LIBRARY outline. Reads rows
// straight from the C++ state (rackRows) and the AXLibNode tree (libraryRoots).
@interface RackAXController : NSObject <NSTableViewDataSource, NSTableViewDelegate,
                                        NSOutlineViewDataSource, NSOutlineViewDelegate> {
@public
	rack::accessible::AccessibleWindow* owner;
}
@end

@implementation RackAXController
- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
	if (!owner || !owner->internal)
		return 0;
	rack::accessible::AccessibleWindow::Internal* in = owner->internal;
	if (tv == in->paramTable)
		return (NSInteger) in->paramRows.size();
	if (tv == in->outputTable)
		return in->currentModule ? in->currentModule->getNumOutputs() : 0;
	if (tv == in->inputTable)
		return in->currentModule ? in->currentModule->getNumInputs() : 0;
	if (tv == in->contextTable)
		return (NSInteger) in->contextItems.size();
	return (NSInteger) in->rackRows.size();
}
- (id)tableView:(NSTableView*)tv objectValueForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
	if (!owner || !owner->internal)
		return @"";
	rack::accessible::AccessibleWindow::Internal* in = owner->internal;

	// PARAM table: two columns (name, value) read live from the ParamQuantity, so a
	// reload after an edit always shows the engine's current value.
	if (tv == in->paramTable) {
		if (row < 0 || row >= (NSInteger) in->paramRows.size() || !in->currentModule)
			return @"";
		engine::ParamQuantity* pq = in->currentModule->getParamQuantity(in->paramRows[row]);
		if (!pq)
			return @"";
		if ([[col identifier] isEqualToString:@"pvalue"])
			return [NSString stringWithUTF8String:(pq->getDisplayValueString() + pq->getUnit()).c_str()];
		return [NSString stringWithUTF8String:pq->name.c_str()];
	}

	// OUTPUT / INPUT tables: two columns (port name, cable status), read live.
	if (tv == in->outputTable || tv == in->inputTable) {
		bool isOutput = (tv == in->outputTable);
		if (!in->currentModule)
			return @"";
		int numPorts = isOutput ? in->currentModule->getNumOutputs()
		                        : in->currentModule->getNumInputs();
		if (row < 0 || row >= numPorts)
			return @"";
		int portId = (int) row;
		if ([[col identifier] isEqualToString:@"portstatus"]) {
			std::string st = rack::accessible::portStatusString(in->currentModule, isOutput, portId);
			return [NSString stringWithUTF8String:st.c_str()];
		}
		engine::PortInfo* info = isOutput ? in->currentModule->getOutputInfo(portId)
		                                  : in->currentModule->getInputInfo(portId);
		std::string name = info ? info->getName()
		                        : (rack::accessible::L("Port ", "Porta ") + std::to_string(portId));
		// If the module never named this port (getName() gave a bare "#N"), recover a
		// semantic name from the panel SVG placement id.
		if (info && info->name.empty() && APP && APP->scene && APP->scene->rack) {
			app::ModuleWidget* mw = APP->scene->rack->getModule(in->currentModule->id);
			app::PortWidget* pw = mw ? (isOutput ? mw->getOutput(portId) : mw->getInput(portId)) : NULL;
			std::string svgId = rack::accessible::portSvgId(mw, pw);
			if (!svgId.empty())
				name = svgId;
		}
		return [NSString stringWithUTF8String:name.c_str()];
	}

	// CONTEXT_MENU table: single column of item labels.
	if (tv == in->contextTable) {
		if (row < 0 || row >= (NSInteger) in->contextItems.size())
			return @"";
		return [NSString stringWithUTF8String:in->contextItems[row].label.c_str()];
	}

	auto& rows = in->rackRows;
	if (row < 0 || row >= (NSInteger) rows.size())
		return @"";
	return [NSString stringWithUTF8String:rows[row].label.c_str()];
}

// ── LIBRARY outline datasource ───────────────────────────────────────────────
// Items are AXLibNode*: nil is the root, brand nodes have children, model nodes don't.
- (NSInteger)outlineView:(NSOutlineView*)ov numberOfChildrenOfItem:(id)item {
	if (!owner || !owner->internal)
		return 0;
	if (item == nil)
		return (NSInteger) [owner->internal->libraryRoots count];
	AXLibNode* n = (AXLibNode*) item;
	return n->children ? (NSInteger) [n->children count] : 0;
}
- (id)outlineView:(NSOutlineView*)ov child:(NSInteger)index ofItem:(id)item {
	if (!owner || !owner->internal)
		return nil;
	if (item == nil)
		return [owner->internal->libraryRoots objectAtIndex:index];
	AXLibNode* n = (AXLibNode*) item;
	return [n->children objectAtIndex:index];
}
- (BOOL)outlineView:(NSOutlineView*)ov isItemExpandable:(id)item {
	AXLibNode* n = (AXLibNode*) item;
	return n && n->model == nullptr;
}
- (id)outlineView:(NSOutlineView*)ov objectValueForTableColumn:(NSTableColumn*)col byItem:(id)item {
	AXLibNode* n = (AXLibNode*) item;
	return n ? n->label : @"";
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
	if (handleGlobalKey(owner, e))
		return;
	NSEventModifierFlags m = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
	if (m & NSEventModifierFlagCommand) {
		// Cmd+Enter moves the focused module onto a new row (grows the patch vertically) —
		// the Mac home for the Win32 Ctrl+Enter.
		if (e.keyCode == 36 || e.keyCode == 76) {
			moveFocusedModuleToNewRow(owner);
			return;
		}
		// Cmd+M / Cmd+Shift+M open the context menus; other Cmd-combos go to the menu bar.
		if ([[[e charactersIgnoringModifiers] lowercaseString] isEqualToString:@"m"]) {
			if (m & NSEventModifierFlagShift)
				onModuleSpecificContextMenuKey(owner);
			else
				onContextMenuKey(owner);
			return;
		}
		[super keyDown:e];
		return;
	}
	unsigned short kc = e.keyCode;
	NSString* ch = [[e charactersIgnoringModifiers] lowercaseString];
	bool shift = (m & NSEventModifierFlagShift) != 0;

	// Shift+L jumps to the LIBRARY view (parity with the Win32 Shift+L shortcut).
	if (shift && [ch isEqualToString:@"l"]) {
		switchTo(owner, AX_LIBRARY);
		return;
	}
	// Arrow keys drive the 2D spatial grid: Left/Right within a row, Up/Down between rows.
	// NSTableView's native Up/Down (linear) would step through every cell in sequence, so
	// we replace all four with navigateRack to mirror the Win32 icon-view navigation.
	if (kc == 123) { navigateRack(owner, -1,  0); return; }   // Left
	if (kc == 124) { navigateRack(owner, +1,  0); return; }   // Right
	if (kc == 126) { navigateRack(owner,  0, -1); return; }   // Up
	if (kc == 125) { navigateRack(owner,  0, +1); return; }   // Down
	if (kc == 53) {                      // Escape → cancel learn mode / pending connection
		if (cancelLearnMode(owner))
			return;
		cancelPendingCable(owner);
		return;
	}
	if (kc == 36 || kc == 76) {          // Return / keypad Enter
		onRackEnter(owner);
		return;
	}
	if (kc == 51 || kc == 117) {         // Backspace / Forward Delete
		onRackDelete(owner);
		return;
	}
	if (kc == 49) {                      // Space → learn toggle, else selection toggle
		if (toggleLearnSelect(owner))
			return;
		onRackToggleSelect(owner);
		return;
	}
	// Nessuna lettera nuda qui: P/O/I/D sono diventate F4/F3/F2 e Shift+D (vedi
	// handleGlobalKey), così le lettere tornano al type-ahead della lista.
	[super keyDown:e];
}
@end

// NSTableView for the PARAM list (2 columns). Up/Down navigate rows natively (VoiceOver
// reads name + value); we intercept the editing keys. Arrow Left/Right are handled even
// with Cmd held, since no menu item claims Cmd+Arrow — Cmd/Shift only pick the step size.
@interface RackAXParamTableView : NSTableView {
@public
	rack::accessible::AccessibleWindow* owner;
}
@end

@implementation RackAXParamTableView
- (void)keyDown:(NSEvent*)e {
	using namespace rack::accessible;
	if (!owner || !owner->internal) {
		[super keyDown:e];
		return;
	}
	if (handleGlobalKey(owner, e))
		return;
	NSEventModifierFlags m = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
	unsigned short kc = e.keyCode;
	NSString* ch = [[e charactersIgnoringModifiers] lowercaseString];
	bool shift = (m & NSEventModifierFlagShift) != 0;
	bool cmd   = (m & NSEventModifierFlagCommand) != 0;

	if (kc == 123) { onParamKey(owner, 'L', cmd, shift); return; }  // Left → step down
	if (kc == 124) { onParamKey(owner, 'R', cmd, shift); return; }  // Right → step up

	if (cmd) {
		// Cmd+M / Cmd+Shift+M open the context menus; other Cmd-combos go to the menu bar.
		if ([ch isEqualToString:@"m"]) {
			if (shift)
				onModuleSpecificContextMenuKey(owner);
			else
				onContextMenuKey(owner);
			return;
		}
		[super keyDown:e];
		return;
	}

	if (kc == 36 || kc == 76) { onParamKey(owner, 'V', false, false); return; } // Return → value
	if (kc == 51)             { onParamKey(owner, 'B', false, false); return; } // Backspace → reset
	if (kc == 49) {                                                            // Space
		if (toggleLearnSelect(owner))
			return;
		onParamKey(owner, 'S', false, false);   // toggle switch / pulse momentary
		return;
	}
	if (kc == 53) {                                                             // Escape → RACK
		if (cancelLearnMode(owner))
			return;
		if (cancelPendingCable(owner))
			return;
		switchTo(owner, AX_RACK);
		return;
	}

	if (kc == 48) {                                // Tab / Shift+Tab → cycle detail views
		AXView cycle[3] = {AX_PARAM, AX_OUTPUT, AX_INPUT};
		for (int i = 0; i < 3; i++) {
			if (owner->internal->currentView == cycle[i]) {
				if (!owner->internal->currentModule)
					return;
				int nx = shift ? (i + 2) % 3 : (i + 1) % 3;
				switchTo(owner, cycle[nx]);
				return;
			}
		}
		return;
	}

	if (shift && [ch isEqualToString:@"r"]) { switchTo(owner, AX_RACK); return; }
	if (shift && [ch isEqualToString:@"l"]) { switchTo(owner, AX_LIBRARY); return; }
	if ([ch isEqualToString:@"v"]) { onParamKey(owner, 'V', false, false); return; }
	if ([ch isEqualToString:@"d"]) { onDisplayKey(owner); return; }
	[super keyDown:e];
}
@end

// NSTableView for the OUTPUT / INPUT port lists (2 columns: name, cable status). One
// instance per direction; isOutput tells the handlers which side this is. Up/Down
// navigate natively; Enter connects (two-step), Delete/Backspace disconnect, Escape
// cancels a pending connection then returns to RACK.
@interface RackAXPortTableView : NSTableView {
@public
	rack::accessible::AccessibleWindow* owner;
	BOOL isOutput;
}
@end

@implementation RackAXPortTableView
- (void)keyDown:(NSEvent*)e {
	using namespace rack::accessible;
	if (!owner || !owner->internal) {
		[super keyDown:e];
		return;
	}
	if (handleGlobalKey(owner, e))
		return;
	NSEventModifierFlags m = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
	if (m & NSEventModifierFlagCommand) {
		// Cmd+Shift+M opens the module-specific menu; other Cmd-combos go to the menu bar.
		if ([[[e charactersIgnoringModifiers] lowercaseString] isEqualToString:@"m"]) {
			if (m & NSEventModifierFlagShift)
				onModuleSpecificContextMenuKey(owner);
			return;
		}
		[super keyDown:e];
		return;
	}
	unsigned short kc = e.keyCode;
	NSString* ch = [[e charactersIgnoringModifiers] lowercaseString];
	bool shift = (m & NSEventModifierFlagShift) != 0;

	if (kc == 36 || kc == 76) { onPortEnter(owner, isOutput); return; }   // Return → connect
	if (kc == 51 || kc == 117) { onPortDelete(owner, isOutput); return; } // Backspace/Del → disconnect
	if (kc == 53) {                                                       // Escape
		if (cancelPendingCable(owner))
			return;
		switchTo(owner, AX_RACK);
		return;
	}
	if (kc == 48) {                                // Tab / Shift+Tab → cycle detail views
		AXView cycle[3] = {AX_PARAM, AX_OUTPUT, AX_INPUT};
		for (int i = 0; i < 3; i++) {
			if (owner->internal->currentView == cycle[i]) {
				if (!owner->internal->currentModule)
					return;
				int nx = shift ? (i + 2) % 3 : (i + 1) % 3;
				switchTo(owner, cycle[nx]);
				return;
			}
		}
		return;
	}
	if (shift && [ch isEqualToString:@"r"]) { switchTo(owner, AX_RACK); return; }
	if (shift && [ch isEqualToString:@"l"]) { switchTo(owner, AX_LIBRARY); return; }
	[super keyDown:e];
}
@end

// NSTableView for the CONTEXT_MENU list (single column). Up/Down navigate natively;
// Enter activates a row (submenu push or leaf action), Escape pops a level or closes.
// Cmd-combos pass through so the trigger can't re-fire while the menu is open.
@interface RackAXContextTableView : NSTableView {
@public
	rack::accessible::AccessibleWindow* owner;
}
@end

@implementation RackAXContextTableView
- (void)keyDown:(NSEvent*)e {
	using namespace rack::accessible;
	if (!owner || !owner->internal) {
		[super keyDown:e];
		return;
	}
	if (handleGlobalKey(owner, e))
		return;
	NSEventModifierFlags m = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
	if (m & NSEventModifierFlagCommand) {
		[super keyDown:e];
		return;
	}
	unsigned short kc = e.keyCode;
	if (kc == 36 || kc == 76) { onContextEnter(owner); return; }  // Return → activate
	if (kc == 53)             { onContextEsc(owner); return; }    // Escape → pop / close
	[super keyDown:e];
}
@end

// NSOutlineView for the LIBRARY tree. Up/Down navigate and Left/Right collapse/expand
// natively (VoiceOver reads each item); we only intercept Enter (select/toggle), Escape
// and Shift+R (back to RACK). Mirrors the Win32 treeLibrary subclass.
@interface RackAXOutlineView : NSOutlineView {
@public
	rack::accessible::AccessibleWindow* owner;
}
@end

@implementation RackAXOutlineView
- (void)keyDown:(NSEvent*)e {
	using namespace rack::accessible;
	if (!owner || !owner->internal) {
		[super keyDown:e];
		return;
	}
	if (handleGlobalKey(owner, e))
		return;
	NSEventModifierFlags m = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
	if (m & NSEventModifierFlagCommand) {   // Cmd-combos belong to the menu bar
		[super keyDown:e];
		return;
	}
	unsigned short kc = e.keyCode;
	NSString* ch = [[e charactersIgnoringModifiers] lowercaseString];
	bool shift = (m & NSEventModifierFlagShift) != 0;

	if (kc == 36 || kc == 76) {             // Return / keypad Enter
		onLibraryEnter(owner);
		return;
	}
	if (kc == 53) {                         // Escape → back to RACK
		if (cancelPendingCable(owner))
			return;
		switchTo(owner, AX_RACK);
		return;
	}
	if (shift && [ch isEqualToString:@"r"]) { // Shift+R → back to RACK
		switchTo(owner, AX_RACK);
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

// The accessible panel's window class.
//
// VoiceOver's window chooser (VO+F2 F2) switches windows through the accessibility API, and
// AppKit's default raise only ORDERS THE WINDOW FRONT — it does not make it the key window.
// Standalone that is invisible: nothing else in the process wants the keyboard, so whoever
// is in front effectively has it. Inside a DAW it splits the two apart, and the split is
// exactly what the user feels: VoiceOver reads the MetaRack panel while every keystroke
// still goes to the host, which kept key. For this panel, being in front and being usable
// are the same thing, so raising it must also focus it.
@interface AXPanelWindow : NSWindow
@end

@implementation AXPanelWindow

- (BOOL)accessibilityPerformRaise {
	[self makeKeyAndOrderFront:nil];
	return YES;
}

@end


// A button of the panel's menu row (hosted only).
//
// Due ragioni per una sottoclasse invece di un NSButton nudo. La prima: un NSButton accetta
// il primo responder SOLO se "Accesso completo da tastiera" è attivo nel sistema, e non
// possiamo far dipendere l'unica via ai menu da un'impostazione che l'utente potrebbe non
// avere. La seconda: dentro la riga le frecce devono spostarsi tra i pulsanti, come in una
// menu bar vera, non far scorrere altro.
@interface AXMenuButton : NSButton {
@public
	rack::accessible::AccessibleWindow* owner;
}
@end

@implementation AXMenuButton

- (BOOL)acceptsFirstResponder {
	return YES;
}

- (void)keyDown:(NSEvent*)e {
	using namespace rack::accessible;
	if (!owner) {
		[super keyDown:e];
		return;
	}
	unsigned short kc = e.keyCode;
	if (kc == 123) { focusMenuButton(owner, (int)[self tag] - 1); return; }  // Left
	if (kc == 124) { focusMenuButton(owner, (int)[self tag] + 1); return; }  // Right
	if (kc == 36 || kc == 76 || kc == 49) {                                  // Return / Enter / Space
		[self performClick:nil];
		return;
	}
	if (kc == 53) {                                                          // Escape → back
		focusActiveControl(owner);
		return;
	}
	[super keyDown:e];
}

@end


// Action target of the menu row's buttons: the tag is the index of the top-level menu.
@interface AXMenuButtonTarget : NSObject {
@public
	rack::accessible::AccessibleWindow* owner;
}
- (void)pressed:(id)sender;
@end

@implementation AXMenuButtonTarget
- (void)pressed:(id)sender {
	using namespace rack::accessible;
	if (owner)
		openTopMenu(owner, (int)[(NSButton*) sender tag]);
}
@end


// Timer target for the hosted (plugin) case. The standalone drains the command queue from
// Window::step(); inside a DAW there is no Rack run loop, so this fires on the host's run
// loop instead. Same contract as the standalone's per-frame call: it runs on the main
// thread, where the adapter has already made this instance's Context current.
@interface AXDrainTimerTarget : NSObject {
@public
	rack::accessible::AccessibleWindow* owner;
}
- (void)tick:(NSTimer*)timer;
@end

@implementation AXDrainTimerTarget
- (void)tick:(NSTimer*)timer {
	(void) timer;
	if (owner)
		owner->drainCommands();
}
@end


namespace rack {
namespace accessible {

// Clear pointers that a patch load / undo / paste may have invalidated, then rebuild
// the RACK list. Mirrors the Win32 reloadRackAfterMutation.
static void reloadRackAfterMutation(AccessibleWindow* self) {
	// A patch load / undo / paste may have destroyed the modules these point at.
	cleanupContextMenu(self);
	self->internal->currentModule = nullptr;
	self->internal->lastParamModule = nullptr;
	self->internal->pendingCable.active = false;
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

// ── Port table builder ───────────────────────────────────────────────────────
// Build one OUTPUT/INPUT table (2 columns) inside a hidden scroll view, add it to the
// panel content and return it. Shared by the two directions; isOutput picks the labels
// and tags the view so the key handlers know which side they are on.
static NSTableView* buildPortTable(AccessibleWindow* self, NSView* content,
                                   id controller, bool isOutput, CGFloat w, CGFloat h) {
	NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 22, w, contentH(self, h))];
	[scroll setHasVerticalScroller:YES];
	[scroll setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
	[scroll setHidden:YES];

	RackAXPortTableView* t = [[RackAXPortTableView alloc] initWithFrame:NSMakeRect(0, 0, w, contentH(self, h))];
	t->owner = self;
	t->isOutput = isOutput ? YES : NO;
	CGFloat cw = w > 80 ? (w - 40) / 2 : 400;
	NSTableColumn* cName = [[NSTableColumn alloc] initWithIdentifier:@"portname"];
	[cName setTitle:nsstr(isOutput ? L("Output", "Uscita") : L("Input", "Ingresso"))];
	[cName setWidth:cw];
	[cName setEditable:NO];
	[t addTableColumn:cName];
	[cName release];
	NSTableColumn* cStat = [[NSTableColumn alloc] initWithIdentifier:@"portstatus"];
	[cStat setTitle:nsstr(L("Cable", "Cavo"))];
	[cStat setWidth:cw];
	[cStat setEditable:NO];
	[t addTableColumn:cStat];
	[cStat release];
	[t setHeaderView:nil];
	[t setAllowsMultipleSelection:NO];
	[t setAllowsEmptySelection:YES];
	[t setColumnAutoresizingStyle:NSTableViewUniformColumnAutoresizingStyle];
	[t setDataSource:controller];
	[t setDelegate:controller];
	[scroll setDocumentView:t];
	[t release];
	[content addSubview:scroll];
	[scroll release];
	return t;
}

// ── Menu bar ─────────────────────────────────────────────────────────────────
// Build the whole menu tree. UNICA FONTE DI VERITÀ per le due interfacce: lo standalone lo
// consegna a NSApp come menu bar vera, il plugin lo tiene come DATO e non lo mostra mai —
// la riga di pulsanti del pannello legge da qui (vedi itemsFromNSMenu / openTopMenu). Così
// aggiungere una voce la fa comparire in entrambe, e non possono divergere.
//
// `hosted` = dentro una DAW. Cambia pochissimo, e solo dove una voce sarebbe una trappola
// invece che un comando; il resto è identico apposta.
//
// Il chiamante possiede il menu restituito.
static NSMenu* buildMenuTree(AccessibleWindow* self, bool hosted) {
	NSMenu* mainMenu = [[NSMenu alloc] init];

	auto fpreset = [&](NSMenu* m, const std::string& label, float* s, float v) {
		addCmd(self, m, label, [s, v]() { *s = v; }, [s, v]() { return *s == v; });
	};

	// ── Application menu (first; its title is replaced by the app name) ─────────
	// Hosted there is no application menu to speak of: l'applicazione è la DAW, e
	// "Nascondi Rack" / "Esci da Rack" agirebbero su di lei.
	if (!hosted) {
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
	}

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
	// Hosted queste due sarebbero trappole, non comandi: nascondere il layer toglierebbe
	// l'UNICA interfaccia che il plugin ha (non resta una menu bar da cui riaprirla), e lo
	// schermo intero agirebbe sulla finestra GL che il plugin tiene nascosta.
	if (!hosted) {
		addCmd(self, view, L("Toggle accessible interface", "Mostra/nascondi interfaccia accessibile"), [self]() {
			setLayerVisible(self, !self->internal->visible);
		}, nullptr, @"a", NSEventModifierFlagCommand | NSEventModifierFlagShift);
		axSep(view);
		addCmd(self, view, L("Fullscreen", "Schermo intero"), [self]() {
			pushCommand(self, []() { APP->window->setFullScreen(!APP->window->isFullScreen()); });
		}, []() { return APP->window->isFullScreen(); });
	}

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

	return mainMenu;
}


// Standalone: l'albero diventa la menu bar dell'applicazione.
static void buildMenuBar(AccessibleWindow* self) {
	NSMenu* mainMenu = buildMenuTree(self, false);
	[NSApp setMainMenu:mainMenu];
	[mainMenu release];
}

AccessibleWindow* AccessibleWindow::create(void* glfwWindow, bool pluginMode) {
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
	self->internal->pluginMode = pluginMode;

	NSRect frame = [rackWindow frame];
	CGFloat w = frame.size.width, h = frame.size.height;
	NSWindow* panel = [[AXPanelWindow alloc]
	    initWithContentRect:NSMakeRect(0, 0, w, h)
	              styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
	                backing:NSBackingStoreBuffered
	                  defer:NO];
	// Hosted, this window sits among the DAW's own in Command+` and in its Window menu, so
	// it says which product it is; standalone it is the Rack window's layer.
	[panel setTitle:pluginMode ? @"MetaRack" : @"Rack"];
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
	NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 22, w, contentH(self, h))];
	[scroll setHasVerticalScroller:YES];
	[scroll setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

	RackAXTableView* table = [[RackAXTableView alloc]
	    initWithFrame:NSMakeRect(0, 0, w, contentH(self, h))];
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

	// LIBRARY outline, same geometry as the RACK table but hidden until switched to.
	NSScrollView* libScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 22, w, contentH(self, h))];
	[libScroll setHasVerticalScroller:YES];
	[libScroll setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
	[libScroll setHidden:YES];

	RackAXOutlineView* outline = [[RackAXOutlineView alloc]
	    initWithFrame:NSMakeRect(0, 0, w, contentH(self, h))];
	outline->owner = self;
	NSTableColumn* lcol = [[NSTableColumn alloc] initWithIdentifier:@"lib"];
	[lcol setWidth:w > 80 ? w - 40 : 800];
	[lcol setEditable:NO];
	[outline addTableColumn:lcol];
	[outline setOutlineTableColumn:lcol];
	[lcol release];
	[outline setHeaderView:nil];
	[outline setAllowsMultipleSelection:NO];
	[outline setAllowsEmptySelection:YES];
	[outline setDataSource:controller];
	[outline setDelegate:controller];
	[libScroll setDocumentView:outline];
	[outline release];
	[content addSubview:libScroll];
	[libScroll release];
	self->internal->libraryOutline = outline;

	// PARAM table: two columns (name, value), same geometry, hidden until switched to.
	NSScrollView* paramScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 22, w, contentH(self, h))];
	[paramScroll setHasVerticalScroller:YES];
	[paramScroll setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
	[paramScroll setHidden:YES];

	RackAXParamTableView* paramTable = [[RackAXParamTableView alloc]
	    initWithFrame:NSMakeRect(0, 0, w, contentH(self, h))];
	paramTable->owner = self;
	CGFloat pcw = w > 80 ? (w - 40) / 2 : 400;
	NSTableColumn* pcName = [[NSTableColumn alloc] initWithIdentifier:@"pname"];
	[pcName setTitle:nsstr(L("Parameter", "Parametro"))];
	[pcName setWidth:pcw];
	[pcName setEditable:NO];
	[paramTable addTableColumn:pcName];
	[pcName release];
	NSTableColumn* pcValue = [[NSTableColumn alloc] initWithIdentifier:@"pvalue"];
	[pcValue setTitle:nsstr(L("Value", "Valore"))];
	[pcValue setWidth:pcw];
	[pcValue setEditable:NO];
	[paramTable addTableColumn:pcValue];
	[pcValue release];
	[paramTable setHeaderView:nil];
	[paramTable setAllowsMultipleSelection:NO];
	[paramTable setAllowsEmptySelection:YES];
	[paramTable setColumnAutoresizingStyle:NSTableViewUniformColumnAutoresizingStyle];
	[paramTable setDataSource:controller];
	[paramTable setDelegate:controller];
	[paramScroll setDocumentView:paramTable];
	[paramTable release];
	[content addSubview:paramScroll];
	[paramScroll release];
	self->internal->paramTable = paramTable;

	// OUTPUT / INPUT port tables, same geometry, hidden until switched to.
	self->internal->outputTable = buildPortTable(self, content, controller, true, w, h);
	self->internal->inputTable  = buildPortTable(self, content, controller, false, w, h);

	// CONTEXT_MENU table: single column, same geometry, hidden until switched to.
	NSScrollView* ctxScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 22, w, contentH(self, h))];
	[ctxScroll setHasVerticalScroller:YES];
	[ctxScroll setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
	[ctxScroll setHidden:YES];

	RackAXContextTableView* ctxTable = [[RackAXContextTableView alloc]
	    initWithFrame:NSMakeRect(0, 0, w, contentH(self, h))];
	ctxTable->owner = self;
	NSTableColumn* ctxCol = [[NSTableColumn alloc] initWithIdentifier:@"ctx"];
	[ctxCol setWidth:w > 80 ? w - 40 : 800];
	[ctxCol setEditable:NO];
	[ctxTable addTableColumn:ctxCol];
	[ctxCol release];
	[ctxTable setHeaderView:nil];
	[ctxTable setAllowsMultipleSelection:NO];
	[ctxTable setAllowsEmptySelection:YES];
	[ctxTable setColumnAutoresizingStyle:NSTableViewUniformColumnAutoresizingStyle];
	[ctxTable setDataSource:controller];
	[ctxTable setDelegate:controller];
	[ctxScroll setDocumentView:ctxTable];
	[ctxTable release];
	[content addSubview:ctxScroll];
	[ctxScroll release];
	self->internal->contextTable = ctxTable;

	instance = self;

	if (!pluginMode) {
		// Native menu bar (App/File/Edit/View/Engine/Library/Help). Its key equivalents
		// provide the global shortcuts (⌘N/⌘S/⌘Z…) and the toggle (⇧⌘A), so no event
		// monitor is needed. GLFW left NSApp without a menu (GLFW_COCOA_MENUBAR = FALSE).
		//
		// Hosted, NSApp's main menu belongs to the DAW: replacing it would take the host's
		// own menus away, so there the same tree becomes the panel's menu row (below).
		AXMenuTarget* menuTarget = [[AXMenuTarget alloc] init];
		menuTarget->owner = self;
		self->internal->menuTarget = menuTarget;
		buildMenuBar(self);

		if (settings::accessibleLayerVisible)
			setLayerVisible(self, true);
	}
	else {
		// ── Menu row ────────────────────────────────────────────────────────────────
		// La menu bar di sistema è della DAW, quindi gli stessi menu entrano nel pannello
		// come riga di pulsanti. L'albero è quello che lo standalone darebbe a NSApp: qui
		// non viene mai mostrato come NSMenu, lo leggono i pulsanti (vedi itemsFromNSMenu).
		AXMenuTarget* menuTarget = [[AXMenuTarget alloc] init];
		menuTarget->owner = self;
		self->internal->menuTarget = menuTarget;
		self->internal->menuTree = buildMenuTree(self, true);

		AXMenuButtonTarget* buttonTarget = [[AXMenuButtonTarget alloc] init];
		buttonTarget->owner = self;
		self->internal->menuButtonTarget = buttonTarget;

		NSView* row = [[NSView alloc] initWithFrame:NSMakeRect(0, h - kMenuRowH, w, kMenuRowH)];
		[row setAutoresizingMask:(NSViewWidthSizable | NSViewMinYMargin)];
		CGFloat bx = 4;
		int topIndex = 0;
		for (NSMenuItem* top in [self->internal->menuTree itemArray]) {
			NSString* title = [top title];
			if (![top submenu] || !title || ![title length]) {
				topIndex++;
				continue;
			}
			AXMenuButton* b = [[AXMenuButton alloc]
			    initWithFrame:NSMakeRect(bx, 3, 100, kMenuRowH - 6)];
			b->owner = self;
			[b setTitle:title];
			[b setBezelStyle:NSBezelStyleRounded];
			[b setTarget:buttonTarget];
			[b setAction:@selector(pressed:)];
			// Il tag è l'indice NEL MENU TREE, non nella riga: openTopMenu ci indicizza
			// dentro, quindi deve contare anche le voci eventualmente saltate.
			[b setTag:topIndex];
			[b sizeToFit];
			NSRect bf = [b frame];
			bf.origin = NSMakePoint(bx, 3);
			bf.size.height = kMenuRowH - 6;
			[b setFrame:bf];
			[row addSubview:b];
			bx += bf.size.width + 4;
			[b release];
			topIndex++;
		}
		// In fondo alla lista dei subview, quindi in TESTA all'ordine con cui VoiceOver
		// percorre il pannello: la riga di menù si legge per prima, come una menu bar.
		[content addSubview:row positioned:NSWindowBelow relativeTo:nil];
		self->internal->menuRow = row;
		INFO("Accessible (macOS): menu row with %d buttons", (int)[[row subviews] count]);
		[row release];

		// ── I nostri tasti sono NOSTRI ───────────────────────────────────────────────
		// Dentro una DAW ogni evento di tastiera passa da NSApp PRIMA di raggiungere una
		// finestra, e l'host lega lì le proprie azioni: Reaper si prende F2 ("rename FX
		// instance") senza che il pannello lo veda mai. Un local monitor gira in quello
		// stesso stadio, e arrivando prima può consegnare l'evento direttamente alla nostra
		// catena di responder e restituire nil, così l'host non ha occasione di vederlo.
		//
		// Non è un dirottamento indiscriminato: panelOwnsKeyEvent() pretende che il
		// pannello sia la finestra chiave e che il fuoco sia su una delle nostre liste.
		// Fuori da quelle condizioni l'evento prosegue intatto verso l'host.
		self->internal->keyMonitor =
		    [[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
		              handler:^NSEvent* (NSEvent * e) {
			if (!panelOwnsKeyEvent(self, e))
				return e;
			// I tasti funzione sono quelli che gli host rivendicano più spesso: lasciarne
			// traccia rende evidente, dal log, chi li ha avuti.
			unsigned short kc = [e keyCode];
			if (kc == 122 || kc == 120 || kc == 99 || kc == 118 || kc == 96)
				INFO("Accessible (macOS): tasto funzione (keyCode %d) intercettato per MetaRack", (int) kc);
			[[self->internal->panel firstResponder] keyDown:e];
			return nil;   // consumato: l'host non lo vedrà
		}] retain];

		// No Rack run loop here: keep the command queue moving from the host's.
		AXDrainTimerTarget* drainTarget = [[AXDrainTimerTarget alloc] init];
		drainTarget->owner = self;
		self->internal->drainTarget = drainTarget;
		self->internal->drainTimer = [[NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0
		                              target:drainTarget
		                              selector:@selector(tick:)
		                              userInfo:nil
		                              repeats:YES] retain];
		// The adapter decides when to show the layer (the host opening or closing its
		// editor), so the standalone's remembered visibility does not apply.

		// Ogni volta che il pannello torna a essere finestra chiave — con lo switcher di
		// VoiceOver, con Command+`, o perché l'utente ci clicca — il first responder deve
		// tornare sul controllo della vista attiva. AppKit non lo garantisce dopo un giro
		// resign/become, e un pannello con la chiave ma senza first responder manda i tasti
		// a NSApp, cioè alla DAW.
		self->internal->keyObserver =
		    [[[NSNotificationCenter defaultCenter]
		      addObserverForName:NSWindowDidBecomeKeyNotification
		      object:panel queue:nil
		      usingBlock:^(NSNotification * n) {
			(void) n;
			focusActiveControl(self);
		}] retain];
	}

	INFO("Accessible (macOS) window created (pluginMode=%d)", (int) pluginMode);
	return self;
}


void AccessibleWindow::setVisible(bool show) {
	if (internal)
		setLayerVisible(this, show);
}

AccessibleWindow::~AccessibleWindow() {
	if (internal) {
		// Stop the hosted drain timer before anything it could touch goes away.
		if (internal->drainTimer) {
			[internal->drainTimer invalidate];
			[internal->drainTimer release];
			internal->drainTimer = nil;
		}
		if (internal->drainTarget) {
			((AXDrainTimerTarget*) internal->drainTarget)->owner = nullptr;
			[(id) internal->drainTarget release];
			internal->drainTarget = nil;
		}
		if (internal->menuButtonTarget) {
			((AXMenuButtonTarget*) internal->menuButtonTarget)->owner = nullptr;
			[(id) internal->menuButtonTarget release];
			internal->menuButtonTarget = nil;
		}
		if (internal->menuTree) {
			[internal->menuTree release];
			internal->menuTree = nil;
		}
		// Monitor e observer sopravviverebbero al pannello che osservano.
		if (internal->keyMonitor) {
			[NSEvent removeMonitor:internal->keyMonitor];
			[(id) internal->keyMonitor release];
			internal->keyMonitor = nil;
		}
		if (internal->keyObserver) {
			[[NSNotificationCenter defaultCenter] removeObserver:internal->keyObserver];
			[(id) internal->keyObserver release];
			internal->keyObserver = nil;
		}
		// Hosted, the main menu is the DAW's: clearing it would leave the host without its
		// menus for the rest of the session.
		if (!internal->pluginMode)
			[NSApp setMainMenu:nil];
		cleanupContextMenu(this);   // free any detached appendContextMenu() menus

		// Detach the datasource/delegate from every view before releasing the controller.
		// AppKit holds these as *unretained* refs, and the panel (with its tables) can
		// outlive this destructor — at app termination the window sits in an autorelease
		// pool and gets a final redraw after we return. That redraw would call back into
		// the freed controller and read its dangling `owner`, crashing in
		// tableView:objectValueForTableColumn:row:. Clearing the refs (and the owner)
		// severs that path. Messaging nil is a no-op, so unset views are harmless.
		NSTableView* dataViews[] = { internal->rackTable, internal->paramTable,
		                             internal->outputTable, internal->inputTable,
		                             internal->contextTable };
		for (NSTableView* v : dataViews) {
			[v setDataSource:nil];
			[v setDelegate:nil];
		}
		[internal->libraryOutline setDataSource:nil];
		[internal->libraryOutline setDelegate:nil];
		if (internal->controller)
			((RackAXController*) internal->controller)->owner = nullptr;

		if (internal->panel) {
			[internal->panel orderOut:nil];
			[internal->panel release];
		}
		if (internal->libraryRoots)
			[internal->libraryRoots release];
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

	// Release a momentary button once its high window elapses (the Win32 SetTimer
	// equivalent, polled from the main loop). Re-validate the module against the
	// engine first: it may have been removed during the brief high window.
	if (internal->momentaryModule && system::getTime() >= internal->momentaryReleaseTime) {
		engine::Module* mod = internal->momentaryModule;
		if (APP && APP->engine && APP->engine->getModule(mod->id) == mod) {
			if (engine::ParamQuantity* pq = mod->getParamQuantity(internal->momentaryParamId))
				pq->setValue(pq->minValue);
		}
		internal->momentaryModule = nullptr;
		internal->momentaryParamId = -1;
	}

	// Tier-B learn mode: poll the learning cell. If it was deselected externally, cancel;
	// otherwise announce any change to its text (the learned MIDI assignment).
	if (internal->learningCell && APP && APP->event) {
		if (APP->event->getSelectedWidget() != internal->learningCell) {
			internal->learningCell = nullptr;
			internal->learningLastText.clear();
		}
		else {
			std::string newText = internal->learningCell->text;
			if (newText != internal->learningLastText) {
				internal->learningLastText = newText;
				setStatus(this, L("Value updated: ", "Valore aggiornato: ") + newText + ".");
			}
		}
	}

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
