/* The stage the process is in, kept for the crash report. */
#ifndef DCP_STARTUP_PHASE_H
#define DCP_STARTUP_PHASE_H

#ifdef __cplusplus
extern "C" {
#endif

/* `phase` must outlive the process. Nothing is logged. */
void dcp_startup_phase(const char *phase);
const char *dcp_startup_phase_current(void);

#ifdef __cplusplus
}
#endif

#endif
