/* Windows: the DLLs outside the Windows folder in the process at startup and the ones loaded until the game's Vulkan
 * presenter, so a crash report can be matched to the files involved. Failures are logged and never stop the game. */
#ifndef DCP_MODULE_DIAGNOSTICS_H
#define DCP_MODULE_DIAGNOSTICS_H

#include <stddef.h>
#include <stdint.h>
#include <windows.h>

/* Remembers the modules loaded now, early in main. A failure is reported by the functions below. It also starts the
 * record of module loads and unloads that the crash report reads. */
void module_diagnostics_baseline(void);

/* Logs each module of that list outside the Windows folder, except this program: path and base address. */
void module_diagnostics_report_startup(void);

/* Logs each module loaded since module_diagnostics_baseline() outside the Windows folder: path and base address. */
void module_diagnostics_report(void);

/* A short description of what `address` is: module and offset, a module unloaded earlier, or the kind of memory. 0 and an
 * empty string when there is nothing meaningful to say. Takes no locks. */
int module_diagnostics_describe(char *out, size_t size, uintptr_t address);

/* The "Crash:" lines of a fatal exception on stderr: startup phase, thread, `rip`, `target`, recent unloads and the
 * modules outside the Windows folder. Addresses inside [guest_base, guest_base + guest_size) are the game's. */
void module_diagnostics_report_crash(uintptr_t rip, uintptr_t target, uintptr_t guest_base, size_t guest_size);

/* "Crash: frame" lines unwound from `fault`; for a call into an address no function covers, also the "Crash: call site"
 * lines that decode the instruction that made it. */
void module_diagnostics_report_backtrace(const CONTEXT *fault, uintptr_t guest_base, size_t guest_size);

#endif
