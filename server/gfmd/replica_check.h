/*
 * $Id: replica_check.h 8880 2014-02-11 13:48:18Z takuya-i $
 */

void replica_check_start(void);
void replica_check_signal_host_up(void);
void replica_check_signal_host_down(void);
void replica_check_signal_update_xattr(void);
void replica_check_signal_rename(void);
void replica_check_signal_rep_request_failed(void);
void replica_check_signal_rep_result_failed(void);
void replica_check_info(void);
