/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common_startup.hh"
#include "ws-auth.hh"
#include "secpoll-auth.hh"
#include <sys/time.h>
#include <sys/resource.h>
#include "dynhandler.hh"
#include "dnsseckeeper.hh"
#include "threadname.hh"
#include "misc.hh"
#include "query-local-address.hh"
#include "trusted-notification-proxy.hh"
#include "packethandler.hh"

#include <thread>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

bool g_anyToTcp;
bool g_8bitDNS;
#ifdef HAVE_LUA_RECORDS
bool g_doLuaRecord;
int g_luaRecordExecLimit;
time_t g_luaHealthChecksInterval{5};
time_t g_luaHealthChecksExpireDelay{3600};
#endif
typedef Distributor<DNSPacket,DNSPacket,PacketHandler> DNSDistributor;

ArgvMap theArg;
StatBag S;  //!< Statistics are gathered across PDNS via the StatBag class S
AuthPacketCache PC; //!< This is the main PacketCache, shared across all threads
AuthQueryCache QC;
AuthZoneCache g_zoneCache;
std::unique_ptr<DNSProxy> DP{nullptr};
std::unique_ptr<DynListener> dl{nullptr};
CommunicatorClass Communicator;
shared_ptr<UDPNameserver> N;
double avg_latency{0.0};
unique_ptr<TCPNameserver> TN;
static vector<DNSDistributor*> g_distributors;
vector<std::shared_ptr<UDPNameserver> > g_udpReceivers;
NetmaskGroup g_proxyProtocolACL;
size_t g_proxyProtocolMaximumSize;

ArgvMap &arg()
{
  return theArg;
}

void declareArguments()
{
  ::arg().set("config-dir","Location of configuration directory (pdns.conf)")=SYSCONFDIR;
  ::arg().set("config-name","Name of this virtual configuration - will rename the binary image")="";
  ::arg().set("socket-dir",string("Where the controlsocket will live, ")+LOCALSTATEDIR+"/pdns when unset and not chrooted"
#ifdef HAVE_SYSTEMD
      + ". Set to the RUNTIME_DIRECTORY environment variable when that variable has a value (e.g. under systemd).")="";
   auto runtimeDir = getenv("RUNTIME_DIRECTORY");
   if (runtimeDir != nullptr) {
     ::arg().set("socket-dir") = runtimeDir;
   }
#else
      )="";
#endif
  ::arg().set("module-dir","Default directory for modules")=PKGLIBDIR;
  ::arg().set("chroot","If set, chroot to this directory for more security")="";
  ::arg().set("logging-facility","Log under a specific facility")="";
  ::arg().set("daemon","Operate as a daemon")="no";

  ::arg().set("local-port","The port on which we listen")="53";
  ::arg().setSwitch("dnsupdate","Enable/Disable DNS update (RFC2136) support. Default is no.")="no";
  ::arg().setSwitch("write-pid","Write a PID file")="yes";
  ::arg().set("allow-dnsupdate-from","A global setting to allow DNS updates from these IP ranges.")="127.0.0.0/8,::1";
  ::arg().set("proxy-protocol-from", "A Proxy Protocol header is only allowed from these subnets, and is mandatory then too.")="";
  ::arg().set("proxy-protocol-maximum-size", "The maximum size of a proxy protocol payload, including the TLV values")="512";
  ::arg().setSwitch("send-signed-notify", "Send TSIG secured NOTIFY if TSIG key is configured for a zone") = "yes";
  ::arg().set("allow-unsigned-notify", "Allow unsigned notifications for TSIG secured zones") = "yes"; //FIXME: change to 'no' later
  ::arg().set("allow-unsigned-supermaster", "Allow supermasters to create zones without TSIG signed NOTIFY")="yes";
  ::arg().set("allow-unsigned-autoprimary", "Allow autoprimaries to create zones without TSIG signed NOTIFY")="yes";
  ::arg().setSwitch("forward-dnsupdate", "A global setting to allow DNS update packages that are for a Slave zone, to be forwarded to the master.") = "yes";
  ::arg().setSwitch("log-dns-details","If PDNS should log DNS non-erroneous details")="no";
  ::arg().setSwitch("log-dns-queries","If PDNS should log all incoming DNS queries")="no";
  ::arg().set("local-address","Local IP addresses to which we bind")="0.0.0.0, ::";
  ::arg().setSwitch("local-address-nonexist-fail","Fail to start if one or more of the local-address's do not exist on this server")="yes";
  ::arg().setSwitch("non-local-bind", "Enable binding to non-local addresses by using FREEBIND / BINDANY socket options")="no";
  ::arg().setSwitch("reuseport","Enable higher performance on compliant kernels by using SO_REUSEPORT allowing each receiver thread to open its own socket")="no";
  ::arg().set("query-local-address","Source IP addresses for sending queries")="0.0.0.0 ::";
  ::arg().set("overload-queue-length","Maximum queuelength moving to packetcache only")="0";
  ::arg().set("max-queue-length","Maximum queuelength before considering situation lost")="5000";

  ::arg().set("retrieval-threads", "Number of AXFR-retrieval threads for slave operation")="2";
  ::arg().setSwitch("api", "Enable/disable the REST API (including HTTP listener)")="no";
  ::arg().set("api-key", "Static pre-shared authentication key for access to the REST API")="";
  ::arg().setSwitch("default-api-rectify","Default API-RECTIFY value for zones")="yes";
  ::arg().setSwitch("dname-processing", "If we should support DNAME records")="no";

  ::arg().setCmd("help","Provide a helpful message");
  ::arg().setCmd("version","Output version and compilation date");
  ::arg().setCmd("config","Provide configuration file on standard output");
  ::arg().setCmd("list-modules","Lists all modules available");
  ::arg().setCmd("no-config","Don't parse configuration file");
  
  ::arg().set("version-string","PowerDNS version in packets - full, anonymous, powerdns or custom")="full"; 
  ::arg().set("control-console","Debugging switch - don't use")="no"; // but I know you will!
  ::arg().set("loglevel","Amount of logging. Higher is more. Do not set below 3")="4";
  ::arg().set("disable-syslog","Disable logging to syslog, useful when running inside a supervisor that logs stdout")="no";
  ::arg().set("log-timestamp","Print timestamps in log lines")="yes";
  ::arg().set("distributor-threads","Default number of Distributor (backend) threads to start")="3";
  ::arg().set("signing-threads","Default number of signer threads to start")="3";
  ::arg().set("receiver-threads","Default number of receiver threads to start")="1";
  ::arg().set("queue-limit","Maximum number of milliseconds to queue a query")="1500"; 
  ::arg().set("resolver","Use this resolver for ALIAS and the internal stub resolver")="no";
  ::arg().set("udp-truncation-threshold", "Maximum UDP response size before we truncate")="1232";
  
  ::arg().set("config-name","Name of this virtual configuration - will rename the binary image")="";

  ::arg().set("load-modules","Load this module - supply absolute or relative path")="";
  ::arg().set("launch","Which backends to launch and order to query them in")="";
  ::arg().setSwitch("disable-axfr","Disable zonetransfers but do allow TCP queries")="no";
  ::arg().set("allow-axfr-ips","Allow zonetransfers only to these subnets")="127.0.0.0/8,::1";
  ::arg().set("only-notify", "Only send AXFR NOTIFY to these IP addresses or netmasks")="0.0.0.0/0,::/0";
  ::arg().set("also-notify", "When notifying a zone, also notify these nameservers") = "";
  ::arg().set("allow-notify-from","Allow AXFR NOTIFY from these IP ranges. If empty, drop all incoming notifies.")="0.0.0.0/0,::/0";
  ::arg().set("slave-cycle-interval","Schedule slave freshness checks once every .. seconds")="60";
  ::arg().set("xfr-cycle-interval","Schedule primary/secondary SOA freshness checks once every .. seconds")="60";

  ::arg().set("tcp-control-address","If set, PowerDNS can be controlled over TCP on this address")="";
  ::arg().set("tcp-control-port","If set, PowerDNS can be controlled over TCP on this address")="53000";
  ::arg().set("tcp-control-secret","If set, PowerDNS can be controlled over TCP after passing this secret")="";
  ::arg().set("tcp-control-range","If set, remote control of PowerDNS is possible over these networks only")="127.0.0.0/8, 10.0.0.0/8, 192.168.0.0/16, 172.16.0.0/12, ::1/128, fe80::/10";
  
  ::arg().setSwitch("slave","Act as a secondary")="no";
  ::arg().setSwitch("secondary","Act as a secondary")="no";
  ::arg().setSwitch("master","Act as a primary")="no";
  ::arg().setSwitch("primary","Act as a primary")="no";
  ::arg().setSwitch("superslave", "Act as a autosecondary")="no";
  ::arg().setSwitch("autosecondary", "Act as an autosecondary (formerly superslave)")="no";
  ::arg().setSwitch("disable-axfr-rectify","Disable the rectify step during an outgoing AXFR. Only required for regression testing.")="no";
  ::arg().setSwitch("guardian","Run within a guardian process")="no";
  ::arg().setSwitch("prevent-self-notification","Don't send notifications to what we think is ourself")="yes";
  ::arg().setSwitch("any-to-tcp","Answer ANY queries with tc=1, shunting to TCP")="yes";
  ::arg().setSwitch("edns-subnet-processing","If we should act on EDNS Subnet options")="no";

  ::arg().set("edns-cookie-secret", "When set, set a server cookie when responding to a query with a Client cookie (in hex)")="";

  ::arg().setSwitch("webserver","Start a webserver for monitoring (api=yes also enables the HTTP listener)")="no";
  ::arg().setSwitch("webserver-print-arguments","If the webserver should print arguments")="no";
  ::arg().set("webserver-address","IP Address of webserver/API to listen on")="127.0.0.1";
  ::arg().set("webserver-port","Port of webserver/API to listen on")="8081";
  ::arg().set("webserver-password","Password required for accessing the webserver")="";
  ::arg().set("webserver-allow-from","Webserver/API access is only allowed from these subnets")="127.0.0.1,::1";
  ::arg().set("webserver-loglevel", "Amount of logging in the webserver (none, normal, detailed)") = "normal";
  ::arg().set("webserver-max-bodysize","Webserver/API maximum request/response body size in megabytes")="2";
  ::arg().setSwitch("webserver-hash-plaintext-credentials","Whether to hash passwords and api keys supplied in plaintext, to prevent keeping the plaintext version in memory at runtime")="no";

  ::arg().setSwitch("query-logging","Hint backends that queries should be logged")="no";

  ::arg().set("carbon-namespace", "If set overwrites the first part of the carbon string")="pdns";
  ::arg().set("carbon-ourname", "If set, overrides our reported hostname for carbon stats")="";
  ::arg().set("carbon-instance", "If set overwrites the instance name default")="auth";
  ::arg().set("carbon-server", "If set, send metrics in carbon (graphite) format to this server IP address")="";
  ::arg().set("carbon-interval", "Number of seconds between carbon (graphite) updates")="30";

  ::arg().set("cache-ttl","Seconds to store packets in the PacketCache")="20";
  ::arg().set("negquery-cache-ttl","Seconds to store negative query results in the QueryCache")="60";
  ::arg().set("query-cache-ttl","Seconds to store query results in the QueryCache")="20";
  ::arg().set("zone-cache-refresh-interval", "Seconds to cache list of known zones") = "300";
  ::arg().set("server-id", "Returned when queried for 'id.server' TXT or NSID, defaults to hostname - disabled or custom")="";
  ::arg().set("default-soa-content","Default SOA content")="a.misconfigured.dns.server.invalid hostmaster.@ 0 10800 3600 604800 3600";
  ::arg().set("default-soa-edit","Default SOA-EDIT value")="";
  ::arg().set("default-soa-edit-signed","Default SOA-EDIT value for signed zones")="";
  ::arg().set("dnssec-key-cache-ttl","Seconds to cache DNSSEC keys from the database")="30";
  ::arg().set("domain-metadata-cache-ttl", "Seconds to cache zone metadata from the database") = "";
  ::arg().set("zone-metadata-cache-ttl", "Seconds to cache zone metadata from the database") = "60";

  ::arg().set("trusted-notification-proxy", "IP address of incoming notification proxy")="";
  ::arg().set("slave-renotify", "If we should send out notifications for secondaried updates")="no";
  ::arg().set("secondary-do-renotify", "If this secondary should send out notifications after receiving zone transfers from a primary")="no";
  ::arg().set("forward-notify", "IP addresses to forward received notifications to regardless of master or slave settings")="";

  ::arg().set("default-ttl","Seconds a result is valid if not set otherwise")="3600";
  ::arg().set("max-tcp-connections","Maximum number of TCP connections")="20";
  ::arg().set("max-tcp-connections-per-client","Maximum number of simultaneous TCP connections per client")="0";
  ::arg().set("max-tcp-transactions-per-conn","Maximum number of subsequent queries per TCP connection")="0";
  ::arg().set("max-tcp-connection-duration","Maximum time in seconds that a TCP DNS connection is allowed to stay open.")="0";
  ::arg().set("tcp-idle-timeout","Maximum time in seconds that a TCP DNS connection is allowed to stay open while being idle")="5";

  ::arg().setSwitch("no-shuffle","Set this to prevent random shuffling of answers - for regression testing")="off";

  ::arg().set("setuid","If set, change user id to this uid for more security")="";
  ::arg().set("setgid","If set, change group id to this gid for more security")="";

  ::arg().set("max-cache-entries", "Maximum number of entries in the query cache")="1000000";
  ::arg().set("max-packet-cache-entries", "Maximum number of entries in the packet cache")="1000000";
  ::arg().set("max-signature-cache-entries", "Maximum number of signatures cache entries")="";
  ::arg().set("max-ent-entries", "Maximum number of empty non-terminals in a zone")="100000";
  ::arg().set("entropy-source", "If set, read entropy from this file")="/dev/urandom";

  ::arg().set("lua-prequery-script", "Lua script with prequery handler (DO NOT USE)")="";
  ::arg().set("lua-dnsupdate-policy-script", "Lua script with DNS update policy handler")="";

  ::arg().setSwitch("traceback-handler","Enable the traceback handler (Linux only)")="yes";
  ::arg().setSwitch("direct-dnskey","Fetch DNSKEY, CDS and CDNSKEY RRs from backend during DNSKEY or CDS/CDNSKEY synthesis")="no";
  ::arg().set("default-ksk-algorithm","Default KSK algorithm")="ecdsa256";
  ::arg().set("default-ksk-size","Default KSK size (0 means default)")="0";
  ::arg().set("default-zsk-algorithm","Default ZSK algorithm")="";
  ::arg().set("default-zsk-size","Default ZSK size (0 means default)")="0";
  ::arg().set("max-nsec3-iterations", "Limit the number of NSEC3 hash iterations") = "100";
  ::arg().set("default-publish-cdnskey","Default value for PUBLISH-CDNSKEY")="";
  ::arg().set("default-publish-cds","Default value for PUBLISH-CDS")="";

  ::arg().set("include-dir","Include *.conf files from this directory");
  ::arg().set("security-poll-suffix", "Zone name from which to query security update notifications") = "secpoll.powerdns.com.";

  ::arg().setSwitch("expand-alias", "Expand ALIAS records")="no";
  ::arg().setSwitch("outgoing-axfr-expand-alias", "Expand ALIAS records during outgoing AXFR")="no";
  ::arg().setSwitch("8bit-dns", "Allow 8bit dns queries")="no";
#ifdef HAVE_LUA_RECORDS
  ::arg().setSwitch("enable-lua-records", "Process LUA records for all zones (metadata overrides this)")="no";
  ::arg().set("lua-records-exec-limit", "LUA records scripts execution limit (instructions count). Values <= 0 mean no limit")="1000";
  ::arg().set("lua-health-checks-expire-delay", "Stops doing health checks after the record hasn't been used for that delay (in seconds)")="3600";
  ::arg().set("lua-health-checks-interval", "LUA records health checks monitoring interval in seconds")="5";
#endif
  ::arg().setSwitch("axfr-lower-serial", "Also AXFR a zone from a master with a lower serial")="no";

  ::arg().set("lua-axfr-script", "Script to be used to edit incoming AXFRs")="";
  ::arg().set("xfr-max-received-mbytes", "Maximum number of megabytes received from an incoming XFR")="100";
  ::arg().set("axfr-fetch-timeout", "Maximum time in seconds for inbound AXFR to start or be idle after starting")="10";

  ::arg().set("tcp-fast-open", "Enable TCP Fast Open support on the listening sockets, using the supplied numerical value as the queue size")="0";

  ::arg().set("max-generate-steps", "Maximum number of $GENERATE steps when loading a zone from a file")="0";
  ::arg().setSwitch("upgrade-unknown-types","Transparently upgrade known TYPExxx records. Recommended to keep off, except for PowerDNS upgrades until data sources are cleaned up")="no";
  ::arg().setSwitch("svc-autohints", "Transparently fill ipv6hint=auto ipv4hint=auto SVC params with AAAA/A records for the target name of the record (if within the same zone)")="no";

  ::arg().setSwitch("consistent-backends", "Assume individual zones are not divided over backends. Send only ANY lookup operations to the backend to reduce the number of lookups") = "yes";

  ::arg().set("rng", "Specify the random number generator to use. Valid values are auto,sodium,openssl,getrandom,arc4random,urandom.")="auto";
  ::arg().setDefaults();
}

static time_t s_start=time(nullptr);
static uint64_t uptimeOfProcess(const std::string& str)
{
  return time(nullptr) - s_start;
}

static uint64_t getSysUserTimeMsec(const std::string& str)
{
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);

  if(str=="sys-msec") {
    return (ru.ru_stime.tv_sec*1000ULL + ru.ru_stime.tv_usec/1000);
  }
  else
    return (ru.ru_utime.tv_sec*1000ULL + ru.ru_utime.tv_usec/1000);

}

static uint64_t getTCPConnectionCount(const std::string& str)
{
  return TN->numTCPConnections();
}

static uint64_t getQCount(const std::string& str)
try
{
  int totcount=0;
  for(const auto& d : g_distributors) {
    if(!d)
      continue;
    totcount += d->getQueueSize();  // this does locking and other things, so don't get smart
  }
  return totcount;
}
catch(std::exception& e)
{
  g_log<<Logger::Error<<"Had error retrieving queue sizes: "<<e.what()<<endl;
  return 0;
}
catch(PDNSException& e)
{
  g_log<<Logger::Error<<"Had error retrieving queue sizes: "<<e.reason<<endl;
  return 0;
}

static uint64_t getLatency(const std::string& str) 
{
  return round(avg_latency);
}

void declareStats()
{
  S.declare("udp-queries","Number of UDP queries received");
  S.declare("udp-do-queries","Number of UDP queries received with DO bit");
  S.declare("udp-cookie-queries", "Number of UDP queries received with the COOKIE EDNS option");
  S.declare("udp-answers","Number of answers sent out over UDP");
  S.declare("udp-answers-bytes","Total size of answers sent out over UDP");
  S.declare("udp4-answers-bytes","Total size of answers sent out over UDPv4");
  S.declare("udp6-answers-bytes","Total size of answers sent out over UDPv6");

  S.declare("udp4-answers","Number of IPv4 answers sent out over UDP");
  S.declare("udp4-queries","Number of IPv4 UDP queries received");
  S.declare("udp6-answers","Number of IPv6 answers sent out over UDP");
  S.declare("udp6-queries","Number of IPv6 UDP queries received");
  S.declare("overload-drops","Queries dropped because backends overloaded");

  S.declare("rd-queries", "Number of recursion desired questions");
  S.declare("recursion-unanswered", "Number of packets unanswered by configured recursor");
  S.declare("recursing-answers","Number of recursive answers sent out");
  S.declare("recursing-questions","Number of questions sent to recursor");
  S.declare("corrupt-packets","Number of corrupt packets received");
  S.declare("signatures", "Number of DNSSEC signatures made");
  S.declare("tcp-queries","Number of TCP queries received");
  S.declare("tcp-cookie-queries","Number of TCP queries received with the COOKIE option");
  S.declare("tcp-answers","Number of answers sent out over TCP");
  S.declare("tcp-answers-bytes","Total size of answers sent out over TCP");
  S.declare("tcp4-answers-bytes","Total size of answers sent out over TCPv4");
  S.declare("tcp6-answers-bytes","Total size of answers sent out over TCPv6");

  S.declare("tcp4-queries","Number of IPv4 TCP queries received");
  S.declare("tcp4-answers","Number of IPv4 answers sent out over TCP");
  
  S.declare("tcp6-queries","Number of IPv6 TCP queries received");
  S.declare("tcp6-answers","Number of IPv6 answers sent out over TCP");

  S.declare("open-tcp-connections","Number of currently open TCP connections", getTCPConnectionCount, StatType::gauge);

  S.declare("qsize-q","Number of questions waiting for database attention", getQCount, StatType::gauge);

  S.declare("dnsupdate-queries", "DNS update packets received.");
  S.declare("dnsupdate-answers", "DNS update packets successfully answered.");
  S.declare("dnsupdate-refused", "DNS update packets that are refused.");
  S.declare("dnsupdate-changes", "DNS update changes to records in total.");

  S.declare("incoming-notifications", "NOTIFY packets received.");

  S.declare("uptime", "Uptime of process in seconds", uptimeOfProcess, StatType::counter);
  S.declare("real-memory-usage", "Actual unique use of memory in bytes (approx)", getRealMemoryUsage, StatType::gauge);
  S.declare("special-memory-usage", "Actual unique use of memory in bytes (approx)", getSpecialMemoryUsage, StatType::gauge);
  S.declare("fd-usage", "Number of open filedescriptors", getOpenFileDescriptors, StatType::gauge);
#ifdef __linux__
  S.declare("udp-recvbuf-errors", "UDP 'recvbuf' errors", udpErrorStats, StatType::counter);
  S.declare("udp-sndbuf-errors", "UDP 'sndbuf' errors", udpErrorStats, StatType::counter);
  S.declare("udp-noport-errors", "UDP 'noport' errors", udpErrorStats, StatType::counter);
  S.declare("udp-in-errors", "UDP 'in' errors", udpErrorStats, StatType::counter);
#endif

  S.declare("sys-msec", "Number of msec spent in system time", getSysUserTimeMsec, StatType::counter);
  S.declare("user-msec", "Number of msec spent in user time", getSysUserTimeMsec, StatType::counter);

#ifdef __linux__
  S.declare("cpu-iowait", "Time spent waiting for I/O to complete by the whole system, in units of USER_HZ", getCPUIOWait, StatType::counter);
  S.declare("cpu-steal", "Stolen time, which is the time spent by the whole system in other operating systems when running in a virtualized environment, in units of USER_HZ", getCPUSteal, StatType::counter);
#endif

  S.declare("meta-cache-size", "Number of entries in the metadata cache", DNSSECKeeper::dbdnssecCacheSizes, StatType::gauge);
  S.declare("key-cache-size", "Number of entries in the key cache", DNSSECKeeper::dbdnssecCacheSizes, StatType::gauge);
  S.declare("signature-cache-size", "Number of entries in the signature cache", signatureCacheSize, StatType::gauge);

  S.declare("nxdomain-packets","Number of times an NXDOMAIN packet was sent out");
  S.declare("noerror-packets","Number of times a NOERROR packet was sent out");
  S.declare("servfail-packets","Number of times a server-failed packet was sent out");
  S.declare("unauth-packets", "Number of times a zone we are not auth for was queried");
  S.declare("latency","Average number of microseconds needed to answer a question", getLatency, StatType::gauge);
  S.declare("timedout-packets","Number of packets which weren't answered within timeout set");
  S.declare("security-status", "Security status based on regular polling", StatType::gauge);
  S.declare(
    "xfr-queue", "Size of the queue of zones to be XFRd", [](const string&) { return Communicator.getSuckRequestsWaiting(); }, StatType::gauge);
  S.declareDNSNameQTypeRing("queries","UDP Queries Received");
  S.declareDNSNameQTypeRing("nxdomain-queries", "Queries for non-existent records within existent zones");
  S.declareDNSNameQTypeRing("noerror-queries","Queries for existing records, but for type we don't have");
  S.declareDNSNameQTypeRing("servfail-queries","Queries that could not be answered due to backend errors");
  S.declareDNSNameQTypeRing("unauth-queries", "Queries for zones that we are not authoritative for");
  S.declareRing("logmessages","Log Messages");
  S.declareComboRing("remotes","Remote server IP addresses");
  S.declareComboRing("remotes-unauth", "Remote hosts querying zones for which we are not auth");
  S.declareComboRing("remotes-corrupt","Remote hosts sending corrupt packets");
}

int isGuarded(char **argv)
{
  char *p=strstr(argv[0],"-instance");

  return !!p;
}

static void sendout(std::unique_ptr<DNSPacket>& a)
{
  if(!a)
    return;

  try {
    N->send(*a);

    int diff=a->d_dt.udiff();
    avg_latency=0.999*avg_latency+0.001*diff;
  }
  catch (const std::exception& e) {
    g_log<<Logger::Error<<"Caught unhandled exception while sending a response: "<<e.what()<<endl;
  }
}

//! The qthread receives questions over the internet via the Nameserver class, and hands them to the Distributor for further processing
static void qthread(unsigned int num)
try
{
  setThreadName("pdns/receiver");

  g_distributors[num] = DNSDistributor::Create(::arg().asNum("distributor-threads", 1));
  DNSDistributor* distributor = g_distributors[num]; // the big dispatcher!
  DNSPacket question(true);
  DNSPacket cached(false);

  AtomicCounter &numreceived=*S.getPointer("udp-queries");
  AtomicCounter &numreceiveddo=*S.getPointer("udp-do-queries");
  AtomicCounter &numreceivedcookie=*S.getPointer("udp-cookie-queries");

  AtomicCounter &numreceived4=*S.getPointer("udp4-queries");

  AtomicCounter &numreceived6=*S.getPointer("udp6-queries");
  AtomicCounter &overloadDrops=*S.getPointer("overload-drops");

  int diff;
  bool logDNSQueries = ::arg().mustDo("log-dns-queries");
  shared_ptr<UDPNameserver> NS;
  std::string buffer;

  // If we have SO_REUSEPORT then create a new port for all receiver threads
  // other than the first one.
  if(N->canReusePort() ) {
    NS = g_udpReceivers[num];
    if (NS == nullptr) {
      NS = N;
    }
  } else {
    NS = N;
  }

  for(;;) {
    try {
      if (g_proxyProtocolACL.empty()) {
        buffer.resize(DNSPacket::s_udpTruncationThreshold);
      }
      else {
        buffer.resize(DNSPacket::s_udpTruncationThreshold + g_proxyProtocolMaximumSize);
      }

      if(!NS->receive(question, buffer)) { // receive a packet         inline
        continue;                    // packet was broken, try again
      }

      numreceived++;

      if(question.d_remote.getSocklen()==sizeof(sockaddr_in))
        numreceived4++;
      else
        numreceived6++;

      if(question.d_dnssecOk)
        numreceiveddo++;

      if(question.hasEDNSCookie())
        numreceivedcookie++;

      if(question.d.qr)
        continue;

      S.ringAccount("queries", question.qdomain, question.qtype);
      S.ringAccount("remotes", question.d_remote);
      if(logDNSQueries) {
        g_log << Logger::Notice<<"Remote "<< question.getRemoteString() <<" wants '" << question.qdomain<<"|"<<question.qtype <<
          "', do = " <<question.d_dnssecOk <<", bufsize = "<< question.getMaxReplyLen();
        if(question.d_ednsRawPacketSizeLimit > 0 && question.getMaxReplyLen() != (unsigned int)question.d_ednsRawPacketSizeLimit)
          g_log<<" ("<<question.d_ednsRawPacketSizeLimit<<")";
      }

      if(PC.enabled() && (question.d.opcode != Opcode::Notify && question.d.opcode != Opcode::Update) && question.couldBeCached()) {
        bool haveSomething=PC.get(question, cached); // does the PacketCache recognize this question?
        if (haveSomething) {
          if(logDNSQueries)
            g_log<<": packetcache HIT"<<endl;
          cached.setRemote(&question.d_remote);  // inlined
          cached.setSocket(question.getSocket());                               // inlined
          cached.d_anyLocal = question.d_anyLocal;
          cached.setMaxReplyLen(question.getMaxReplyLen());
          cached.d.rd=question.d.rd; // copy in recursion desired bit
          cached.d.id=question.d.id;
          cached.commitD(); // commit d to the packet                        inlined
          NS->send(cached); // answer it then                              inlined
          diff=question.d_dt.udiff();
          avg_latency=0.999*avg_latency+0.001*diff; // 'EWMA'
          continue;
        }
      }

      if(distributor->isOverloaded()) {
        if(logDNSQueries)
          g_log<<": Dropped query, backends are overloaded"<<endl;
        overloadDrops++;
        continue;
      }

      if (logDNSQueries) {
        if (PC.enabled()) {
          g_log<<": packetcache MISS"<<endl;
        } else {
          g_log<<endl;
        }
      }

      try {
        distributor->question(question, &sendout); // otherwise, give to the distributor
      }
      catch(DistributorFatal& df) { // when this happens, we have leaked loads of memory. Bailing out time.
        _exit(1);
      }
    }
    catch (const std::exception& e) {
      g_log<<Logger::Error<<"Caught unhandled exception in question thread: "<<e.what()<<endl;
    }
  }
}
catch(PDNSException& pe)
{
  g_log<<Logger::Error<<"Fatal error in question thread: "<<pe.reason<<endl;
  _exit(1);
}

static void dummyThread()
{
}

static void triggerLoadOfLibraries()
{
  std::thread dummy(dummyThread);
  dummy.join();
}

void mainthread()
{
   Utility::srandom();

   gid_t newgid = 0;
   if(!::arg()["setgid"].empty())
     newgid = strToGID(::arg()["setgid"]);
   uid_t newuid = 0;
   if(!::arg()["setuid"].empty())
     newuid = strToUID(::arg()["setuid"]);
   
   g_anyToTcp = ::arg().mustDo("any-to-tcp");
   g_8bitDNS = ::arg().mustDo("8bit-dns");
#ifdef HAVE_LUA_RECORDS
   g_doLuaRecord = ::arg().mustDo("enable-lua-records");
   g_LuaRecordSharedState = (::arg()["enable-lua-records"] == "shared");
   g_luaRecordExecLimit = ::arg().asNum("lua-records-exec-limit");
   g_luaHealthChecksInterval = ::arg().asNum("lua-health-checks-interval");
   g_luaHealthChecksExpireDelay = ::arg().asNum("lua-health-checks-expire-delay");
#endif

   DNSPacket::s_udpTruncationThreshold = std::max(512, ::arg().asNum("udp-truncation-threshold"));
   DNSPacket::s_doEDNSSubnetProcessing = ::arg().mustDo("edns-subnet-processing");
   PacketHandler::s_SVCAutohints = ::arg().mustDo("svc-autohints");

   g_proxyProtocolACL.toMasks(::arg()["proxy-protocol-from"]);
   g_proxyProtocolMaximumSize = ::arg().asNum("proxy-protocol-maximum-size");

   if (::arg()["edns-cookie-secret"].size() != 0) {
     // User wants cookie processing
#ifdef HAVE_CRYPTO_SHORTHASH // we can do siphash-based cookies
     DNSPacket::s_doEDNSCookieProcessing = true;
     try {
       if (::arg()["edns-cookie-secret"].size() != EDNSCookiesOpt::EDNSCookieSecretSize) {
         throw std::range_error("wrong size (" + std::to_string(::arg()["edns-cookie-secret"].size()) + "), must be " + std::to_string(EDNSCookiesOpt::EDNSCookieSecretSize));
       }
       DNSPacket::s_EDNSCookieKey = makeBytesFromHex(::arg()["edns-cookie-secret"]);
     } catch(const std::range_error &e) {
       g_log<<Logger::Error<<"edns-cookie-secret invalid: "<<e.what()<<endl;
       exit(1);
     }
#else
     g_log<<Logger::Error<<"Support for EDNS Cookies is not available because of missing cryptographic functions (libsodium support should be enabled, with the crypto_shorthash() function available)"<<endl;
     exit(1);
#endif
   }

   PC.setTTL(::arg().asNum("cache-ttl"));
   PC.setMaxEntries(::arg().asNum("max-packet-cache-entries"));
   QC.setMaxEntries(::arg().asNum("max-cache-entries"));
   DNSSECKeeper::setMaxEntries(::arg().asNum("max-cache-entries"));

   if (!PC.enabled() && ::arg().mustDo("log-dns-queries")) {
     g_log<<Logger::Warning<<"Packet cache disabled, logging queries without HIT/MISS"<<endl;
   }

   stubParseResolveConf();

   if(!::arg()["chroot"].empty()) {
#ifdef HAVE_SYSTEMD
     char *ns;
     ns = getenv("NOTIFY_SOCKET");
     if (ns != nullptr) {
       g_log<<Logger::Error<<"Unable to chroot when running from systemd. Please disable chroot= or set the 'Type' for this service to 'simple'"<<endl;
       exit(1);
     }
#endif
     triggerLoadOfLibraries();
     if(::arg().mustDo("primary") || ::arg().mustDo("secondary"))
        gethostbyname("a.root-servers.net"); // this forces all lookup libraries to be loaded
     Utility::dropGroupPrivs(newuid, newgid);
     if(chroot(::arg()["chroot"].c_str())<0 || chdir("/")<0) {
       g_log<<Logger::Error<<"Unable to chroot to '"+::arg()["chroot"]+"': "<<stringerror()<<", exiting"<<endl; 
       exit(1);
     }   
     else
       g_log<<Logger::Error<<"Chrooted to '"<<::arg()["chroot"]<<"'"<<endl;      
   } else {
     Utility::dropGroupPrivs(newuid, newgid);
   }

  AuthWebServer webserver;
  Utility::dropUserPrivs(newuid);

  if(::arg().mustDo("resolver")){
    DP=std::unique_ptr<DNSProxy>(new DNSProxy(::arg()["resolver"]));
    DP->go();
  }

  try {
    doSecPoll(true);
  }
  catch(...) {}

  {
    // Some sanity checking on default key settings
    bool hadKeyError = false;
    int kskAlgo{0}, zskAlgo{0};
    for (const string algotype : {"ksk", "zsk"}) {
      int algo, size;
      if (::arg()["default-"+algotype+"-algorithm"].empty())
        continue;
      algo = DNSSECKeeper::shorthand2algorithm(::arg()["default-"+algotype+"-algorithm"]);
      size = ::arg().asNum("default-"+algotype+"-size");
      if (algo == -1) {
        g_log<<Logger::Error<<"Error: default-"<<algotype<<"-algorithm set to unknown algorithm: "<<::arg()["default-"+algotype+"-algorithm"]<<endl;
        hadKeyError = true;
      }
      else if (algo <= 10 && size == 0) {
        g_log<<Logger::Error<<"Error: default-"<<algotype<<"-algorithm is set to an algorithm ("<<::arg()["default-"+algotype+"-algorithm"]<<") that requires a non-zero default-"<<algotype<<"-size!"<<endl;
        hadKeyError = true;
      }
      if (algotype == "ksk") {
        kskAlgo = algo;
      } else {
        zskAlgo = algo;
      }
    }
    if (hadKeyError) {
      exit(1);
    }
    if (kskAlgo == 0 && zskAlgo != 0) {
      g_log<<Logger::Error<<"Error: default-zsk-algorithm is set, but default-ksk-algorithm is not set."<<endl;
      exit(1);
    }
    if (zskAlgo != 0 && zskAlgo != kskAlgo) {
      g_log<<Logger::Error<<"Error: default-zsk-algorithm ("<<::arg()["default-zsk-algorithm"]<<"), when set, can not be different from default-ksk-algorithm ("<<::arg()["default-ksk-algorithm"]<<")."<<endl;
      exit(1);
    }
  }

  pdns::parseQueryLocalAddress(::arg()["query-local-address"]);

  pdns::parseTrustedNotificationProxy(::arg()["trusted-notification-proxy"]);

  // NOW SAFE TO CREATE THREADS!
  dl->go();

  if(::arg().mustDo("webserver") || ::arg().mustDo("api"))
    webserver.go();

  if(::arg().mustDo("primary") || ::arg().mustDo("secondary")|| !::arg()["forward-notify"].empty())
    Communicator.go(); 

  TN->go(); // tcp nameserver launch

  unsigned int max_rthreads= ::arg().asNum("receiver-threads", 1);
  g_distributors.resize(max_rthreads);
  for(unsigned int n=0; n < max_rthreads; ++n) {
    std::thread t(qthread, n);
    t.detach();
  }

  std::thread carbonThread(carbonDumpThread); // runs even w/o carbon, might change @ runtime    

#ifdef HAVE_SYSTEMD
  /* If we are here, notify systemd that we are ay-ok! This might have some
   * timing issues with the backend-threads. e.g. if the initial MySQL connection
   * is slow and times out (leading to process termination through the backend)
   * We probably have told systemd already that we have started correctly.
   */
    sd_notify(0, "READY=1");
#endif

  const uint32_t secpollInterval = 1800;
  uint32_t secpollSince = 0;
  uint32_t zoneCacheUpdateSince = 0;
  for(;;) {
    const uint32_t sleeptime = g_zoneCache.getRefreshInterval() == 0 ? secpollInterval : std::min(secpollInterval, g_zoneCache.getRefreshInterval());
    sleep(sleeptime);  // if any signals arrive, we might run more often than expected.

    zoneCacheUpdateSince += sleeptime;
    if (zoneCacheUpdateSince >= g_zoneCache.getRefreshInterval()) {
      try {
        UeberBackend B;
        B.updateZoneCache();
        zoneCacheUpdateSince = 0;
      }
      catch(PDNSException &e) {
        g_log<<Logger::Error<<"PDNSException while updating zone cache: "<<e.reason<<endl;
      }
      catch(std::exception &e) {
        g_log<<Logger::Error<<"STL Exception while updating zone cache: "<<e.what()<<endl;
      }
    }

    secpollSince += sleeptime;
    if (secpollSince >= secpollInterval) {
      secpollSince = 0;
      try {
        doSecPoll(false);
      }
      catch(...){}
    }
  }
  
  g_log<<Logger::Error<<"Mainthread exiting - should never happen"<<endl;
}
