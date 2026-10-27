/* Windows: the DLLs outside the Windows folder in the process at startup and the ones loaded until the game's Vulkan
 * presenter, so a crash report can be matched to the files involved. Failures are logged and never stop the game. */
#ifndef DCP_MODULE_DIAGNOSTICS_H
#define DCP_MODULE_DIAGNOSTICS_H

/* Remembers the modules loaded now, early in main. A failure is reported by the functions below. */
void module_diagnostics_baseline(void);

/* Logs each module of that list outside the Windows folder, except this program: path and base address. */
void module_diagnostics_report_startup(void);

/* Logs each module loaded since module_diagnostics_baseline() outside the Windows folder: path and base address. */
void module_diagnostics_report(void);

#endif
