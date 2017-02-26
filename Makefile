MODULES = bgw_replstatus

PG_CONFIG=pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
