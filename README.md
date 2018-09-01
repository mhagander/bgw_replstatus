bgw_replstatus
==============

`bgw_replstatus` is a tiny background worker to cheaply report the
replication status of a node. It's intended to be polled by a load
balancer such as `haproxy`.

When installed, a background worker will be started that listens on a
defined TCP port (configured `bgw_replstatus.port`). Any connection to
this port will get a TCP response back (no request necessary, response
will be sent immediately on connect) saying either `MASTER`,
`STANDBY` or `OFFLINE` depending on the current state of the node. The connection
is then automatically closed.

Using a background worker like this will make polling a lot more light
weight than making a full PostgreSQL connection, logging in, and
checking the status.

Installing
----------

Build and install is done using PGXS. As long as `pg_config` is
available in the path, build and install using:

```
$ make
$ make install
```

Once the binary is installed, it needs to be enabled in
`shared_preload_libraries` in postgresql.conf:

```
shared_preload_libraries = 'bgw_replstatus'
```

If other libraries are already configured for loading, it can be
appended to the end of the list. Order should not matter.

Configuration
-------------

By default, the background worker will listen to port 5400 on a
wild card IP address. There is *no* verification of the source done, so
protect the port with a proper host firewall!!!

To change the port, set the value of `bgw_replstatus.port` to another
value. Any TCP port above 1024 will work (but don't pick the same one
as PostgreSQL itself...).

To change the socket to bind to a specific IP address, set
`bgw_replstatus.bind` to an IP address, which will cause the
background worker to bind to this IP on the defined port.

There is no support for multiple ports or multiple IP addresses.

Example usage
-------------

In it's simplest form, you can just verify the status of your system
with `nc` or `telnet`:

```
$ nc localhost 5400
MASTER
```

Since the text coming back is easily identifiable, it's easy enough to
integrate with a load balancer such as haproxy. This example haproxy
configuration will show how to ensure that haproxy is connected to the
master node of the cluster, and automatically switches over to the
backup node if the master goes down and the backup is
promoted. Multiple backups can be used.

```
frontend test
	bind 127.0.0.1:5999
	default_backend pgcluster

backend pgcluster
	mode tcp
	option tcp-check
	tcp-check expect string MASTER
	server s1 127.0.0.1:5500 check port 5400
	server s2 127.0.0.1:5501 check port 5401 backup
	server s3 127.0.0.1:5502 check port 5402 backup
```

In this example all nodes are local, with postgres running on ports
5500/5501/5502 with `bgw_replstatus` bound to ports 5400/5401/5402
respectively.

Example testing replication delay
---------------------------------

In some cases you may want to only consider a standby as 'up' if
it's replication delay is below a certain threshold. In order to do this
you can write the threshold to the port when connecting and
if the replication delay is greater than the threshold the response
will be `OFFLINE`:

```
$ echo '30' | nc localhost 5400
STANDBY

$ echo '1' | nc localhost 5400
OFFLINE
```

Here is an example of how you would use this with haproxy:

```
tcp-check send 30
tcp-check expect rstring MASTER|STANDBY
```
