#pragma once
#include <arch.hpp>

#if defined ARCH_MAC

// macOS / VoiceOver accessibility layer.
//
// This is the Cocoa counterpart of the Win32 layer in accessible/AccessibleWindow.hpp.
// It mirrors that layer's public surface (a singleton created from the main Rack
// window, plus a drainCommands() hook called from the run loop) so the wiring in
// adapters/standalone.cpp and src/window/Window.cpp stays symmetric between platforms.
//
// All Cocoa types live in AccessibleWindowMac.mm; this header stays free of
// AppKit/GLFW types (PIMPL via Internal) so plain C++ translation units can include it.

namespace rack {
namespace accessible {

struct AccessibleWindow {
	struct Internal;
	Internal* internal = nullptr;

	// There is only ever one accessible window, so the run loop can reach it.
	static AccessibleWindow* instance;

	// glfwWindow: the GLFWwindow* of the main Rack window, passed as void* to keep
	// this header free of GLFW/Cocoa types. The .mm resolves the backing NSWindow
	// via glfwGetCocoaWindow().
	//
	// pluginMode: true when Rack is hosted inside a DAW (the VST3/CLAP adapters) rather
	// than running standalone. Three things change, all of them because the process
	// belongs to the host and not to us: we must not touch NSApp's main menu (it is the
	// DAW's), the panel cannot be a child window of the Rack window (which stays hidden,
	// and a child window is ordered out with its parent), and nothing calls
	// drainCommands() for us — there is no Rack run loop — so the layer drives it from
	// its own timer on the host's run loop.
	static AccessibleWindow* create(void* glfwWindow, bool pluginMode = false);
	~AccessibleWindow();

	// Show or hide the accessible layer. The standalone toggles it from the menu bar
	// (⇧⌘A); the plugin adapters drive it when the host opens or closes the editor.
	void setVisible(bool show);

	// Run any commands queued from Cocoa event handlers. MUST be called from the
	// main run loop right after glfwPollEvents() — see Window::step(). Mutations of
	// the widget tree are queued instead of run inline because Cocoa event handlers
	// run at a reentrancy point where tearing down widgets/framebuffers can crash.
	void drainCommands();

	// True while the accessible layer is shown. The run loop uses this to pump Cocoa
	// events (servicing VoiceOver promptly) instead of sleeping blindly during the
	// frame-rate-limit idle — see Window::step().
	bool isVisible();
};

} // namespace accessible
} // namespace rack

#endif // ARCH_MAC
