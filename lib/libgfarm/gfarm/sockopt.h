/*
 * $Id: sockopt.h 5627 2011-12-31 11:10:50Z tatebe $
 */

struct gfarm_hostspec;

gfarm_error_t gfarm_sockopt_set_option(int, char *);
gfarm_error_t gfarm_sockopt_config_add(char *, struct gfarm_hostspec *);
gfarm_error_t gfarm_sockopt_apply_by_name_addr(int, const char *,
	struct sockaddr *);
gfarm_error_t gfarm_sockopt_listener_config_add(char *);
gfarm_error_t gfarm_sockopt_apply_listener(int);
