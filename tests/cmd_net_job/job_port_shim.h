#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Host-only port shim for cmd_net_job tests. */
void job_test_port_reset(void);
void job_test_port_install(void);

void job_test_set_ble_connected(bool on);
void job_test_set_cleanup_result(bool ok);

int job_test_rp_count(void);
int job_test_tc_count(void);
int job_test_nc_count(void);
int job_test_clear_count(void);
