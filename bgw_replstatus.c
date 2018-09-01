#include "stdio.h"
#include "string.h"
#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "access/xlog.h"
#include "utils/guc.h"
#include "pgstat.h"
#include "utils/timestamp.h"
#include "replication/walreceiver.h"

#include <unistd.h>
#include <arpa/inet.h>

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(bgw_replstatus_launch);

void _PG_init(void);
void bgw_replstatus_main(Datum d) pg_attribute_noreturn();
long read_max_replication_delay(int socketFd, int *max_sd);
long __read_max_replication_delay(int socketFd);

/* flags set by signal handlers */
static volatile sig_atomic_t got_sigterm = false;

/* config */
int portnum = 5400;
char *bindaddr = NULL;

enum STATUS {
	master, standby, offline,
};

static const char *STATUS_NAMES[] = {
	"MASTER", "STANDBY", "OFFLINE"
};

fd_set master_set;

/*
 * Perform a clean shutdown on SIGTERM. To do that, just
 * set a boolean in the sig handler and then set our own
 * latch to break the main loop.
 */
static void
bgw_replstatus_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_sigterm = true;
	SetLatch(MyLatch);
	errno = save_errno;
}

long read_max_replication_delay(int socketFd, int *max_sd_p)
{
	int desc_ready = 0;
	long max_repl_delay = 0;
	int rc = 0;
	int max_sd = *max_sd_p;
	struct timeval timeout;

	timeout.tv_sec  = 0;
	timeout.tv_usec = 0.1 * USECS_PER_SEC;

	FD_SET(socketFd, &master_set);
	if (socketFd > max_sd)
		max_sd = socketFd;

	rc = select(max_sd + 1, &master_set, NULL, NULL, &timeout);

	if (rc <= 0)
	{
		ereport(LOG, (errmsg("bgw_replstatus: unable to read max_replication_delay: %m")));
		return 0;
	}
	desc_ready = rc;
	for (int i=0; i <= max_sd && desc_ready > 0; ++i)
	{
		if (FD_ISSET(i, &master_set))
		{
			desc_ready -= 1;
			max_repl_delay = __read_max_replication_delay(i);
		}
	}
	FD_CLR(socketFd, &master_set);
	if (socketFd == max_sd)
	{
		while (max_sd > 0 && FD_ISSET(max_sd, &master_set) == 0)
			max_sd -= 1;
	}
	return max_repl_delay;
}

long __read_max_replication_delay(int socketFd){
	char buffer[100];
	int r_len;
	long max_repl_delay = 0;
	r_len = read(socketFd, &buffer, sizeof(buffer));
	if (r_len > 0) {
		ereport(DEBUG5, (errmsg("bgw_replstatus: data read from socket %s: %m", buffer)));
		max_repl_delay = strtol(buffer, NULL, 10);
		if (errno == ERANGE){
			ereport(ERROR, (errmsg("bgw_replstatus: range error reading max_replication_delay: %m")));
			max_repl_delay = 0;
			errno = 0;
		}
		ereport(LOG, (errmsg("bgw_replstatus: max_replication_delay %ld: %m", max_repl_delay)));
	}
	else
	{
		ereport(LOG, (errmsg("bgw_replstatus: unable to read max_replication_delay: %m")));
	}
	return max_repl_delay;
}

void bgw_replstatus_main(Datum d)
{
	int enable = 1;
	int listensocket;
	struct sockaddr_in addr;
	int max_sd = 0;

	pqsignal(SIGTERM, bgw_replstatus_sigterm);

	BackgroundWorkerUnblockSignals();

	/* Setup our listening socket */
	listensocket = socket(AF_INET, SOCK_STREAM, 0);
	if (listensocket == -1)
		ereport(ERROR,
				(errmsg("bgw_replstatus: could not create socket: %m")));

	if (setsockopt(listensocket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) != 0)
		ereport(ERROR,
				(errmsg("bgw_replstatus: could not set socket option: %m")));

	if (!pg_set_noblock(listensocket))
		ereport(ERROR,
				(errmsg("bgw_replstatus: could not set non blocking socket: %m")));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
		addr.sin_port = htons(portnum);

	if (bindaddr == NULL || strlen(bindaddr) == 0)
		addr.sin_addr.s_addr = INADDR_ANY;
	else
	{
		if (inet_aton(bindaddr, &addr.sin_addr) == 0)
			ereport(ERROR,
					(errmsg("bgw_replstatus: could not translate IP address '%s'",
							bindaddr)));
	}

	if (bind(listensocket, &addr, sizeof(addr)) != 0)
		ereport(ERROR,
				(errmsg("bgw_replstatus: could not bind socket: %m")));

	if (listen(listensocket, 5) != 0)
		ereport(ERROR,
				(errmsg("bgw_replstatus: could not listen on socket: %m")));

	FD_ZERO(&master_set);

	/*
	 * Loop forever looking for new connections. Terminate on SIGTERM,
	 * which is sent by the postmaster when it wants us to shut down.
	 * XXX: we don't currently support changing the port at runtime.
	 */
	while (!got_sigterm)
	{
		int rc;

		rc = WaitLatchOrSocket(MyLatch,
							   WL_LATCH_SET | WL_POSTMASTER_DEATH | WL_SOCKET_READABLE,
							   listensocket,
							   -1
#if PG_VERSION_NUM >= 100000
							   /* 10.0 introduced PG_WAIT_EXTENSION */
							   ,PG_WAIT_EXTENSION);
#else
							   );
#endif
		ResetLatch(MyLatch);

		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);
		else if (rc & WL_SOCKET_READABLE)
		{
			const char *status_str;
			enum STATUS status;
			long max_replication_delay = 0;
			socklen_t addrsize = sizeof(addr);

			int worksock = accept4(listensocket, &addr, &addrsize, SOCK_NONBLOCK);
			if (worksock == -1)
			{
				ereport(LOG,
						(errmsg("bgw_replstatus: could not accept socket: %m")));
				continue;
			}

			status = RecoveryInProgress() ? standby : master;
			if (status == standby && !WalRcvRunning())
			{
				status = offline;
			}

			// Check replication delay
			if (status == standby)
			{
				XLogRecPtr replay = (int64) GetXLogReplayRecPtr(NULL);
				XLogRecPtr receive = (int64) GetWalRcvWriteRecPtr(NULL, NULL);
				TimestampTz xtime = GetLatestXTime();

				ereport(DEBUG5, (errmsg("bgw_replstatus: pg_last_wal_replay_lsn %lu", replay)));
				ereport(DEBUG5, (errmsg("bgw_replstatus: pg_last_wal_receive_lsn %lu", receive)));

				if (replay != receive && xtime != 0)
				{
					max_replication_delay = read_max_replication_delay(worksock, &max_sd);
					if (max_replication_delay > 0)
					{
						TimestampTz diff_s = (long) (GetCurrentTimestamp() - xtime) / USECS_PER_SEC;
						ereport(DEBUG5,
							(errmsg("bgw_replstatus: actual delay %lu : max allowed %li",
									diff_s, max_replication_delay)));
						status = (diff_s > max_replication_delay) ? offline : standby;
					}
				}
			}

			status_str = STATUS_NAMES[status];
			if (write(worksock, status_str, strlen(status_str)) != strlen(status_str))
			{
				ereport(LOG,
						(errmsg("bgw_replstatus: could not write %s: %m",
								status_str)));
				close(worksock);
				continue;
			}

			if (close(worksock) != 0)
			{
				ereport(LOG,
						(errmsg("bgw_replstatus: could not close working socket: %m")));
			}
		}
	}

	ereport(LOG,
			(errmsg("bgw_replstatus: shutting down")));

	close(listensocket);

	proc_exit(0);
}


/*
 * Initialization entrypoint
 */
void _PG_init(void)
{
	BackgroundWorker worker;

	/*
	 * Define our GUCs so we can get the configuration values.
	 */
	DefineCustomIntVariable("bgw_replstatus.port",
							"TCP port to bind to",
							NULL,
							&portnum,
							5400,
							1025,
							65535,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("bgw_replstatus.bind",
							   "IP address to bind to",
							   NULL,
							   &bindaddr,
							   "",
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);


	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Set up our bgworker
	 */
	MemSet(&worker, 0, sizeof(worker));

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_PostmasterStart;
	worker.bgw_restart_time = 30; /* Restart after 30 seconds -- just
									 so we don't end up in a hard loop
									 if something fails */
	sprintf(worker.bgw_library_name, "bgw_replstatus");
	sprintf(worker.bgw_function_name, "bgw_replstatus_main");
	worker.bgw_notify_pid = 0;
	snprintf(worker.bgw_name, BGW_MAXLEN, "bgw_replstatus");

	RegisterBackgroundWorker(&worker);
}
