seztuner: Reversed Sezmi Tuner API
========

Use this to talk to a Sezmi TUN-01 6V DC tuner. It's similar to (but a little worse than) the first-revision HD Homerun.

This code will find it on your network and scan for TV channels.

This does not play audio or video.

# Example

```
$ make
g++ $CFLAGS -o .obj/main.o -c main.cpp
g++ $CFLAGS -o .obj/iface.o -c iface.cpp
g++ $CFLAGS -o .obj/socket.o -c socket.cpp
g++ $CFLAGS -o .obj/tuner.o -c tuner.cpp
g++ $CFLAGS -o .obj/mpgts.o -c mpgts.cpp
g++ $CFLAGS -o .obj/mpgatsc.o -c mpgatsc.cpp
g++ $CFLAGS -o sez .obj/main.o .obj/iface.o .obj/socket.o .obj/tuner.o .obj/mpgts.o .obj/mpgatsc.o -lpthread
```
It compiled ok
```
$ ./sez
Error: no interfaces have 169.254.0.0/16
$
```
You need to add an address in the range 169.254.0.0/16 (AutoIP). First run sudo ifconfig to check your network interface name. If it is "eth0" then add ":1" to it, like so:
```
$ sudo ifconfig eth0:1 up 169.254.5.1 netmask 255.255.0.0
SIOCSIFFLAGS: Cannot assign requested address
```
The SIOCSIFFLAGS error is meaningless, so ignore it. Run sez again:
```
$ ./sez
Warn: eth0:1 mac address 10:20:30:01:02:03
Warn: tuners will only send video if you "sudo ifconfig eth0:1 hw ether 00:21:33:01:02:03"
```
The sezmi TUN-01 will only talk to you if your [mac address(http://en.wikipedia.org/wiki/MAC_address)] startrs with 00:21:33:xx:xx:xx so copy/paste the command: (Note that it may confuse any open connections so close any open ssh sessions, for example.)
```
$ sudo ifconfig eth0:1 hw ether 00:21:33:01:02:03
```
Now run sez again:
```
$ ./sez
./sez found 1 IP, probing in order found:
169.254.54.254 auto-detected -a1
169.254.54.254 scan... 26/26
.54.254 all carrier freqs: 2 3 4 5 6 11 15 18 21 23 36 38 41 43 45 47 49
.54.254 strong freqs: 2 5 15 18 23 38 43 49
```
It should now be working.
