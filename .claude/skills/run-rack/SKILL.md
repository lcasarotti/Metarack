---
name: run-rack
description: Build and launch VCV Rack with the debug flag for live testing of changes. Equivalent to make run.
disable-model-invocation: false
---

Run `make run` from the project root (`C:\Users\Luca Casarotti\Rack`).

This builds Rack if needed and then launches it with the `-d` debug flag so debug output appears in the terminal. Tell the user that Rack is launching and they can close it normally when done testing. If the build step fails, surface the compiler error clearly.
