// Protezione macOS attorno all'init dei sottosistemi GUI di Rack — vedi rackhost.hpp.
//
// PERCHÉ ESISTE QUESTO FILE. Dentro una DAW il processo non è nostro, e glfwInit() (che
// window::init() chiama) ha due effetti PROCESS-WIDE che nello standalone sono innocui e
// qui non lo sono affatto:
//
//   1. `[NSApp setDelegate:_glfw.ns.delegate]` (dep/glfw/src/cocoa_init.m) SOSTITUISCE il
//      delegate dell'applicazione, che è quello della DAW. Da quel momento l'host smette
//      di ricevere i propri eventi applicativi (apertura file, richiesta di terminare…).
//      Il delegate di GLFW serve a una finestra che vive nel run loop di GLFW: la nostra
//      resta nascosta e non viene mai disegnata, quindi non ci perdiamo nulla a rimettere
//      subito quello dell'host.
//
//   2. `GLFW_COCOA_CHDIR_RESOURCES` (impostato a TRUE da window::init) fa una chdir alla
//      cartella Resources del main bundle — che qui è quello della DAW. Cambiare la
//      working directory dell'host è un effetto collaterale silenzioso e insidioso
//      (percorsi relativi dell'host che smettono di risolvere), quindi la ripristiniamo.
//
// Restano due residui accettati e non neutralizzabili dall'esterno: il monitor locale di
// eventi key-up che GLFW installa (inoltra i key-up con ⌘ alla key window) e la
// registrazione del default ApplePressAndHoldEnabled. Nessuno dei due tocca lo stato che
// la DAW usa per funzionare.

// window/Window.hpp definisce GLEW_STATIC e include glew.h, che DEVE venire prima di
// qualunque header OpenGL di sistema: Cocoa tira dentro gltypes.h, e glew.h invertito
// fallisce con "gltypes.h included before glew.h". Stesso ordine di
// src/accessible/AccessibleWindowMac.mm.
#include <window/Window.hpp>

#import <Cocoa/Cocoa.h>

#include <unistd.h> // chdir()

#include "rackhost.hpp"

#include <logger.hpp>
#include <system.hpp>
#include <ui/common.hpp>

using namespace rack;


namespace rackhost {
namespace mac {


void initGuiSubsystemsHosted() {
	@autoreleasepool {
		id hostDelegate = [NSApp delegate];
		const std::string hostCwd = system::getWorkingDirectory();

		ui::init();
		window::init();   // qui dentro glfwInit()

		// Rimetti ciò che glfwInit si è preso. Messaggiare nil è legale, quindi il caso
		// "l'host non aveva un delegate" si risolve da sé.
		if ([NSApp delegate] != hostDelegate) {
			[NSApp setDelegate:hostDelegate];
			INFO("Delegate di NSApp restituito alla DAW dopo glfwInit");
		}
		if (!hostCwd.empty() && system::getWorkingDirectory() != hostCwd) {
			chdir(hostCwd.c_str());
			INFO("Working directory dell'host ripristinata: %s", hostCwd.c_str());
		}
	}
}


} // namespace mac
} // namespace rackhost
