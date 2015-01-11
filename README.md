ChinaDNS
========

Fork form https://github.com/clowwindy/ChinaDNS

Use [DNS compression pointer mutation] instead of filtering bad ip addresses and delaying.

If you want to fix other weird things as well, you might also want to use [ShadowVPN].

Install
-------

* Linux / Unix

    [Download a release].

        ./configure && make

* OpenWRT
    * For ar71xx router:
      Download [precompiled] ipk file and install.

    * Build yourself:
      cd into [SDK] root, then

            pushd package
            git clone https://github.com/Pentiumluyu/ChinaDNS.git
            popd
            make menuconfig # select Network/ChinaDNS
            make -j
            make V=99 package/ChinaDNS/openwrt/compile

* Tomoto

    * Download [Tomato toolchain], build by yourself.
    * Uncompress the downloaded file to `~/`.
    * Copy the `brcm` directory under
      `~/WRT54GL-US_v4.30.11_11/tools/` to `/opt`, then

            export PATH=/opt/brcm/hndtools-mipsel-uclibc/bin/:/opt/brcm/hndtools-mipsel-linux/bin/:$PATH
            git clone https://github.com/Pentiumluyu/ChinaDNS.git
            cd ChinaDNS
            ./autogen.sh && ./configure --host=mipsel-linux --enable-static && make

Usage
-----

* Linux / Unix

    Run `sudo chinadns -c path_to_chnroute_file` on your local machine. 
    ChinaDNS creates a UDP DNS Server at `0.0.0.0:53`.

* OpenWRT

        opkg install ChinaDNS_1.x.x_ar71xx.ipk
        /etc/init.d/chinadns start

    (Optional) We strongly recommend you to set ChinaDNS as a upstream DNS
    server for dnsmasq instead of using ChinaDNS directly:

    1. Run `/etc/init.d/chinadns stop`
    2. Remove the 2 lines containing `iptables` in `/etc/init.d/chinadns`.
    3. Update `/etc/dnsmasq.conf` to use only 127.0.0.1#5353:

            no-resolv
            server=127.0.0.1#5353

    4. Restart chinadns and dnsmasq

Test if it works correctly:

    $ dig @192.168.1.1 www.youtube.com -p5353
    ; <<>> DiG 9.8.3-P1 <<>> @127.0.0.1 www.google.com -p5353
    ; (1 server found)
    ;; global options: +cmd
    ;; Got answer:
    ;; ->>HEADER<<- opcode: QUERY, status: NOERROR, id: 16179
    ;; flags: qr rd ra; QUERY: 1, ANSWER: 5, AUTHORITY: 0, ADDITIONAL: 0

    ;; QUESTION SECTION:
    ;www.google.com.            IN  A

    ;; ANSWER SECTION:
    www.google.com.     215 IN  A   173.194.127.50
    www.google.com.     215 IN  A   173.194.127.49
    www.google.com.     215 IN  A   173.194.127.48
    www.google.com.     215 IN  A   173.194.127.52
    www.google.com.     215 IN  A   173.194.127.51

    ;; Query time: 197 msec
    ;; SERVER: 127.0.0.1#5353(127.0.0.1)
    ;; WHEN: Thu Jan  1 02:37:16 2015
    ;; MSG SIZE  rcvd: 112

Currently ChinaDNS only supports UDP. Builtin OpenWRT init script works with
dnsmasq, which handles TCP. If you use it directly without dnsmasq, you need to
add a redirect rule for TCP:

    iptables -t nat -A PREROUTING -p tcp --dport 53 -j DNAT --to-destination 8.8.8.8:53

Advanced
--------

  usage: chinadns [-h] [-b BIND_ADDR] [-p BIND_PORT]
         [-c CHNROUTE_FILE] [-s DNS] [-v]
  Forward DNS requests.

    -h, --help            show this help message and exit
    -c CHNROUTE_FILE      path to china route file
                          if not specified, CHNRoute will be turned off
    -d                    enable bi-directional CHNRoute filter
    -b BIND_ADDR          address that listens, default: 127.0.0.1
    -p BIND_PORT          port that listens, default: 53
    -s DNS                DNS servers intended to use,
                          and the format should be "ip:port,ip:port"
                          default: 114.114.114.114,208.67.222.222:443,8.8.8.8
    -v                    verbose logging

    Online help: <https://github.com/Pentiumluyu/ChinaDNS>

About chnroute
--------------

You can generate latest chnroute.txt using this command:

    curl 'http://ftp.apnic.net/apnic/stats/apnic/delegated-apnic-latest' | grep ipv4 | grep CN | awk -F\| '{ printf("%s/%d\n", $4, 32-log($5)/log(2)) }' > chnroute.txt


License
-------
MIT

Bugs and Issues
----------------
Please visit [Issue Tracker]


[Issue Tracker]:        https://github.com/Pentiumluyu/ChinaDNS/issues?state=open
[Download precompiled]: https://sourceforge.net/projects/chinadns/files/dist/
[Download a release]:   https://github.com/Pentiumluyu/ChinaDNS/releases
[SDK]:                  http://wiki.openwrt.org/doc/howto/obtain.firmware.sdk
[ShadowVPN]:            https://github.com/clowwindy/ShadowVPN
[Tomato toolchain]:     http://downloads.linksysbycisco.com/downloads/WRT54GL_v4.30.11_11_US.tgz
[weird things]:         http://en.wikipedia.org/wiki/Great_Firewall_of_China#Blocking_methods
[DNS compression pointer mutation]: https://gist.github.com/klzgrad/f124065c0616022b65e5
[precompiled]:          https://github.com/Pentiumluyu/ChinaDNS/releases/tag/1.2.2
