# External research notes

## TinyUSB official USB Concepts

Source: https://docs.tinyusb.org/en/latest/reference/usb_concepts.html

The official documentation describes host enumeration as detection, reset, descriptor requests, address assignment, configuration, and class loading. It also states that the HCD abstraction includes `hcd_port_connect_status`, `hcd_port_reset`, `hcd_edpt_open`, and `hcd_edpt_xfer`.

The documented event architecture is deferred interrupt processing: the hardware handler captures controller events and pushes them into a central event queue; the application then calls `tuh_task()`/`tuh_task_ext()` in non-interrupt context to drain the queue and invoke class callbacks. This is directly relevant to the current C-OS observation where an EHCI transfer-complete event is generated but the next TinyUSB task-side control stage is not observed.

The same source identifies EHCI-style controllers as descriptor-based controllers whose hardware processes transfer descriptors asynchronously. Therefore the current investigation must verify both descriptor-chain ownership and the HCD-to-task event handoff, rather than treating root-port attach alone as enumeration success.

## Current C-OS evidence

The current diagnostic SMP8 run reaches EHCI initialization, root-port attach, the first dev-0 SETUP transfer, and successful 8-byte completion. It then records event enqueue but no next transfer or HID mount callback. The most likely investigation boundary is the C-OS TinyUSB OSAL/event consumer path after completion; full HID enumeration is not yet proven.

## NetSurf official JavaScript binding documentation

Source: https://github.com/netsurf-browser/netsurf/blob/master/docs/jsbinding.md

NetSurf documents its JavaScript bindings as a WebIDL-driven layer between the JavaScript engine and the browser's DOM/CSSOM. The binding tuple is engine + libdom + libcss + browser. The documentation emphasizes that attributes require real getters/setters, methods must call browser/libdom operations, and returned DOM nodes must be wrapped with stable engine-side node bindings. It also describes event and external-call paths as lifecycle-sensitive and warns that calls from outside normal execution must use the engine's protected call convention.

This supports implementing C-OS wrappers around real libdom nodes rather than synthetic detached objects, with stable wrapper identity and explicit mutation/reformat invalidation.

## LibDOM official project page

Source: https://www.netsurf-browser.org/projects/libdom/

LibDOM is described by NetSurf as a C implementation of the W3C DOM intended for use with NetSurf and other projects, licensed under the MIT License. C-OS can therefore expose libdom's existing node/attribute/tree semantics through QuickJS without replacing the DOM core, provided ownership and wrapper lifetime are handled correctly.

## QuickJS C API official developer guide

Source: https://quickjs-ng.github.io/quickjs/developer-guide/intro/

The official guide states that a `JSRuntime` is an object heap and that no multi-threading is supported inside one runtime. Separate runtimes cannot exchange JS objects. This is a critical constraint for C-OS: HTTP worker threads must not execute QuickJS APIs directly; they may only complete request data and notify the GUI/JS owner context, where Promise resolution, XHR state changes, and DOM mutations are performed.

The guide also requires explicit `JS_DupValue`/`JS_FreeValue` ownership handling. C-backed objects should use a registered `JSClassID`, `JS_NewObjectClass`, and `JS_GetOpaque`/`JS_SetOpaque`; finalizers release C resources but must not execute JavaScript. `JS_SetClassProto` defines the prototype per runtime/context. Exceptions returned as `JS_EXCEPTION` must be tested and handled explicitly.

These rules will govern the DOM wrapper cache, event listener retention, storage objects, XHR/fetch request records, and worker-to-owner completion queue.

## TinyUSB upstream source comparison

A shallow clone of the official TinyUSB repository was compared with the vendored C-OS host stack. The current upstream `host/usbh.c` contains a pending control-transfer FIFO and dispatches queued control requests when the shared control stage becomes idle. The C-OS fork has the single in-flight control-transfer state but lacks that pending FIFO. This is a real compatibility/performance gap for concurrent enumeration/class requests and will be addressed only with the corresponding queue ownership and callback ordering preserved.

The upstream OSAL `osal_none` queue implementation still treats bare-metal operation as a single-context model and protects queue access by interrupt masking. C-OS currently calls the HCD from cooperative polling, so event production and consumption must use a clearly defined context contract; merely changing an ISR boolean is not sufficient to make the stack SMP-safe. The C-OS USB service must remain serialized or gain an explicit SMP-safe queue/lock boundary.

## QEMU official USB emulation documentation

Source: https://qemu-project.gitlab.io/qemu/system/devices/usb.html

QEMU documents that an EHCI controller should be given an explicit ID, producing a bus name such as `ehci.0`, and that USB 2.0 devices should be attached to that bus. USB 1.1 devices use UHCI/OHCI, while USB 2.0 devices use EHCI. This validates the q35 test command's explicit `ich9-usb-ehci1,id=ehci` and `bus=ehci.0` configuration for a USB 2.0 mouse. The documentation also warns that host pass-through is experimental and recommends unplug/replug on relaunch for some devices; the current virtual `usb-mouse` test avoids host pass-through ambiguity.

