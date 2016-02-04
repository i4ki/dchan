# Dchan - distributed channel

Dchan is a server that exposes channels for inter-process communications
over a file tree interface. The channels are much like Go channels and 
can be used in the same way but between processes. 

Instead of implementing a new protocol for data exchange like AMQP, Dchan
uses a simple file interface. There's no need for client libraries for each
language (every language knows how to read and write from files).

Dchan is able to share the files in the network with the help of the
9P protocol. 

The benefits of this approach are:

* Simple interface. 
	- No need for client libraries for each language of the architecture.
* Language agnostic.
* Easy to test.
	- No need of mocking libraries or real AMQP server.
* Possible to use cat, echo, grep, etc, to debug the communications;

The drawbacks are:

* Network failures turns the mount point into a failed state in Unix.
	- not possible to use mount -o remount. 
	- Requires umount and then a new mount invocation.
	- Every file descriptor open will be invalid, requiring a new open(2).
* No data framing.
	- Use your own app convention to avoid fragmented messages.
	- Eg.: read(fd, MAXMSGSZ) and write(fd, msg, strlen(msg));

## Building and running

It's a Plan9 server.

```sh
term% hg https://bitbucket.org/tiago4orion/dchan
term% cd dchan
term% mk
term% ./dchan -h
Usage: dchan [-D] [-d] [-s srvname] [-m mptp]

term% ./dchan -s dchan -a tcp!*!6666
term%
```

The last command above will post a server called dchan into /srv, fork and start
listening for 9P messages on port 6666.

Now you can mount dchan's filesystems on Plan9 or unix machines.

## Mounting the filesystem

To mount in local plan9:

```sh
term% mount -c /srv/dchan /n/dchan
```

Linux:

```bash
$ mount -t 9p -o port=6666,sync,cache=none <ip-of-dchan-server> /n/dchan
```

## Message passing

To create a channel is simple as creating a new file in the dchan directory.
For example, in Linux you can connect to the file server and create a channel
using the `touch` command.

```bash
$ mount -t 9p -o port=6666,sync,cache=none <ip-of-dchan> /n/dchan
$ cd /n/dchan
$ ls
ctl
$ touch pipeline
```

The file `pipeline` created is an unbuffered channel. Any attempt to read(2) from
this file will block until other process write(2) something. In the same way,
any attempt to write(2) will block until some other process read data. It's a 
well know concept for Go developers or people who already used CSP-style concurrency.

```bash
$ cat pipeline # will block in the read(2) syscall
```

In some other machine or in another mounting point you can send data to this 
channel using a simple echo. 

```bash
$ mount -t 9p -o port=6666,sync,cache=none <ip-of-dchan> /n/dchan2
$ cd /n/dchan2
$ ls
ctl pipeline
$ echo AAAAAAAAAAAAAa >> pipeline
$ 
```

The other side of the pipe will get the message.

## Architecture

Dchan is a Plan9 file server that exposes the Plan9 thread(2) channels with a file
tree interface. Every new 9P connection established will create one thread for
handle the subsequent requests and every created file in the tree will spawn 2 other
threads (one for read and one for write requests) and create a channel shared between 
this two threads.

The size of the channel is 0 (unbuffered) by default and it can be changed using the ctl file.

Every read request will block when the channel is empty. And every write request
will block the writer thread when the channel is full.

When the channel is unbuffered (or with size equals 0), the file server do not store
the messages, only transfer the written data from the writer thread to the reader, 
that will then deliver the data to the consumer.

The ctl file is used for channel settings and statistics. To increase the channel 
size write a line with the content below:

```bash
$ echo "/pipeline 256" >> ctl
```

The line above will allocate a channel with size 256.

You can read the ctl file to read statistics about transmission/receive rate and average throughput.

```bash
$ cat ctl
# filename | channel size | rx | tx | average rx | average tx
/pipeline	0	455	500	460	480
```

### Contributing

Please read: [Notes on Programming in C - Rob Pike](http://doc.cat-v.org/bell_labs/pikestyle)

