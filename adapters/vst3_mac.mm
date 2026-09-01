// Segnaposto NSView dell'editor VST3 su macOS — la controparte Cocoa del pannellino Win32
// in adapters/vst3.cpp.
//
// VST3 prevede una sola GUI: IPlugView, che l'host INCORPORA nella propria finestra editor
// via attached(parent). MetaRack però non ha nulla da incorporare — la sua UI è il pannello
// accessibile top-level, l'unico che VoiceOver può leggere. Quindi nell'editor della DAW
// mettiamo un pannellino segnaposto che dice dov'è la UI vera, e attached()/removed()
// mostrano e nascondono il pannello. Il pulsante "apri editor" della DAW resta così
// significativo.
//
// Il segnaposto è deliberatamente MINIMO: una NSView con dentro una static text. Non ha
// controlli, perché ogni controllo qui sarebbe una trappola per chi naviga con VoiceOver
// (sembrerebbe la UI, e non lo è). Serve solo a due cose: dire dove andare, e rimbalzare il
// focus sul pannello se l'host ce lo consegna.

#import <Cocoa/Cocoa.h>

#include "vst3_mac.hpp"


// La view del segnaposto. L'unico comportamento è il rimbalzo del focus: se l'host ci
// rende primi responder (l'utente ha aperto l'editor, o ci è arrivato con Tab), non è qui
// che deve restare.
@interface MetaRackPlaceholderView : NSView {
@public
	void (*onFocus)(void*);
	void* userData;
}
@end

@implementation MetaRackPlaceholderView

- (BOOL)acceptsFirstResponder {
	return YES;
}

- (BOOL)becomeFirstResponder {
	if (onFocus)
		onFocus(userData);
	return YES;
}

// Invio/Spazio come rete di sicurezza, se il rimbalzo automatico non scattasse.
- (void)keyDown:(NSEvent*)event {
	NSString* chars = [event charactersIgnoringModifiers];
	if ([chars length] == 1) {
		unichar c = [chars characterAtIndex:0];
		if (c == '\r' || c == 3 || c == ' ') {
			if (onFocus)
				onFocus(userData);
			return;
		}
	}
	[super keyDown:event];
}

@end


namespace metarack {
namespace vst3mac {


void* createPlaceholder(void* parentNSView, const char* text,
                        void (*onFocus)(void*), void* userData) {
	if (!parentNSView)
		return nullptr;

	@autoreleasepool {
		NSView* parent = (NSView*) parentNSView;
		NSRect frame = NSMakeRect(0, 0, kWidth, kHeight);

		MetaRackPlaceholderView* view = [[MetaRackPlaceholderView alloc] initWithFrame:frame];
		view->onFocus = onFocus;
		view->userData = userData;
		[view setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

		NSString* message = [NSString stringWithUTF8String:(text ? text : "MetaRack")];

		NSTextField* label = [[NSTextField alloc] initWithFrame:NSMakeRect(8, 8, kWidth - 16, kHeight - 16)];
		[label setStringValue:message];
		[label setBezeled:NO];
		[label setDrawsBackground:NO];
		[label setEditable:NO];
		[label setSelectable:NO];
		[label setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
		[[label cell] setWraps:YES];
		[view addSubview:label];
		[label release];

		// Anche la view contenitore porta l'etichetta: alcuni host danno il focus al
		// contenitore, e VoiceOver deve leggere qualcosa di sensato anche allora.
		[view setAccessibilityLabel:message];

		[parent addSubview:view];
		return (void*) view;   // il chiamante lo tiene finché non chiama destroyPlaceholder
	}
}


void destroyPlaceholder(void* placeholder) {
	if (!placeholder)
		return;
	@autoreleasepool {
		MetaRackPlaceholderView* view = (MetaRackPlaceholderView*) placeholder;
		// Niente più rimbalzi mentre la view muore: l'host potrebbe spostare il focus
		// proprio ora, e il callback punta a una RackPlugView in via di distruzione.
		view->onFocus = nullptr;
		view->userData = nullptr;
		[view removeFromSuperview];
		[view release];
	}
}


void resizePlaceholder(void* placeholder, int x, int y, int w, int h) {
	if (!placeholder)
		return;
	@autoreleasepool {
		[(NSView*) placeholder setFrame:NSMakeRect(x, y, w, h)];
	}
}


} // namespace vst3mac
} // namespace metarack
