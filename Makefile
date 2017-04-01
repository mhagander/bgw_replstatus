MODULES = bgw_replstatus

PG_CONFIG=pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

NC = nc
NC_ARGS = -dq1
LISTEN_ADDRESS = 127.0.0.1
LISTEN_PORT = 5400

installcheck:
	[ "$(shell $(NC) $(NC_ARGS) $(LISTEN_ADDRESS) $(LISTEN_PORT))" = "MASTER" ]
