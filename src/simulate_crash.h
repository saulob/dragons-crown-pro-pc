#ifndef DCP_SIMULATE_CRASH_H
#define DCP_SIMULATE_CRASH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Development only: DCP_SIMULATE_CRASH=1 and nothing else arms the simulated crash. */
int dcp_simulate_crash_requested(const char *value);

/* Windows: a real access violation when DCP_SIMULATE_CRASH=1, through whatever fault handling is installed. Other values and
 * other platforms do nothing; Linux says it is unsupported. Call after the log and the fault handlers are ready. */
void dcp_simulate_crash_from_environment(void);

#ifdef __cplusplus
}
#endif
#endif
