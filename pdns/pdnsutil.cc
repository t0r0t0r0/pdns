#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "dnsseckeeper.hh"
#include "dnssecinfra.hh"
#include "statbag.hh"
#include "base32.hh"
#include "base64.hh"

#include <boost/program_options.hpp>
#include <boost/assign/std/vector.hpp>
#include <boost/assign/list_of.hpp>
#include "dnsbackend.hh"
#include "ueberbackend.hh"
#include "arguments.hh"
#include "packetcache.hh"
#include "zoneparser-tng.hh"
#include "signingpipe.hh"
#include "dns_random.hh"
#include <fstream>
#ifdef HAVE_LIBSODIUM
#include <sodium.h>
#endif
#ifdef HAVE_SQLITE3
#include "ssqlite3.hh"
#include "bind-dnssec.schema.sqlite3.sql.h"
#endif

StatBag S;
PacketCache PC;

namespace po = boost::program_options;
po::variables_map g_vm;

string s_programname="pdns";

namespace {
  bool g_verbose;
}

ArgvMap &arg()
{
  static ArgvMap arg;
  return arg;
}

string humanTime(time_t t)
{
  char ret[256];
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(ret, sizeof(ret)-1, "%c", &tm);   // %h:%M %Y-%m-%d
  return ret;
}

static void algorithm2name(uint8_t algo, string &name) {
        switch(algo) {
        case 0:
           name = "Reserved"; return;
        case 1:
           name = "RSAMD5"; return;
        case 2:
           name = "DH"; return;
        case 3:
           name = "DSA"; return;
        case 4:
           name = "ECC"; return;
        case 5:
           name = "RSASHA1"; return;
        case 6:
           name = "DSA-NSEC3-SHA1"; return;
        case 7:
           name = "RSASHA1-NSEC3-SHA1"; return;
        case 8:
           name = "RSASHA256"; return;
        case 9:
           name = "Reserved"; return;
        case 10:
           name = "RSASHA512"; return;
        case 11:
           name = "Reserved"; return;
        case 12:
           name = "ECC-GOST"; return;
        case 13:
           name = "ECDSAP256SHA256"; return;
        case 14:
           name = "ECDSAP384SHA384"; return;
        case 250:
           name = "ED25519SHA512"; return;
        case 252:
           name = "INDIRECT"; return;
        case 253:
           name = "PRIVATEDNS"; return;
        case 254:
           name = "PRIVATEOID"; return;
        default:
           name = "Unallocated/Reserved"; return;
        }
};

static int shorthand2algorithm(const string &algorithm)
{
  if (!algorithm.compare("rsamd5")) return 1;
  if (!algorithm.compare("dh")) return 2;
  if (!algorithm.compare("dsa")) return 3;
  if (!algorithm.compare("ecc")) return 4;
  if (!algorithm.compare("rsasha1")) return 5;
  if (!algorithm.compare("rsasha256")) return 8;
  if (!algorithm.compare("rsasha512")) return 10;
  if (!algorithm.compare("gost")) return 12;
  if (!algorithm.compare("ecdsa256")) return 13;
  if (!algorithm.compare("ecdsa384")) return 14;
  if (!algorithm.compare("experimental-ed25519")) return 250;
  return -1;
}

void loadMainConfig(const std::string& configdir)
{
  ::arg().set("config-dir","Location of configuration directory (pdns.conf)")=configdir;
  ::arg().set("default-ttl","Seconds a result is valid if not set otherwise")="3600";
  ::arg().set("launch","Which backends to launch");
  ::arg().set("dnssec","if we should do dnssec")="true";
  ::arg().set("config-name","Name of this virtual configuration - will rename the binary image")=g_vm["config-name"].as<string>();
  ::arg().setCmd("help","Provide a helpful message");
  //::arg().laxParse(argc,argv);

  if(::arg().mustDo("help")) {
    cout<<"syntax:"<<endl<<endl;
    cout<<::arg().helpstring(::arg()["help"])<<endl;
    exit(0);
  }

  if(::arg()["config-name"]!="") 
    s_programname+="-"+::arg()["config-name"];

  string configname=::arg()["config-dir"]+"/"+s_programname+".conf";
  cleanSlashes(configname);

  ::arg().set("default-ksk-algorithms","Default KSK algorithms")="";
  ::arg().set("default-ksk-size","Default KSK size (0 means default)")="0";
  ::arg().set("default-zsk-algorithms","Default ZSK algorithms")="ecdsa256";
  ::arg().set("default-zsk-size","Default ZSK size (0 means default)")="0";
  ::arg().set("default-soa-edit","Default SOA-EDIT value")="";
  ::arg().set("default-soa-edit-signed","Default SOA-EDIT value for signed zones")="";
  ::arg().set("max-ent-entries", "Maximum number of empty non-terminals in a zone")="100000";
  ::arg().set("module-dir","Default directory for modules")=PKGLIBDIR;
  ::arg().set("entropy-source", "If set, read entropy from this file")="/dev/urandom";
  ::arg().setSwitch("query-logging","Hint backends that queries should be logged")="no";
  ::arg().set("loglevel","Amount of logging. Higher is more.")="0";
  ::arg().setSwitch("direct-dnskey","Fetch DNSKEY RRs from backend during DNSKEY synthesis")="no";
  ::arg().set("max-nsec3-iterations","Limit the number of NSEC3 hash iterations")="500"; // RFC5155 10.3
  ::arg().set("max-signature-cache-entries", "Maximum number of signatures cache entries")="";
  ::arg().laxFile(configname.c_str());

  L.toConsole(Logger::Error);   // so we print any errors
  BackendMakers().launch(::arg()["launch"]); // vrooooom!
  L.toConsole((Logger::Urgency)(::arg().asNum("loglevel")));  
  ::arg().laxFile(configname.c_str());    
  //cerr<<"Backend: "<<::arg()["launch"]<<", '" << ::arg()["gmysql-dbname"] <<"'" <<endl;

  S.declare("qsize-q","Number of questions waiting for database attention");
    
  S.declare("deferred-cache-inserts","Amount of cache inserts that were deferred because of maintenance");
  S.declare("deferred-cache-lookup","Amount of cache lookups that were deferred because of maintenance");
          
  S.declare("query-cache-hit","Number of hits on the query cache");
  S.declare("query-cache-miss","Number of misses on the query cache");
  ::arg().set("max-cache-entries", "Maximum number of cache entries")="1000000";
  ::arg().set("recursor","If recursion is desired, IP address of a recursing nameserver")="no"; 
  ::arg().set("recursive-cache-ttl","Seconds to store packets for recursive queries in the PacketCache")="10";
  ::arg().set("cache-ttl","Seconds to store packets in the PacketCache")="20";              
  ::arg().set("negquery-cache-ttl","Seconds to store negative query results in the QueryCache")="60";
  ::arg().set("query-cache-ttl","Seconds to store query results in the QueryCache")="20";              
  ::arg().set("default-soa-name","name to insert in the SOA record if none set in the backend")="a.misconfigured.powerdns.server";
  ::arg().set("default-soa-mail","mail address to insert in the SOA record if none set in the backend")="";
  ::arg().set("soa-refresh-default","Default SOA refresh")="10800";
  ::arg().set("soa-retry-default","Default SOA retry")="3600";
  ::arg().set("soa-expire-default","Default SOA expire")="604800";
  ::arg().set("soa-minimum-ttl","Default SOA minimum ttl")="3600";    

  UeberBackend::go();
}

// irritatingly enough, rectifyZone needs its own ueberbackend and can't therefore benefit from transactions outside its scope
// I think this has to do with interlocking transactions between B and DK, but unsure.
bool rectifyZone(DNSSECKeeper& dk, const DNSName& zone)
{
  if(dk.isPresigned(zone)){
    cerr<<"Rectify presigned zone '"<<zone.toString()<<"' is not allowed/necessary."<<endl;
    return false;
  }

  UeberBackend B("default");
  bool doTransaction=true; // but see above
  SOAData sd;

  if(!B.getSOAUncached(zone, sd)) {
    cerr<<"No SOA known for '"<<zone.toString()<<"', is such a zone in the database?"<<endl;
    return false;
  }
  sd.db->list(zone, sd.domain_id);

  DNSResourceRecord rr;
  set<DNSName> qnames, nsset, dsnames, insnonterm, delnonterm;
  map<DNSName,bool> nonterm;
  bool doent=true;

  while(sd.db->get(rr)) {
    if (rr.qtype.getCode())
    {
      qnames.insert(rr.qname);
      if(rr.qtype.getCode() == QType::NS && rr.qname != zone)
        nsset.insert(rr.qname);
      if(rr.qtype.getCode() == QType::DS)
        dsnames.insert(rr.qname);
    }
    else
      if(doent)
        delnonterm.insert(rr.qname);
  }

  NSEC3PARAMRecordContent ns3pr;
  bool narrow;
  bool haveNSEC3=dk.getNSEC3PARAM(zone, &ns3pr, &narrow);
  bool isOptOut=(haveNSEC3 && ns3pr.d_flags);
  if(sd.db->doesDNSSEC())
  {
    if(!haveNSEC3)
      cerr<<"Adding NSEC ordering information "<<endl;
    else if(!narrow) {
      if(!isOptOut)
        cerr<<"Adding NSEC3 hashed ordering information for '"<<zone.toString()<<"'"<<endl;
      else
        cerr<<"Adding NSEC3 opt-out hashed ordering information for '"<<zone.toString()<<"'"<<endl;
    } else
      cerr<<"Erasing NSEC3 ordering since we are narrow, only setting 'auth' fields"<<endl;
  }
  else
    cerr<<"Non DNSSEC zone, only adding empty non-terminals"<<endl;

  if(doTransaction)
    sd.db->startTransaction(zone, -1);

  bool realrr=true;
  uint32_t maxent = ::arg().asNum("max-ent-entries");

  dononterm:;
  for (const auto& qname: qnames)
  {
    bool auth=true;
    DNSName ordername;
    auto shorter(qname);

    if(realrr) {
      do {
        if(nsset.count(shorter)) {
          auth=false;
          break;
        }
      } while(shorter.chopOff());
    }

    if(haveNSEC3) // NSEC3
    {
      if(!narrow && (realrr || !isOptOut || nonterm.find(qname)->second))
        ordername=DNSName(toBase32Hex(hashQNameWithSalt(ns3pr, qname))) + zone;
      else if(!realrr)
        auth=false;
    }
    else if (realrr) // NSEC
      ordername=qname;

    if(g_verbose)
      cerr<<"'"<<qname.toString()<<"' -> '"<< ordername.toString() <<"'"<<endl;
    sd.db->updateDNSSECOrderNameAndAuth(sd.domain_id, zone, qname, ordername, auth);

    if(realrr)
    {
      if (dsnames.count(qname))
        sd.db->updateDNSSECOrderNameAndAuth(sd.domain_id, zone, qname, ordername, true, QType::DS);
      if (!auth || nsset.count(qname)) {
        ordername.clear();
        if(isOptOut)
          sd.db->updateDNSSECOrderNameAndAuth(sd.domain_id, zone, qname, ordername, false, QType::NS);
        sd.db->updateDNSSECOrderNameAndAuth(sd.domain_id, zone, qname, ordername, false, QType::A);
        sd.db->updateDNSSECOrderNameAndAuth(sd.domain_id, zone, qname, ordername, false, QType::AAAA);
      }

      if(doent)
      {
        shorter=qname;
        while(shorter!=zone && shorter.chopOff())
        {
          if(!qnames.count(shorter))
          {
            if(!(maxent))
            {
              cerr<<"Zone '"<<zone.toString()<<"' has too many empty non terminals."<<endl;
              insnonterm.clear();
              delnonterm.clear();
              doent=false;
              break;
            }

            if (!delnonterm.count(shorter) && !nonterm.count(shorter))
              insnonterm.insert(shorter);
            else
              delnonterm.erase(shorter);

            if (!nonterm.count(shorter)) {
              nonterm.insert(pair<DNSName, bool>(shorter, auth));
              --maxent;
            } else if (auth)
              nonterm[shorter]=true;
          }
        }
      }
    }
  }

  if(realrr)
  {
    //cerr<<"Total: "<<nonterm.size()<<" Insert: "<<insnonterm.size()<<" Delete: "<<delnonterm.size()<<endl;
    if(!insnonterm.empty() || !delnonterm.empty() || !doent)
    {
      sd.db->updateEmptyNonTerminals(sd.domain_id, zone, insnonterm, delnonterm, !doent);
    }
    if(doent)
    {
      realrr=false;
      qnames.clear();
      for(const auto& nt :  nonterm){
        qnames.insert(nt.first);
      }
      goto dononterm;
    }
  }

  if(doTransaction)
    sd.db->commitTransaction();

  return true;
}

void dbBench(const std::string& fname)
{
  ::arg().set("query-cache-ttl")="0";
  ::arg().set("negquery-cache-ttl")="0";
  UeberBackend B("default");

  vector<string> domains;
  if(!fname.empty()) {
    ifstream ifs(fname.c_str());
    if(!ifs) {
      cerr<<"Could not open '"<<fname<<"' for reading domain names to query"<<endl;
    }
    string line;
    while(getline(ifs,line)) {
      trim(line);
      domains.push_back(line);
    }
  }
  if(domains.empty())
    domains.push_back("powerdns.com");

  int n=0;
  DNSResourceRecord rr;
  DTime dt;
  dt.set();
  unsigned int hits=0, misses=0;
  for(; n < 10000; ++n) {
    DNSName domain(domains[random() % domains.size()]);
    B.lookup(QType(QType::NS), domain);
    while(B.get(rr)) {
      hits++;
    }
    B.lookup(QType(QType::A), DNSName(std::to_string(random()))+domain);
    while(B.get(rr)) {
    }
    misses++;

  }
  cout<<0.001*dt.udiff()/n<<" millisecond/lookup"<<endl;
  cout<<"Retrieved "<<hits<<" records, did "<<misses<<" queries which should have no match"<<endl;
  cout<<"Packet cache reports: "<<S.read("query-cache-hit")<<" hits (should be 0) and "<<S.read("query-cache-miss") <<" misses"<<endl;
}

void rectifyAllZones(DNSSECKeeper &dk) 
{
  UeberBackend B("default");
  vector<DomainInfo> domainInfo;

  B.getAllDomains(&domainInfo);
  for(DomainInfo di :  domainInfo) {
    cerr<<"Rectifying "<<di.zone.toString()<<": ";
    rectifyZone(dk, di.zone);
  }
  cout<<"Rectified "<<domainInfo.size()<<" zones."<<endl;
}

int checkZone(DNSSECKeeper &dk, UeberBackend &B, const DNSName& zone)
{
  SOAData sd;
  if(!B.getSOAUncached(zone, sd)) {
    cout<<"[error] No SOA record present, or active, in zone '"<<zone.toString()<<"'"<<endl;
    cout<<"Checked 0 records of '"<<zone.toString()<<"', 1 errors, 0 warnings."<<endl;
    return 1;
  }

  NSEC3PARAMRecordContent ns3pr;
  bool narrow = false;
  bool haveNSEC3 = dk.getNSEC3PARAM(zone, &ns3pr, &narrow);
  bool isOptOut=(haveNSEC3 && ns3pr.d_flags);

  bool isSecure=dk.isSecuredZone(zone);
  bool presigned=dk.isPresigned(zone);

  DNSResourceRecord rr;
  uint64_t numrecords=0, numerrors=0, numwarnings=0;

  if (haveNSEC3 && isSecure && zone.wirelength() > 222) {
    numerrors++;
    cerr<<"[Error] zone '" << zone.toStringNoDot() << "' has NSEC3 semantics but is too long to have the hash prepended. Zone name is " << zone.wirelength() << " bytes long, whereas the maximum is 222 bytes." << endl;
  }

  // Check for delegation in parent zone
  DNSName parent(zone);
  while(parent.chopOff()) {
    SOAData sd_p;
    if(B.getSOAUncached(parent, sd_p)) {
      bool ns=false;
      DNSResourceRecord rr;
      B.lookup(QType(QType::ANY), zone, NULL, sd_p.domain_id);
      while(B.get(rr))
        ns |= (rr.qtype == QType::NS);
      if (!ns) {
        cerr<<"[Error] No delegation for zone '"<<zone.toString()<<"' in parent '"<<parent.toString()<<"'"<<endl;
        numerrors++;
      }
      break;
    }
  }


  bool hasNsAtApex = false;
  set<DNSName> tlsas, cnames, noncnames, glue, checkglue;
  set<string> records;
  map<string, unsigned int> ttl;

  ostringstream content;
  pair<map<string, unsigned int>::iterator,bool> ret;

  sd.db->list(zone, sd.domain_id, true);

  while(sd.db->get(rr)) {
    if(!rr.qtype.getCode())
      continue;

    numrecords++;

    if(rr.qtype.getCode() == QType::TLSA)
      tlsas.insert(rr.qname);
    if(rr.qtype.getCode() == QType::SOA) {
      vector<string>parts;
      stringtok(parts, rr.content);

      ostringstream o;
      o<<rr.content;
      for(int pleft=parts.size(); pleft < 7; ++pleft) {
        o<<" 0";
      }
      rr.content=o.str();
    }

    if(rr.qtype.getCode() == QType::TXT && !rr.content.empty() && rr.content[0]!='"')
      rr.content = "\""+rr.content+"\"";

    try {
      shared_ptr<DNSRecordContent> drc(DNSRecordContent::mastermake(rr.qtype.getCode(), 1, rr.content));
      string tmp=drc->serialize(rr.qname);
      tmp = drc->getZoneRepresentation(true);
      if (rr.qtype.getCode() != QType::AAAA) {
        if (!pdns_iequals(tmp, rr.content)) {
          cout<<"[Warning] Parsed and original record content are not equal: "<<rr.qname.toString()<<" IN " <<rr.qtype.getName()<< " '" << rr.content<<"' (Content parsed as '"<<tmp<<"')"<<endl;
          numwarnings++;
        }
      } else {
        struct in6_addr tmpbuf;
        if (inet_pton(AF_INET6, rr.content.c_str(), &tmpbuf) != 1 || rr.content.find('.') != string::npos) {
          cout<<"[Warning] Following record is not a valid IPv6 address: "<<rr.qname.toString()<<" IN " <<rr.qtype.getName()<< " '" << rr.content<<"'"<<endl;
          numwarnings++;
        }
      }
    }
    catch(std::exception& e)
    {
      cout<<"[Error] Following record had a problem: "<<rr.qname.toString()<<" IN " <<rr.qtype.getName()<< " " << rr.content<<endl;
      cout<<"[Error] Error was: "<<e.what()<<endl;
      numerrors++;
      continue;
    }

    if(!rr.qname.isPartOf(zone)) {
      cout<<"[Warning] Record '"<<rr.qname.toString()<<" IN "<<rr.qtype.getName()<<" "<<rr.content<<"' in zone '"<<zone.toString()<<"' is out-of-zone."<<endl;
      numwarnings++;
      continue;
    }

    content.str("");
    content<<rr.qname.toString()<<" "<<rr.qtype.getName()<<" "<<rr.content;
    if (records.count(toLower(content.str()))) {
      cout<<"[Error] Duplicate record found in rrset: '"<<rr.qname.toString()<<" IN "<<rr.qtype.getName()<<" "<<rr.content<<"'"<<endl;
      numerrors++;
      continue;
    } else
      records.insert(toLower(content.str()));

    content.str("");
    content<<rr.qname.toString()<<" "<<rr.qtype.getName();
    if (rr.qtype.getCode() == QType::RRSIG) {
      RRSIGRecordContent rrc(rr.content);
      content<<" ("<<DNSRecordContent::NumberToType(rrc.d_type)<<")";
    }
    ret = ttl.insert(pair<string, unsigned int>(toLower(content.str()), rr.ttl));
    if (ret.second == false && ret.first->second != rr.ttl) {
      cout<<"[Error] TTL mismatch in rrset: '"<<rr.qname.toString()<<" IN " <<rr.qtype.getName()<<" "<<rr.content<<"' ("<<ret.first->second<<" != "<<rr.ttl<<")"<<endl;
      numerrors++;
      continue;
    }

    if (isSecure && isOptOut && (rr.qname.countLabels() && rr.qname.getRawLabels()[0] == "*")) {
      cout<<"[Warning] wildcard record '"<<rr.qname.toString()<<" IN " <<rr.qtype.getName()<<" "<<rr.content<<"' is insecure"<<endl;
      cout<<"[Info] Wildcard records in opt-out zones are insecure. Disable the opt-out flag for this zone to avoid this warning. Command: pdnsutil set-nsec3 "<<zone.toString()<<endl;
      numwarnings++;
    }

    if(rr.qname==zone) {
      if (rr.qtype.getCode() == QType::NS) {
        hasNsAtApex=true;
      } else if (rr.qtype.getCode() == QType::DS) {
        cout<<"[Warning] DS at apex in zone '"<<zone.toString()<<"', should not be here."<<endl;
        numwarnings++;
      }
    } else {
      if (rr.qtype.getCode() == QType::SOA) {
        cout<<"[Error] SOA record not at apex '"<<rr.qname.toString()<<" IN "<<rr.qtype.getName()<<" "<<rr.content<<"' in zone '"<<zone.toString()<<"'"<<endl;
        numerrors++;
        continue;
      } else if (rr.qtype.getCode() == QType::DNSKEY) {
        cout<<"[Warning] DNSKEY record not at apex '"<<rr.qname.toString()<<" IN "<<rr.qtype.getName()<<" "<<rr.content<<"' in zone '"<<zone.toString()<<"', should not be here."<<endl;
        numwarnings++;
      } else if (rr.qtype.getCode() == QType::NS && DNSName(rr.content).isPartOf(rr.qname)) {
        checkglue.insert(DNSName(toLower(rr.content)));
      } else if (rr.qtype.getCode() == QType::A || rr.qtype.getCode() == QType::AAAA) {
        glue.insert(rr.qname);
      }
    }

    if (rr.qtype.getCode() == QType::CNAME) {
      if (!cnames.count(rr.qname))
        cnames.insert(rr.qname);
      else {
        cout<<"[Error] Duplicate CNAME found at '"<<rr.qname.toString()<<"'"<<endl;
        numerrors++;
        continue;
      }
    } else {
      if (rr.qtype.getCode() == QType::RRSIG) {
        if(!presigned) {
          cout<<"[Error] RRSIG found at '"<<rr.qname.toString()<<"' in non-presigned zone. These do not belong in the database."<<endl;
          numerrors++;
          continue;
        }
      } else
        noncnames.insert(rr.qname);
    }

    if(rr.qtype.getCode() == QType::NSEC || rr.qtype.getCode() == QType::NSEC3)
    {
      cout<<"[Error] NSEC or NSEC3 found at '"<<rr.qname.toString()<<"'. These do not belong in the database."<<endl;
      numerrors++;
      continue;
    }

    if(!presigned && rr.qtype.getCode() == QType::DNSKEY)
    {
      if(::arg().mustDo("direct-dnskey"))
      {
        if(rr.ttl != sd.default_ttl)
        {
          cout<<"[Warning] DNSKEY TTL of "<<rr.ttl<<" at '"<<rr.qname.toString()<<"' differs from SOA minimum of "<<sd.default_ttl<<endl;
          numwarnings++;
        }
      }
      else
      {
        cout<<"[Warning] DNSKEY at '"<<rr.qname.toString()<<"' in non-presigned zone will mostly be ignored and can cause problems."<<endl;
        numwarnings++;
      }
    }

    // if (rr.qname[rr.qname.size()-1] == '.') {
    //   cout<<"[Error] Record '"<<rr.qname.toString()<<"' has a trailing dot. PowerDNS will ignore this record!"<<endl;
    //   numerrors++;
    // }

    if ( (rr.qtype.getCode() == QType::NS || rr.qtype.getCode() == QType::SRV || rr.qtype.getCode() == QType::MX || rr.qtype.getCode() == QType::CNAME || rr.qtype.getCode() == QType::DNAME) &&
         rr.content[rr.content.size()-1] == '.') {
      cout<<"[Warning] The record "<<rr.qname.toString()<<" with type "<<rr.qtype.getName()<<" has a trailing dot in the content ("<<rr.content<<"). Your backend might not work well with this."<<endl;
      numwarnings++;
    }

    if(rr.auth == 0 && rr.qtype.getCode()!=QType::NS && rr.qtype.getCode()!=QType::A && rr.qtype.getCode()!=QType::AAAA)
    {
      cout<<"[Error] Following record is auth=0, run pdnsutil rectify-zone?: "<<rr.qname.toString()<<" IN " <<rr.qtype.getName()<< " " << rr.content<<endl;
      numerrors++;
    }
  }

  for(auto &i: cnames) {
    if (noncnames.find(i) != noncnames.end()) {
      cout<<"[Error] CNAME "<<i.toString()<<" found, but other records with same label exist."<<endl;
      numerrors++;
    }
  }

  for(const auto &i: tlsas) {
    DNSName name = DNSName(i);
    name.trimToLabels(name.getRawLabels().size()-2);
    if (cnames.find(name) == cnames.end() && noncnames.find(name) == noncnames.end()) {
      // No specific record for the name in the TLSA record exists, this
      // is already worth emitting a warning. Let's see if a wildcard exist.
      cout<<"[Warning] ";
      DNSName wcname(name);
      wcname.chopOff();
      wcname.prependRawLabel("*");
      if (cnames.find(wcname) != cnames.end() || noncnames.find(wcname) != noncnames.end()) {
        cout<<"A wildcard record exist for '"<<wcname.toString()<<"' and a TLSA record for '"<<i.toString()<<"'.";
      } else {
        cout<<"No record for '"<<name.toString()<<"' exists, but a TLSA record for '"<<i.toString()<<"' does.";
      }
      numwarnings++;
      cout<<" A query for '"<<name.toString()<<"' will yield an empty response. This is most likely a mistake, please create records for '"<<name.toString()<<"'."<<endl;
    }
  }

  if(!hasNsAtApex) {
    cout<<"[Error] No NS record at zone apex in zone '"<<zone.toString()<<"'"<<endl;
    numerrors++;
  }

  for(const auto &qname : checkglue) {
    if (!glue.count(qname)) {
      cerr<<"[Warning] Missing glue for '"<<qname.toString()<<"' in zone '"<<zone.toString()<<"'"<<endl;
      numwarnings++;
    }
  }

  cout<<"Checked "<<numrecords<<" records of '"<<zone.toString()<<"', "<<numerrors<<" errors, "<<numwarnings<<" warnings."<<endl;
  if(!numerrors)
    return 0;
  return 1;
}

int checkAllZones(DNSSECKeeper &dk, bool exitOnError)
{
  UeberBackend B("default");
  vector<DomainInfo> domainInfo;

  B.getAllDomains(&domainInfo, true);
  int errors=0;
  for(auto di : domainInfo) {
    if (checkZone(dk, B, di.zone) > 0) {
      errors++;
      if(exitOnError)
        return 1;
    }
  }
  cout<<"Checked "<<domainInfo.size()<<" zones, "<<errors<<" had errors."<<endl;
  if(!errors)
    return 0;
  return 1;
}

int increaseSerial(const DNSName& zone, DNSSECKeeper &dk)
{
  UeberBackend B("default");
  SOAData sd;
  if(!B.getSOAUncached(zone, sd)) {
    cout<<"No SOA for zone '"<<zone.toString()<<"'"<<endl;
    return -1;
  }

  if (dk.isPresigned(zone)) {
    cerr<<"Serial increase of presigned zone '"<<zone<<"' is not allowed."<<endl;
    return -1;
  }
  
  string soaEditKind;
  dk.getSoaEdit(zone, soaEditKind);

  sd.db->lookup(QType(QType::SOA), zone);
  vector<DNSResourceRecord> rrs;
  DNSResourceRecord rr;
  while (sd.db->get(rr)) {
    if (rr.qtype.getCode() == QType::SOA)
      rrs.push_back(rr);
  } 

  if (rrs.size() > 1) {
    cerr<<rrs.size()<<" SOA records found for "<<zone.toString()<<"!"<<endl;
    return -1;
  }
  if (rrs.size() < 1) {
     cerr<<zone.toString()<<" not found!"<<endl;
  }
  
  if (soaEditKind.empty()) {
    sd.serial++;
  }
  else if(pdns_iequals(soaEditKind,"INCREMENT-WEEKS")) {
    sd.serial++;
  }
  else if(pdns_iequals(soaEditKind,"INCEPTION-INCREMENT")) {
    uint32_t today_serial = localtime_format_YYYYMMDDSS(time(NULL), 1);

    if (sd.serial < today_serial) {
      sd.serial = today_serial;
    }
    else {
      sd.serial++;
    }
  }
  else {
    sd.serial = calculateEditSOA(sd, soaEditKind) + 1;
  }
  rrs[0].content = serializeSOAData(sd);

  sd.db->startTransaction(zone, -1);

  if (! sd.db->replaceRRSet(sd.domain_id, zone, rr.qtype, rrs)) {
   sd.db->abortTransaction();
   cerr<<"Backend did not replace SOA record. Backend might not support this operation."<<endl;
   return -1;
  }

  if (sd.db->doesDNSSEC()) {
    NSEC3PARAMRecordContent ns3pr;
    bool narrow;
    bool haveNSEC3=dk.getNSEC3PARAM(zone, &ns3pr, &narrow);
  
    DNSName ordername;
    if(haveNSEC3) {
      if(!narrow)
        ordername=DNSName(toBase32Hex(hashQNameWithSalt(ns3pr, zone))) + zone;
    } else
      ordername=zone;
    if(g_verbose)
      cerr<<"'"<<rrs[0].qname.toString()<<"' -> '"<< ordername.toString() <<"'"<<endl;
    sd.db->updateDNSSECOrderNameAndAuth(sd.domain_id, zone, rrs[0].qname, ordername, true);
  }

  sd.db->commitTransaction();

  cout<<"SOA serial for zone "<<zone.toString()<<" set to "<<sd.serial<<endl;
  return 0;
}

int deleteZone(const DNSName &zone) {
  UeberBackend B;
  DomainInfo di;
  if (! B.getDomainInfo(zone, di)) {
    cerr<<"Domain '"<<zone.toString()<<"' not found!"<<endl;
    return 1;
  }

  if(di.backend->deleteDomain(zone))
    return 0;

  cerr<<"Failed to delete domain '"<<zone.toString()<<"'"<<endl;;
  return 1;
}

void listKey(DomainInfo const &di, DNSSECKeeper& dk, bool printHeader = true) {
  if (printHeader) {
    cout<<"Zone                          Type    Size    Algorithm    ID   Location    Keytag"<<endl;
    cout<<"----------------------------------------------------------------------------------"<<endl;
  }
  unsigned int spacelen = 0;
  for (auto const &key : dk.getKeys(di.zone)) {
    cout<<di.zone.toStringNoDot();
    if (di.zone.toStringNoDot().length() > 29)
      cout<<endl<<string(30, ' ');
    else
      cout<<string(30 - di.zone.toStringNoDot().length(), ' ');

    cout<<(key.second.keyOrZone ? "KSK" : "ZSK")<<"     ";

    spacelen = (std::to_string(key.first.getKey()->getBits()).length() >= 8) ? 1 : 8 - std::to_string(key.first.getKey()->getBits()).length();
    if (key.first.getKey()->getBits() < 1) {
      cout<<"invalid "<<endl;
      continue; 
    } else {
      cout<<key.first.getKey()->getBits()<<string(spacelen, ' ');
    }

    string algname;
    algorithm2name(key.first.d_algorithm, algname);
    spacelen = (algname.length() >= 13) ? 1 : 13 - algname.length();
    cout<<algname<<string(spacelen, ' ');

    spacelen = (std::to_string(key.second.id).length() > 5) ? 1 : 5 - std::to_string(key.second.id).length();
    cout<<key.second.id<<string(spacelen, ' ');

#ifdef HAVE_P11KIT1
    auto stormap = key.first.getKey()->convertToISCVector();
    string engine, slot, label = "";
    for (auto const &elem : stormap) {
      //cout<<elem.first<<" "<<elem.second<<endl;
      if (elem.first == "Engine")
        engine = elem.second;
      if (elem.first == "Slot")
        slot = elem.second;
      if (elem.first == "Label")
        label = elem.second;
    }
    if (engine.empty() || slot.empty()){
      cout<<"cryptokeys  ";
    } else {
      spacelen = (engine.length()+slot.length()+label.length()+2 >= 12) ? 1 : 12 - engine.length()-slot.length()-label.length()-2;
      cout<<engine<<","<<slot<<","<<label<<string(spacelen, ' ');
    }
#else
    cout<<"cryptokeys  ";
#endif
    cout<<key.first.getDNSKEY().getTag()<<endl;
  }
}

bool listKeys(const string &zname, DNSSECKeeper& dk){
  UeberBackend B("default");

  if (zname != "all") {
    DomainInfo di;
    if(!B.getDomainInfo(DNSName(zname), di)) {
      cerr << "Zone "<<zname<<" not found."<<endl;
      return false;
    }
    listKey(di, dk);
  } else {
    vector<DomainInfo> domainInfo;
    B.getAllDomains(&domainInfo);
    bool printHeader = true;
    for (auto const di : domainInfo) {
      listKey(di, dk, printHeader);
      printHeader = false;
    }
  }
  return true;
}

int listZone(const DNSName &zone) {
  UeberBackend B;
  DomainInfo di;
  
  if (! B.getDomainInfo(zone, di)) {
    cerr<<"Domain '"<<zone.toString()<<"' not found!"<<endl;
    return 1;
  }
  di.backend->list(zone, di.id);
  DNSResourceRecord rr;
  while(di.backend->get(rr)) {
    if(rr.qtype.getCode()) {
      if ( (rr.qtype.getCode() == QType::NS || rr.qtype.getCode() == QType::SRV || rr.qtype.getCode() == QType::MX || rr.qtype.getCode() == QType::CNAME) && !rr.content.empty() && rr.content[rr.content.size()-1] != '.') 
	rr.content.append(1, '.');
	
      cout<<rr.qname.toString()<<"\t"<<rr.ttl<<"\tIN\t"<<rr.qtype.getName()<<"\t"<<rr.content<<endl;
    }
  }
  return 0;
}

int loadZone(DNSName zone, const string& fname) {
  UeberBackend B;
  DomainInfo di;

  if (B.getDomainInfo(zone, di)) {
    cerr<<"Domain '"<<zone.toString()<<"' exists already, replacing contents"<<endl;
  }
  else {
    cerr<<"Creating '"<<zone.toString()<<"'"<<endl;
    B.createDomain(zone);
    
    if(!B.getDomainInfo(zone, di)) {
      cerr<<"Domain '"<<zone.toString()<<"' was not created - perhaps backend ("<<::arg()["launch"]<<") does not support storing new zones."<<endl;
      return 1;
    }
  }
  DNSBackend* db = di.backend;
  ZoneParserTNG zpt(fname, zone);
  
  DNSResourceRecord rr;
  if(!db->startTransaction(zone, di.id)) {
    cerr<<"Unable to start transaction for load of zone '"<<zone.toString()<<"'"<<endl;
    return 1;
  }
  rr.domain_id=di.id;  
  while(zpt.get(rr)) {
    if(!rr.qname.isPartOf(zone) && rr.qname!=zone) {
      cerr<<"File contains record named '"<<rr.qname.toString()<<"' which is not part of zone '"<<zone.toString()<<"'"<<endl;
      return 1;
    }
    db->feedRecord(rr);
  }
  db->commitTransaction();
  return 0;
}

int createZone(const DNSName &zone) {
  UeberBackend B;
  DomainInfo di;
  if (B.getDomainInfo(zone, di)) {
    cerr<<"Domain '"<<zone.toString()<<"' exists already"<<endl;
    return 1;
  }
  cerr<<"Creating '"<<zone.toString()<<"'"<<endl;
  B.createDomain(zone);

  if(!B.getDomainInfo(zone, di)) {
    cerr<<"Domain '"<<zone.toString()<<"' was not created!"<<endl;
    return 1;
  }
  return 1;
}


int listAllZones(const string &type="") {

  int kindFilter = -1;
  if (type.size()) {
    if (toUpper(type) == "MASTER")
      kindFilter = 0;
    else if (toUpper(type) == "SLAVE")
      kindFilter = 1;
    else if (toUpper(type) == "NATIVE")
      kindFilter = 2;
    else {
      cerr<<"Syntax: pdnsutil list-all-zones [master|slave|native]"<<endl;
      return 1;
    }
  }

  UeberBackend B("default");

  vector<DomainInfo> domains;
  B.getAllDomains(&domains);

  int count = 0;
  for (vector<DomainInfo>::const_iterator di=domains.begin(); di != domains.end(); di++) {
    if (di->kind == kindFilter || kindFilter == -1) {
      cout<<di->zone.toString()<<endl;
      count++;
    }
  }

  if (kindFilter != -1)
    cout<<type<<" zonecount:"<<count<<endl;
  else
    cout<<"All zonecount:"<<count<<endl;
  return 0;
}

bool testAlgorithm(int algo)
{
  return DNSCryptoKeyEngine::testOne(algo);
}

bool testAlgorithms()
{
  return DNSCryptoKeyEngine::testAll();
}

void testSpeed(DNSSECKeeper& dk, const DNSName& zone, const string& remote, int cores)
{
  DNSResourceRecord rr;
  rr.qname=DNSName("blah")+zone;
  rr.qtype=QType::A;
  rr.ttl=3600;
  rr.auth=1;
  rr.qclass = QClass::IN;
  rr.d_place=DNSResourceRecord::ANSWER;
  
  UeberBackend db("key-only");
  
  if ( ! db.backends.size() )
  {
    throw runtime_error("No backends available for DNSSEC key storage");
  }

  ChunkedSigningPipe csp(DNSName(zone), 1, remote, cores);
  
  vector<DNSResourceRecord> signatures;
  uint32_t rnd;
  unsigned char* octets = (unsigned char*)&rnd;
  char tmp[25];
  DTime dt;
  dt.set();
  for(unsigned int n=0; n < 100000; ++n) {
    rnd = random();
    snprintf(tmp, sizeof(tmp), "%d.%d.%d.%d", 
      octets[0], octets[1], octets[2], octets[3]);
    rr.content=tmp;
    
    snprintf(tmp, sizeof(tmp), "r-%u", rnd);
    rr.qname=DNSName(tmp)+zone;
    
    if(csp.submit(rr))
      while(signatures = csp.getChunk(), !signatures.empty())
        ;
  }
  cerr<<"Flushing the pipe, "<<csp.d_signed<<" signed, "<<csp.d_queued<<" queued, "<<csp.d_outstanding<<" outstanding"<< endl;
  cerr<<"Net speed: "<<csp.d_signed/ (dt.udiffNoReset()/1000000.0) << " sigs/s"<<endl;
  while(signatures = csp.getChunk(true), !signatures.empty())
      ;
  cerr<<"Done, "<<csp.d_signed<<" signed, "<<csp.d_queued<<" queued, "<<csp.d_outstanding<<" outstanding"<< endl;
  cerr<<"Net speed: "<<csp.d_signed/ (dt.udiff()/1000000.0) << " sigs/s"<<endl;
}

void verifyCrypto(const string& zone)
{
  ZoneParserTNG zpt(zone);
  DNSResourceRecord rr;
  DNSKEYRecordContent drc;
  RRSIGRecordContent rrc;
  DSRecordContent dsrc;
  vector<shared_ptr<DNSRecordContent> > toSign;
  DNSName qname, apex;
  dsrc.d_digesttype=0;
  while(zpt.get(rr)) {
    if(rr.qtype.getCode() == QType::DNSKEY) {
      cerr<<"got DNSKEY!"<<endl;
      apex=rr.qname;
      drc = *dynamic_cast<DNSKEYRecordContent*>(DNSRecordContent::mastermake(QType::DNSKEY, 1, rr.content));
    }
    else if(rr.qtype.getCode() == QType::RRSIG) {
      cerr<<"got RRSIG"<<endl;
      rrc = *dynamic_cast<RRSIGRecordContent*>(DNSRecordContent::mastermake(QType::RRSIG, 1, rr.content));
    }
    else if(rr.qtype.getCode() == QType::DS) {
      cerr<<"got DS"<<endl;
      dsrc = *dynamic_cast<DSRecordContent*>(DNSRecordContent::mastermake(QType::DS, 1, rr.content));
    }
    else {
      qname = rr.qname;
      toSign.push_back(shared_ptr<DNSRecordContent>(DNSRecordContent::mastermake(rr.qtype.getCode(), 1, rr.content)));
    }
  }
  
  string msg = getMessageForRRSET(qname, rrc, toSign);        
  cerr<<"Verify: "<<DNSCryptoKeyEngine::makeFromPublicKeyString(drc.d_algorithm, drc.d_key)->verify(msg, rrc.d_signature)<<endl;
  if(dsrc.d_digesttype) {
    cerr<<"Calculated DS: "<<apex.toString()<<" IN DS "<<makeDSFromDNSKey(apex, drc, dsrc.d_digesttype).getZoneRepresentation()<<endl;
    cerr<<"Original DS:   "<<apex.toString()<<" IN DS "<<dsrc.getZoneRepresentation()<<endl;
  }
#if 0
  DNSCryptoKeyEngine*key=DNSCryptoKeyEngine::makeFromISCString(drc, "Private-key-format: v1.2\n"
      "Algorithm: 12 (ECC-GOST)\n"
      "GostAsn1: MEUCAQAwHAYGKoUDAgITMBIGByqFAwICIwEGByqFAwICHgEEIgQg/9MiXtXKg9FDXDN/R9CmVhJDyuzRAIgh4tPwCu4NHIs=\n");
  string resign=key->sign(hash);
  cerr<<Base64Encode(resign)<<endl;
  cerr<<"Verify: "<<DNSCryptoKeyEngine::makeFromPublicKeyString(drc.d_algorithm, drc.d_key)->verify(hash, resign)<<endl;
#endif

}
bool disableDNSSECOnZone(DNSSECKeeper& dk, const DNSName& zone)
{
  UeberBackend B("default");
  DomainInfo di;

  if (!B.getDomainInfo(zone, di)){
    cerr << "No such zone in the database" << endl;
    return false;
  }

  if(!dk.isSecuredZone(zone)) {
    cerr<<"Zone is not secured"<<endl;
    return false;
  }
  DNSSECKeeper::keyset_t keyset=dk.getKeys(zone);

  if(keyset.empty())  {
    cerr << "No keys for zone '"<<zone.toString()<<"'."<<endl;
  }
  else {  
    for(DNSSECKeeper::keyset_t::value_type value :  keyset) {
      dk.deactivateKey(zone, value.second.id);
      dk.removeKey(zone, value.second.id);
    }
  }
  dk.unsetNSEC3PARAM(zone);
  dk.unsetPresigned(zone);
  return true;
}
bool showZone(DNSSECKeeper& dk, const DNSName& zone)
{
  UeberBackend B("default");
  DomainInfo di;
  std::vector<std::string> meta;

  if (!B.getDomainInfo(zone, di)){
    cerr << "No such zone in the database" << endl;
    return false;
  }

  if(!dk.isSecuredZone(zone)) {
    cerr<<"Zone is not actively secured"<<endl;
  }
  NSEC3PARAMRecordContent ns3pr;
  bool narrow;
  bool haveNSEC3=dk.getNSEC3PARAM(zone, &ns3pr, &narrow);
  
  DNSSECKeeper::keyset_t keyset=dk.getKeys(zone);
  if (B.getDomainMetadata(zone, "TSIG-ALLOW-AXFR", meta) && meta.size() > 0) {
     cerr << "Zone has following allowed TSIG key(s): " << boost::join(meta, ",") << endl;
  }

  meta.clear();
  if (B.getDomainMetadata(zone, "AXFR-MASTER-TSIG", meta) && meta.size() > 0) {
     cerr << "Zone uses following TSIG key(s): " << boost::join(meta, ",") << endl;
  }
  
  cout <<"Zone is " << (dk.isPresigned(zone) ? "" : "not ") << "presigned"<<endl;

  if(keyset.empty())  {
    cerr << "No keys for zone '"<<zone.toString()<<"'."<<endl;
  }
  else {  
    if(!haveNSEC3) 
      cout<<"Zone has NSEC semantics"<<endl;
    else
      cout<<"Zone has " << (narrow ? "NARROW " : "") <<"hashed NSEC3 semantics, configuration: "<<ns3pr.getZoneRepresentation()<<endl;
  
    cout << "keys: "<<endl;
    for(DNSSECKeeper::keyset_t::value_type value :  keyset) {
      string algname;
      algorithm2name(value.first.d_algorithm, algname);
      if (value.first.getKey()->getBits() < 1) {
        cout<<"ID = "<<value.second.id<<" ("<<(value.second.keyOrZone ? "KSK" : "ZSK")<<") <key missing or defunct>" <<endl;
        continue;
      }
      cout<<"ID = "<<value.second.id<<" ("<<(value.second.keyOrZone ? "KSK" : "ZSK")<<"), tag = "<<value.first.getDNSKEY().getTag();
      cout<<", algo = "<<(int)value.first.d_algorithm<<", bits = "<<value.first.getKey()->getBits()<<"\t"<<((int)value.second.active == 1 ? "  A" : "Ina")<<"ctive ( " + algname + " ) "<<endl;
      if(value.second.keyOrZone || ::arg().mustDo("direct-dnskey") || 1)
        cout<<(value.second.keyOrZone ? "KSK" : "ZSK")<<" DNSKEY = "<<zone.toString()<<" IN DNSKEY "<< value.first.getDNSKEY().getZoneRepresentation() << " ; ( "  + algname + " )" << endl;
      if(value.second.keyOrZone || 1) {
        cout<<"DS = "<<zone.toString()<<" IN DS "<<makeDSFromDNSKey(zone, value.first.getDNSKEY(), 1).getZoneRepresentation() << " ; ( SHA1 digest )" << endl;
        cout<<"DS = "<<zone.toString()<<" IN DS "<<makeDSFromDNSKey(zone, value.first.getDNSKEY(), 2).getZoneRepresentation() << " ; ( SHA256 digest )" << endl;
        try {
          string output=makeDSFromDNSKey(zone, value.first.getDNSKEY(), 3).getZoneRepresentation();
          cout<<"DS = "<<zone.toString()<<" IN DS "<< output << " ; ( GOST R 34.11-94 digest )" << endl;
        }
        catch(...)
        {
        }
        try {
          string output=makeDSFromDNSKey(zone, value.first.getDNSKEY(), 4).getZoneRepresentation();
          cout<<"DS = "<<zone.toString()<<" IN DS "<< output << " ; ( SHA-384 digest )" << endl;
        }
        catch(...)
        {
        }
        cout<<endl;  
      }
    }
  }
  return true;
}

bool secureZone(DNSSECKeeper& dk, const DNSName& zone)
{
  // parse attribute
  vector<string> k_algos;
  vector<string> z_algos;
  int k_size;
  int z_size; 

  stringtok(k_algos, ::arg()["default-ksk-algorithms"], " ,");
  k_size = ::arg().asNum("default-ksk-size");
  stringtok(z_algos, ::arg()["default-zsk-algorithms"], " ,");
  z_size = ::arg().asNum("default-zsk-size");

  if (k_size < 0) {
     throw runtime_error("KSK key size must be equal to or greater than 0");
  }

  if (k_algos.size() < 1 && z_algos.size() < 1) {
     throw runtime_error("Zero algorithms given for KSK+ZSK in total");
  }

  if (z_size < 0) {
     throw runtime_error("ZSK key size must be equal to or greater than 0");
  }

  if(dk.isSecuredZone(zone)) {
    cerr << "Zone '"<<zone.toString()<<"' already secure, remove keys with pdnsutil remove-zone-key if needed"<<endl;
    return false;
  }

  DomainInfo di;
  UeberBackend B("default");
  if(!B.getDomainInfo(zone, di) || !di.backend) { // di.backend and B are mostly identical
    cout<<"Can't find a zone called '"<<zone.toString()<<"'"<<endl;
    return false;
  }

  if(di.kind == DomainInfo::Slave)
  {
    cout<<"Warning! This is a slave domain! If this was a mistake, please run"<<endl;
    cout<<"pdnsutil disable-dnssec "<<zone.toString()<<" right now!"<<endl;
  }

  if (k_size)
    cout << "Securing zone with key size " << k_size << endl;
  else
    cout << "Securing zone with default key size" << endl;


  DNSSECKeeper::keyset_t zskset=dk.getKeys(zone, false);

  if(!zskset.empty())  {
    cerr<<"There were ZSKs already for zone '"<<zone.toString()<<"', no need to add more"<<endl;
    return false;
  }

  for(auto &k_algo: k_algos) {
    cout << "Adding KSK with algorithm " << k_algo << endl;

    int algo = shorthand2algorithm(k_algo);

    if(!dk.addKey(zone, true, algo, k_size, true)) {
      cerr<<"No backend was able to secure '"<<zone.toString()<<"', most likely because no DNSSEC"<<endl;
      cerr<<"capable backends are loaded, or because the backends have DNSSEC disabled."<<endl;
      cerr<<"For the Generic SQL backends, set the 'gsqlite3-dnssec', 'gmysql-dnssec' or"<<endl;
      cerr<<"'gpgsql-dnssec' flag. Also make sure the schema has been updated for DNSSEC!"<<endl;
      return false;
    }
  }

  for(auto &z_algo :  z_algos)
  {
    cout << "Adding ZSK with algorithm " << z_algo << endl;

    int algo = shorthand2algorithm(z_algo);

    if(!dk.addKey(zone, false, algo, z_size, true)) {
      cerr<<"No backend was able to secure '"<<zone.toString()<<"', most likely because no DNSSEC"<<endl;
      cerr<<"capable backends are loaded, or because the backends have DNSSEC disabled."<<endl;
      cerr<<"For the Generic SQL backends, set the 'gsqlite3-dnssec', 'gmysql-dnssec' or"<<endl;
      cerr<<"'gpgsql-dnssec' flag. Also make sure the schema has been updated for DNSSEC!"<<endl;
      return false;
    }
  }

  if(!dk.isSecuredZone(zone)) {
    cerr<<"Failed to secure zone. Is your backend dnssec enabled? (set "<<endl;
    cerr<<"gsqlite3-dnssec, or gmysql-dnssec etc). Check this first."<<endl;
    cerr<<"If you run with the BIND backend, make sure you have configured"<<endl;
    cerr<<"it to use DNSSEC with 'bind-dnssec-db=/path/fname' and"<<endl;
    cerr<<"'pdnsutil create-bind-db /path/fname'!"<<endl;
    return false;
  }

  // rectifyZone(dk, zone);
  // showZone(dk, zone);
  cout<<"Zone "<<zone.toString()<<" secured"<<endl;
  return true;
}

void testSchema(DNSSECKeeper& dk, const DNSName& zone)
{
  cout<<"Note: test-schema will try to create the zone, but it will not remove it."<<endl;
  cout<<"Please clean up after this."<<endl;
  cout<<endl;
  cout<<"Constructing UeberBackend"<<endl;
  UeberBackend B("default");
  cout<<"Picking first backend - if this is not what you want, edit launch line!"<<endl;
  DNSBackend *db = B.backends[0];
  cout<<"Creating slave domain "<<zone.toString()<<endl;
  db->createSlaveDomain("127.0.0.1", zone, "", "_testschema");
  cout<<"Slave domain created"<<endl;

  DomainInfo di;
  if(!B.getDomainInfo(zone, di) || !di.backend) { // di.backend and B are mostly identical
    cout<<"Can't find domain we just created, aborting"<<endl;
    return;
  }
  db=di.backend;
  DNSResourceRecord rr, rrget;
  cout<<"Starting transaction to feed records"<<endl;
  db->startTransaction(zone, di.id);

  rr.qtype=QType::SOA;
  rr.qname=zone;
  rr.ttl=86400;
  rr.domain_id=di.id;
  rr.auth=1;
  rr.content="ns1.example.com. ahu.example.com. 2012081039 7200 3600 1209600 3600";
  cout<<"Feeding SOA"<<endl;
  db->feedRecord(rr);
  rr.qtype=QType::TXT;
  // 300 As
  rr.content="\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"";
  cout<<"Feeding overlong TXT"<<endl;
  db->feedRecord(rr);
  cout<<"Committing"<<endl;
  db->commitTransaction();
  cout<<"Querying TXT"<<endl;
  db->lookup(QType(QType::TXT), zone, NULL, di.id);
  if(db->get(rrget))
  {
    DNSResourceRecord rrthrowaway;
    if(db->get(rrthrowaway)) // should not touch rr but don't assume anything
    {
      cout<<"Expected one record, got multiple, aborting"<<endl;
      return;
    }
    int size=rrget.content.size();
    if(size != 302)
    {
      cout<<"Expected 302 bytes, got "<<size<<", aborting"<<endl;
      return;
    }
  }
  cout<<"[+] content field is over 255 bytes"<<endl;

  cout<<"Dropping all records, inserting SOA+2xA"<<endl;
  db->startTransaction(zone, di.id);

  rr.qtype=QType::SOA;
  rr.qname=zone;
  rr.ttl=86400;
  rr.domain_id=di.id;
  rr.auth=1;
  rr.content="ns1.example.com. ahu.example.com. 2012081039 7200 3600 1209600 3600";
  cout<<"Feeding SOA"<<endl;
  db->feedRecord(rr);

  rr.qtype=QType::A;
  rr.qname=DNSName("_underscore")+zone;
  rr.content="127.0.0.1";
  db->feedRecord(rr);

  rr.qname=DNSName("bla")+zone;
  cout<<"Committing"<<endl;
  db->commitTransaction();

  cout<<"Securing zone"<<endl;
  secureZone(dk, zone);
  cout<<"Rectifying zone"<<endl;
  rectifyZone(dk, zone);
  cout<<"Checking underscore ordering"<<endl;
  DNSName before, after;
  db->getBeforeAndAfterNames(di.id, zone, DNSName("z")+zone, before, after);
  cout<<"got '"<<before.toString()<<"' < 'z."<<zone.toString()<<"' < '"<<after.toString()<<"'"<<endl;
  if(before != DNSName("_underscore")+zone)
  {
    cout<<"before is wrong, got '"<<before.toString()<<"', expected '_underscore."<<zone.toString()<<"', aborting"<<endl;
    return;
  }
  if(after != zone)
  {
    cout<<"after is wrong, got '"<<after.toString()<<"', expected '"<<zone.toString()<<"', aborting"<<endl;
    return;
  }
  cout<<"[+] ordername sorting is correct for names starting with _"<<endl;
  cout<<endl;
  cout<<"End of tests, please remove "<<zone.toString()<<" from domains+records"<<endl;
}

int main(int argc, char** argv)
try
{  
  po::options_description desc("Allowed options");
  desc.add_options()
    ("help,h", "produce help message")
    ("verbose,v", "be verbose")
    ("force", "force an action")
    ("config-name", po::value<string>()->default_value(""), "virtual configuration name")
    ("config-dir", po::value<string>()->default_value(SYSCONFDIR), "location of pdns.conf")
    ("commands", po::value<vector<string> >());

  po::positional_options_description p;
  p.add("commands", -1);
  po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), g_vm);
  po::notify(g_vm);

  vector<string> cmds;

  if(g_vm.count("commands")) 
    cmds = g_vm["commands"].as<vector<string> >();

  g_verbose = g_vm.count("verbose");

  if(cmds.empty() || g_vm.count("help")) {
    cerr<<"Usage: \npdnsutil [options] <command> [params ..]\n"<<endl;
    cerr<<"Commands:"<<endl;
    cerr<<"activate-tsig-key ZONE NAME {master|slave}"<<endl;
    cerr<<"                                   Enable TSIG key for a zone"<<endl;
    cerr<<"activate-zone-key ZONE KEY-ID      Activate the key with key id KEY-ID in ZONE"<<endl;
    cerr<<"add-zone-key ZONE {zsk|ksk} [BITS] [active|inactive]"<<endl;
    cerr<<"             [rsasha1|rsasha256|rsasha512|gost|ecdsa256|ecdsa384";
#ifdef HAVE_LIBSODIUM
    cerr<<"|experimental-ed25519";
#endif
    cerr<<"]"<<endl;
    cerr<<"                                   Add a ZSK or KSK to zone and specify algo&bits"<<endl;
    cerr<<"backend-cmd BACKEND CMD [CMD..]    Perform one or more backend commands"<<endl;
    cerr<<"b2b-migrate OLD NEW                Move all data from one backend to another"<<endl;
    cerr<<"bench-db [filename]                Bench database backend with queries, one domain per line"<<endl;
    cerr<<"check-zone ZONE                    Check a zone for correctness"<<endl;
    cerr<<"check-all-zones [exit-on-error]    Check all zones for correctness. Set exit-on-error to exit immediately"<<endl;
    cerr<<"                                   after finding an error in a zone."<<endl;
    cerr<<"create-bind-db FNAME               Create DNSSEC db for BIND backend (bind-dnssec-db)"<<endl;
    cerr<<"create-zone ZONE                   Create empty zone ZONE"<<endl;
    cerr<<"deactivate-tsig-key ZONE NAME {master|slave}"<<endl;
    cerr<<"                                   Disable TSIG key for a zone"<<endl;
    cerr<<"deactivate-zone-key ZONE KEY-ID    Deactivate the key with key id KEY-ID in ZONE"<<endl;
    cerr<<"delete-tsig-key NAME               Delete TSIG key (warning! will not unmap key!)"<<endl;
    cerr<<"delete-zone ZONE                   Delete the zone"<<endl;
    cerr<<"disable-dnssec ZONE                Deactivate all keys and unset PRESIGNED in ZONE"<<endl;
    cerr<<"export-zone-dnskey ZONE KEY-ID     Export to stdout the public DNSKEY described"<<endl;
    cerr<<"export-zone-key ZONE KEY-ID        Export to stdout the private key described"<<endl;
    cerr<<"generate-tsig-key NAME ALGORITHM   Generate new TSIG key"<<endl;
    cerr<<"generate-zone-key {zsk|ksk} [ALGORITHM] [BITS]"<<endl;
    cerr<<"                                   Generate a ZSK or KSK to stdout with specified ALGORITHM and BITS"<<endl;
    cerr<<"get-meta ZONE [KIND ...]           Get zone metadata. If no KIND given, lists all known"<<endl;
    cerr<<"hash-zone-record ZONE RNAME        Calculate the NSEC3 hash for RNAME in ZONE"<<endl;
#ifdef HAVE_P11KIT1
    cerr<<"hsm assign ZONE ALGORITHM {ksk|zsk} MODULE SLOT PIN LABEL"<<endl<<
          "                                   Assign a hardware signing module to a ZONE"<<endl;
    cerr<<"hsm create-key ZONE KEY-ID [BITS]  Create a key using hardware signing module for ZONE (use assign first)"<<endl; 
    cerr<<"                                   BITS defaults to 2048"<<endl;
#endif
    cerr<<"increase-serial ZONE               Increases the SOA-serial by 1. Uses SOA-EDIT"<<endl;
    cerr<<"import-tsig-key NAME ALGORITHM KEY Import TSIG key"<<endl;
    cerr<<"import-zone-key ZONE FILE          Import from a file a private key, ZSK or KSK"<<endl;
    cerr<<"       [active|inactive] [ksk|zsk]  Defaults to KSK and active"<<endl;
    cerr<<"load-zone ZONE FILE                Load ZONE from FILE, possibly creating zone or atomically"<<endl;
    cerr<<"                                   replacing contents"<<endl;
    cerr<<"list-keys [ZONE]                   List DNSSEC keys for ZONE. When ZONE is unset or \"all\", display all keys for all zones"<<endl;
    cerr<<"list-zone ZONE                     List zone contents"<<endl;
    cerr<<"list-all-zones [master|slave|native]"<<endl;
    cerr<<"                                   List all zone names"<<endl;;
    cerr<<"list-tsig-keys                     List all TSIG keys"<<endl;
    cerr<<"rectify-zone ZONE [ZONE ..]        Fix up DNSSEC fields (order, auth)"<<endl;
    cerr<<"rectify-all-zones                  Rectify all zones."<<endl;
    cerr<<"remove-zone-key ZONE KEY-ID        Remove key with KEY-ID from ZONE"<<endl;
    cerr<<"secure-all-zones [increase-serial] Secure all zones without keys."<<endl;
    cerr<<"secure-zone ZONE [ZONE ..]         Add KSK and two ZSKs for ZONE"<<endl;
    cerr<<"set-nsec3 ZONE ['PARAMS' [narrow]] Enable NSEC3 with PARAMS. Optionally narrow"<<endl;
    cerr<<"set-presigned ZONE                 Use presigned RRSIGs from storage"<<endl;
    cerr<<"set-publish-cdnskey ZONE           Enable sending CDNSKEY responses for ZONE"<<endl;
    cerr<<"set-publish-cds ZONE [DIGESTALGOS] Enable sending CDS responses for ZONE, using DIGESTALGOS as signature algirithms"<<endl;
    cerr<<"                                   DIGESTALGOS should be a comma separated list of numbers, is is '1,2' by default"<<endl;
    cerr<<"set-meta ZONE KIND [VALUE ..]"<<endl;
    cerr<<"                                   Set zone metadata, optionally providing a value. Empty clears meta."<<endl;
    cerr<<"show-zone ZONE                     Show DNSSEC (public) key details about a zone"<<endl;
    cerr<<"unset-nsec3 ZONE                   Switch back to NSEC"<<endl;
    cerr<<"unset-presigned ZONE               No longer use presigned RRSIGs"<<endl;
    cerr<<"unset-publish-cdnskey ZONE         Disable sending CDNSKEY responses for ZONE"<<endl;
    cerr<<"unset-publish-cds ZONE             Disable sending CDS responses for ZONE"<<endl;
    cerr<<"test-schema ZONE                   Test DB schema - will create ZONE"<<endl;
    cerr<<desc<<endl;
    return 0;
  }

#ifdef HAVE_LIBSODIUM
  if (sodium_init() == -1) {
    cerr<<"Unable to initialize sodium crypto library"<<endl;
    exit(99);
  }
#endif

  if (cmds[0] == "test-algorithm") {
    if(cmds.size() != 2) {
      cerr << "Syntax: pdnsutil test-algorithm algonum"<<endl;
      return 0;
    }
    if (testAlgorithm(pdns_stou(cmds[1])))
      return 0;
    return 1;
  }

  if(cmds[0] == "test-algorithms") {
    if (testAlgorithms())
      return 0;
    return 1;
  }

  loadMainConfig(g_vm["config-dir"].as<string>());
  reportAllTypes();

  if(cmds[0] == "create-bind-db") {
#ifdef HAVE_SQLITE3
    if(cmds.size() != 2) {
      cerr << "Syntax: pdnsutil create-bind-db FNAME"<<endl;
      return 0;
    }
    try {
      SSQLite3 db(cmds[1], true); // create=ok
      vector<string> statements;
      stringtok(statements, sqlCreate, ";");
      for(const string& statement :  statements) {
        db.execute(statement);
      }
    }
    catch(SSqlException& se) {
      throw PDNSException("Error creating database in BIND backend: "+se.txtReason());
    }
    return 0;
#else
    cerr<<"bind-dnssec-db requires building PowerDNS with SQLite3"<<endl;
    return 1;
#endif
  }

  DNSSECKeeper dk;

  if (cmds[0] == "test-schema") {
    if(cmds.size() != 2) {
      cerr << "Syntax: pdnsutil test-schema ZONE"<<endl;
      return 0;
    }
    testSchema(dk, DNSName(cmds[1]));
    return 0;
  }
  if(cmds[0] == "rectify-zone") {
    if(cmds.size() < 2) {
      cerr << "Syntax: pdnsutil rectify-zone ZONE [ZONE..]"<<endl;
      return 0;
    }
    unsigned int exitCode = 0;
    for(unsigned int n = 1; n < cmds.size(); ++n) 
      if (!rectifyZone(dk, DNSName(cmds[n]))) 
	exitCode = 1;
    return exitCode;
  }
  else if (cmds[0] == "rectify-all-zones") {
    rectifyAllZones(dk);
  }
  else if(cmds[0] == "check-zone") {
    if(cmds.size() != 2) {
      cerr << "Syntax: pdnsutil check-zone ZONE"<<endl;
      return 0;
    }
    UeberBackend B("default");
    exit(checkZone(dk, B, DNSName(cmds[1])));
  }
  else if(cmds[0] == "bench-db") {
    dbBench(cmds.size() > 1 ? cmds[1] : "");
  }
  else if (cmds[0] == "check-all-zones") {
    bool exitOnError = (cmds[1] == "exit-on-error");
    exit(checkAllZones(dk, exitOnError));
  }
  else if (cmds[0] == "list-all-zones") {
    if (cmds.size() > 2) {
      cerr << "Syntax: pdnsutil list-all-zones [master|slave|native]"<<endl;
      return 0;
    }
    if (cmds.size() == 2)
      return listAllZones(cmds[1]);
    return listAllZones();
  }
  else if (cmds[0] == "test-zone") {
    cerr << "Did you mean check-zone?"<<endl;
    return 0;
  }
  else if (cmds[0] == "test-all-zones") {
    cerr << "Did you mean check-all-zones?"<<endl;
    return 0;
  }
#if 0
  else if(cmds[0] == "signing-server" )
  {
    signingServer();
  }
  else if(cmds[0] == "signing-slave")
  {
    launchSigningService(0);
  }
#endif
  else if(cmds[0] == "test-speed") {
    if(cmds.size() < 2) {
      cerr << "Syntax: pdnsutil test-speed numcores [signing-server]"<<endl;
      return 0;
    }
    testSpeed(dk, DNSName(cmds[1]),  (cmds.size() > 3) ? cmds[3] : "", pdns_stou(cmds[2]));
  }
  else if(cmds[0] == "verify-crypto") {
    if(cmds.size() != 2) {
      cerr << "Syntax: pdnsutil verify-crypto FILE"<<endl;
      return 0;
    }
    verifyCrypto(cmds[1]);
  }

  else if(cmds[0] == "show-zone") {
    if(cmds.size() != 2) {
      cerr << "Syntax: pdnsutil show-zone ZONE"<<endl;
      return 0;
    }
    if (!showZone(dk, DNSName(cmds[1]))) return 1;
  }
  else if(cmds[0] == "disable-dnssec") {
    if(cmds.size() != 2) {
      cerr << "Syntax: pdnsutil disable-dnssec ZONE"<<endl;
      return 0;
    }
    DNSName zone(cmds[1]);
    if(!disableDNSSECOnZone(dk, zone)) {
      cerr << "Cannot disable DNSSEC on " << zone << endl;
      return 1;
    }
  }
  else if(cmds[0] == "activate-zone-key") {
    if(cmds.size() != 3) {
      cerr << "Syntax: pdnsutil activate-zone-key ZONE KEY-ID"<<endl;
      return 0;
    }
    DNSName zone(cmds[1]);
    unsigned int id=pdns_stou(cmds[2]);
    if(!id)
    {
      cerr<<"Invalid KEY-ID"<<endl;
      return 1;
    }
    if (!dk.activateKey(zone, id)) {
      cerr<<"Activation of key failed"<<endl;
      return 1;
    }
    return 0;
  }
  else if(cmds[0] == "deactivate-zone-key") {
    if(cmds.size() != 3) {
      cerr << "Syntax: pdnsutil deactivate-zone-key ZONE KEY-ID"<<endl;
      return 0;
    }
    DNSName zone(cmds[1]);
    unsigned int id=pdns_stou(cmds[2]);
    if(!id)
    {
      cerr<<"Invalid KEY-ID"<<endl;
      return 1;
    }
    if (!dk.deactivateKey(zone, id)) {
      cerr<<"Deactivation of key failed"<<endl;
      return 1;
    }
    return 0;
  }
  else if(cmds[0] == "add-zone-key") {
    if(cmds.size() < 3 ) {
      cerr << "Syntax: pdnsutil add-zone-key ZONE zsk|ksk [bits] [rsasha1|rsasha256|rsasha512|gost|ecdsa256|ecdsa384]"<<endl;
      return 0;
    }
    DNSName zone(cmds[1]);

    UeberBackend B("default");
    DomainInfo di;

    if (!B.getDomainInfo(zone, di)){
      cerr << "No such zone in the database" << endl;
      return 0;
    }

    // need to get algorithm, bits & ksk or zsk from commandline
    bool keyOrZone=false;
    int tmp_algo=0;
    int bits=0;
    int algorithm=8;
    bool active=false;
    for(unsigned int n=2; n < cmds.size(); ++n) {
      if(pdns_iequals(cmds[n], "zsk"))
        keyOrZone = false;
      else if(pdns_iequals(cmds[n], "ksk"))
        keyOrZone = true;
      else if((tmp_algo = shorthand2algorithm(cmds[n]))>0) {
        algorithm = tmp_algo;
      } else if(pdns_iequals(cmds[n], "active")) {
        active=true;
      } else if(pdns_iequals(cmds[n], "inactive") || pdns_iequals(cmds[n], "passive")) { // 'passive' eventually needs to be removed
        active=false;
      } else if(pdns_stou(cmds[n])) {
        bits = pdns_stou(cmds[n]);
      } else { 
        cerr<<"Unknown algorithm, key flag or size '"<<cmds[n]<<"'"<<endl;
        exit(EXIT_FAILURE);;
      }
    }
    if(!dk.addKey(zone, keyOrZone, algorithm, bits, active)) {
      cerr<<"Adding key failed, perhaps DNSSEC not enabled in configuration?"<<endl;
      exit(1);
    }
    else {
      cerr<<"Added a " << (keyOrZone ? "KSK" : "ZSK")<<" with algorithm = "<<algorithm<<", active="<<active<<endl;
      if(bits)
        cerr<<"Requested specific key size of "<<bits<<" bits"<<endl;
    }
  }
  else if(cmds[0] == "remove-zone-key") {
    if(cmds.size() < 3) {
      cerr<<"Syntax: pdnsutil remove-zone-key ZONE KEY-ID"<<endl;
      return 0;
    }
    DNSName zone(cmds[1]);
    unsigned int id=pdns_stou(cmds[2]);
    if (!dk.removeKey(zone, id)) {
       cerr<<"Cannot remove key " << id << " from " << zone <<endl;
      return 1;
    }
    return 0;
  }
  else if(cmds[0] == "delete-zone") {
    if(cmds.size() != 2) {
      cerr<<"Syntax: pdnsutil delete-zone ZONE"<<endl;
      return 0;
    }
    exit(deleteZone(DNSName(cmds[1])));
  }
  else if(cmds[0] == "create-zone") {
    if(cmds.size() != 2) {
      cerr<<"Syntax: pdnsutil create-zone ZONE"<<endl;
      return 0;
    }
    exit(createZone(DNSName(cmds[1])));
  }
  else if(cmds[0] == "list-zone") {
    if(cmds.size() != 2) {
      cerr<<"Syntax: pdnsutil list-zone ZONE"<<endl;
      return 0;
    }
    if(cmds[1]==".")
      cmds[1].clear();

    exit(listZone(DNSName(cmds[1])));
  }
  else if(cmds[0] == "list-keys") {
    if(cmds.size() > 2) {
      cerr<<"Syntax: pdnsutil list-keys [ZONE]"<<endl;
      return 0;
    }
    string zname = (cmds.size() == 2) ? cmds[1] : "all";
    exit(listKeys(zname, dk));
  }
  else if(cmds[0] == "load-zone") {
    if(cmds.size() != 3) {
      cerr<<"Syntax: pdnsutil load-zone ZONE FILENAME"<<endl;
      return 0;
    }
    if(cmds[1]==".")
      cmds[1].clear();

    exit(loadZone(DNSName(cmds[1]), cmds[2]));
  }
  else if(cmds[0] == "secure-zone") {
    if(cmds.size() < 2) {
      cerr << "Syntax: pdnsutil secure-zone ZONE"<<endl;
      return 0;
    }
    vector<DNSName> mustRectify;
    unsigned int zoneErrors=0;
    for(unsigned int n = 1; n < cmds.size(); ++n) {
      DNSName zone(cmds[n]);
      dk.startTransaction(zone, -1);
      if(secureZone(dk, zone)) {
        mustRectify.push_back(zone);
      } else {
        zoneErrors++;
      }
      dk.commitTransaction();
    }
    
    for(const auto& zone : mustRectify)
      rectifyZone(dk, zone);

    if (zoneErrors) {
      return 1;
    }
    return 0;
  }
  else if (cmds[0] == "secure-all-zones") {
    if (cmds.size() >= 2 && !pdns_iequals(cmds[1], "increase-serial")) {
      cerr << "Syntax: pdnsutil secure-all-zones [increase-serial]"<<endl;
      return 0;
    }

    UeberBackend B("default");

    vector<DomainInfo> domainInfo;
    B.getAllDomains(&domainInfo);

    unsigned int zonesSecured=0, zoneErrors=0;
    for(DomainInfo di :  domainInfo) {
      if(!dk.isSecuredZone(di.zone)) {
        cout<<"Securing "<<di.zone.toString()<<": ";
        if (secureZone(dk, di.zone)) {
          zonesSecured++;
          if (cmds.size() == 2) {
            if (!increaseSerial(di.zone, dk))
              continue;
          } else
            continue;
        }
        zoneErrors++;
      }
    }

    cout<<"Secured: "<<zonesSecured<<" zones. Errors: "<<zoneErrors<<endl;

    if (zoneErrors) {
      return 1;
    }
    return 0;
  }
  else if(cmds[0]=="set-nsec3") {
    if(cmds.size() < 2) {
      cerr<<"Syntax: pdnsutil set-nsec3 ZONE 'params' [narrow]"<<endl;
      return 0;
    }
    string nsec3params =  cmds.size() > 2 ? cmds[2] : "1 0 1 ab";
    bool narrow = cmds.size() > 3 && cmds[3]=="narrow";
    NSEC3PARAMRecordContent ns3pr(nsec3params);

    DNSName zone(cmds[1]);
    if (zone.wirelength() > 222) {
      cerr<<"Cannot enable NSEC3 for " << zone.toString() << " as it is too long (" << zone.wirelength() << " bytes, maximum is 222 bytes)"<<endl;
      return 1;
    }
    if (! dk.setNSEC3PARAM(zone, ns3pr, narrow)) {
      cerr<<"Cannot set NSEC3 param for " << zone.toString() << endl;
      return 1;
    }

    if (!ns3pr.d_flags)
      cerr<<"NSEC3 set, ";
    else
      cerr<<"NSEC3 (opt-out) set, ";

    if(dk.isSecuredZone(zone))
      cerr<<"please rectify your zone if your backend needs it"<<endl;
    else
      cerr<<"please secure and rectify your zone."<<endl;

    return 0;
  }
  else if(cmds[0]=="set-presigned") {
    if(cmds.size() < 2) {
      cerr<<"Syntax: pdnsutil set-presigned ZONE"<<endl;
      return 0; 
    }
    if (! dk.setPresigned(DNSName(cmds[1]))) {
      cerr << "Could not set presigned on for " << cmds[1] << endl;
      return 1;
    }
    return 0;
  }
  else if(cmds[0]=="set-publish-cdnskey") {
    if(cmds.size() < 2) {
      cerr<<"Syntax: pdnsutil set-publish-cdnskey ZONE"<<endl;
      return 0;
    }
    if (! dk.setPublishCDNSKEY(DNSName(cmds[1]))) {
      cerr << "Could not set publishing for CDNSKEY records for "<< cmds[1]<<endl;
      return 1;
    }
    return 0;
  }
  else if(cmds[0]=="set-publish-cds") {
    if(cmds.size() < 2) {
      cerr<<"Syntax: pdnsutil set-publish-cds ZONE [DIGESTALGOS]"<<endl;
      return 0;
    }

    // If DIGESTALGOS is unset
    if(cmds.size() == 2)
      cmds.push_back("1,2");

    if (! dk.setPublishCDS(DNSName(cmds[1]), cmds[2])) {
      cerr << "Could not set publishing for CDS records for "<< cmds[1]<<endl;
      return 1;
    }
    return 0;
  }
  else if(cmds[0]=="unset-presigned") {
    if(cmds.size() < 2) {
      cerr<<"Syntax: pdnsutil unset-presigned ZONE"<<endl;
      return 0;  
    }
    if (! dk.unsetPresigned(DNSName(cmds[1]))) {
      cerr << "Could not unset presigned on for " << cmds[1] << endl;
      return 1;
    }
    return 0;
  }
  else if(cmds[0]=="unset-publish-cdnskey") {
    if(cmds.size() < 2) {
      cerr<<"Syntax: pdnsutil unset-publish-cdnskey ZONE"<<endl;
      return 0;
    }
    if (! dk.unsetPublishCDNSKEY(DNSName(cmds[1]))) {
      cerr << "Could not unset publishing for CDNSKEY records for "<< cmds[1]<<endl;
      return 1;
    }
    return 0;
  }
  else if(cmds[0]=="unset-publish-cds") {
    if(cmds.size() < 2) {
      cerr<<"Syntax: pdnsutil unset-publish-cds ZONE"<<endl;
      return 0;
    }
    if (! dk.unsetPublishCDS(DNSName(cmds[1]))) {
      cerr << "Could not unset publishing for CDS records for "<< cmds[1]<<endl;
      return 1;
    }
    return 0;
  }
  else if(cmds[0]=="hash-zone-record") {
    if(cmds.size() < 3) {
      cerr<<"Syntax: pdnsutil hash-zone-record ZONE RNAME"<<endl;
      return 0;
    }
    DNSName zone(cmds[1]);
    DNSName record(cmds[2]);
    NSEC3PARAMRecordContent ns3pr;
    bool narrow;
    if(!dk.getNSEC3PARAM(zone, &ns3pr, &narrow)) {
      cerr<<"The '"<<zone.toString()<<"' zone does not use NSEC3"<<endl;
      return 0;
    }
    if(narrow) {
      cerr<<"The '"<<zone.toString()<<"' zone uses narrow NSEC3, but calculating hash anyhow"<<endl;
    }
      
    cout<<toBase32Hex(hashQNameWithSalt(ns3pr, record))<<endl;
  }
  else if(cmds[0]=="unset-nsec3") {
    if(cmds.size() < 2) {
      cerr<<"Syntax: pdnsutil unset-nsec3 ZONE"<<endl;
      return 0;
    }
    if ( ! dk.unsetNSEC3PARAM(DNSName(cmds[1]))) {
      cerr<<"Cannot unset NSEC3 param for " << cmds[1] << endl;
      return 1;
    }
    return 0;
  }
  else if(cmds[0]=="export-zone-key") {
    if(cmds.size() < 3) {
      cerr<<"Syntax: pdnsutil export-zone-key ZONE KEY-ID"<<endl;
      return 0;
    }

    string zone=cmds[1];
    unsigned int id=pdns_stou(cmds[2]);
    DNSSECPrivateKey dpk=dk.getKeyById(DNSName(zone), id);
    cout << dpk.getKey()->convertToISC() <<endl;
  }  
  else if(cmds[0]=="increase-serial") {
    if (cmds.size() < 2) {
      cerr<<"Syntax: pdnsutil increase-serial ZONE"<<endl;
      return 0;
    }
    return increaseSerial(DNSName(cmds[1]), dk);
  }
  else if(cmds[0]=="import-zone-key-pem") {
    if(cmds.size() < 4) {
      cerr<<"Syntax: pdnsutil import-zone-key-pem ZONE FILE ALGORITHM {ksk|zsk}"<<endl;
      exit(1);
    }
    string zone=cmds[1];
    string fname=cmds[2];
    string line;
    ifstream ifs(fname.c_str());
    string tmp, interim, raw;
    while(getline(ifs, line)) {
      if(line[0]=='-')
        continue;
      trim(line);
      interim += line;
    }
    B64Decode(interim, raw);
    DNSSECPrivateKey dpk;
    DNSKEYRecordContent drc;
    shared_ptr<DNSCryptoKeyEngine> key(DNSCryptoKeyEngine::makeFromPEMString(drc, raw));
    dpk.setKey(key);
    
    dpk.d_algorithm = pdns_stou(cmds[3]);
    
    if(dpk.d_algorithm == 7)
      dpk.d_algorithm = 5;
      
    cerr<<(int)dpk.d_algorithm<<endl;
    
    if(cmds.size() > 4) {
      if(pdns_iequals(cmds[4], "ZSK"))
        dpk.d_flags = 256;
      else if(pdns_iequals(cmds[4], "KSK"))
        dpk.d_flags = 257;
      else {
        cerr<<"Unknown key flag '"<<cmds[4]<<"'"<<endl;
        exit(1);
      }
    }
    else
      dpk.d_flags = 257; // ksk
      
    if(!dk.addKey(DNSName(zone), dpk)) {
      cerr<<"Adding key failed, perhaps DNSSEC not enabled in configuration?"<<endl;
      exit(1);
    }
    
  }
  else if(cmds[0]=="import-zone-key") {
    if(cmds.size() < 3) {
      cerr<<"Syntax: pdnsutil import-zone-key ZONE FILE [ksk|zsk] [active|inactive]"<<endl;
      exit(1);
    }
    string zone=cmds[1];
    string fname=cmds[2];
    DNSSECPrivateKey dpk;
    DNSKEYRecordContent drc;
    shared_ptr<DNSCryptoKeyEngine> key(DNSCryptoKeyEngine::makeFromISCFile(drc, fname.c_str()));
    dpk.setKey(key);
    dpk.d_algorithm = drc.d_algorithm;
    
    if(dpk.d_algorithm == 7)
      dpk.d_algorithm = 5;
    
    dpk.d_flags = 257; 
    bool active=true;

    for(unsigned int n = 3; n < cmds.size(); ++n) {
      if(pdns_iequals(cmds[n], "ZSK"))
        dpk.d_flags = 256;
      else if(pdns_iequals(cmds[n], "KSK"))
        dpk.d_flags = 257;
      else if(pdns_iequals(cmds[n], "active"))
        active = 1;
      else if(pdns_iequals(cmds[n], "passive") || pdns_iequals(cmds[n], "inactive")) // passive eventually needs to be removed
        active = 0;
      else { 
        cerr<<"Unknown key flag '"<<cmds[n]<<"'"<<endl;
        exit(1);
      }          
    }
    if(!dk.addKey(DNSName(zone), dpk, active)) {
      cerr<<"Adding key failed, perhaps DNSSEC not enabled in configuration?"<<endl;
      exit(1);
    }
  }
  else if(cmds[0]=="export-zone-dnskey") {
    if(cmds.size() < 3) {
      cerr<<"Syntax: pdnsutil export-zone-dnskey ZONE KEY-ID"<<endl;
      exit(1);
    }

    DNSName zone(cmds[1]);
    unsigned int id=pdns_stou(cmds[2]);
    DNSSECPrivateKey dpk=dk.getKeyById(zone, id);
    cout << zone<<" IN DNSKEY "<<dpk.getDNSKEY().getZoneRepresentation() <<endl;
    if(dpk.d_flags == 257) {
      cout << zone << " IN DS "<<makeDSFromDNSKey(zone, dpk.getDNSKEY(), 1).getZoneRepresentation() << endl;
      cout << zone << " IN DS "<<makeDSFromDNSKey(zone, dpk.getDNSKEY(), 2).getZoneRepresentation() << endl;
    }
  }
  else if(cmds[0] == "generate-zone-key") {
    if(cmds.size() < 2 ) {
      cerr << "Syntax: pdnsutil generate-zone-key zsk|ksk [rsasha1|rsasha256|rsasha512|gost|ecdsa256|ecdsa384] [bits]"<<endl;
      return 0;
    }
    // need to get algorithm, bits & ksk or zsk from commandline
    bool keyOrZone=false;
    int tmp_algo=0;
    int bits=0;
    int algorithm=8;
    for(unsigned int n=1; n < cmds.size(); ++n) {
      if(pdns_iequals(cmds[n], "zsk"))
        keyOrZone = false;
      else if(pdns_iequals(cmds[n], "ksk"))
        keyOrZone = true;
      else if((tmp_algo = shorthand2algorithm(cmds[n]))>0) {
        algorithm = tmp_algo;
      } else if(pdns_stou(cmds[n]))
        bits = pdns_stou(cmds[n]);
      else {
        cerr<<"Unknown algorithm, key flag or size '"<<cmds[n]<<"'"<<endl;
        return 0;
      }
    }
    cerr<<"Generating a " << (keyOrZone ? "KSK" : "ZSK")<<" with algorithm = "<<algorithm<<endl;
    if(bits)
      cerr<<"Requesting specific key size of "<<bits<<" bits"<<endl;

    DNSSECPrivateKey dspk; 
    shared_ptr<DNSCryptoKeyEngine> dpk(DNSCryptoKeyEngine::make(algorithm)); // defaults to RSA for now, could be smart w/algorithm! XXX FIXME 
    if(!bits) {
      if(algorithm <= 10)
        bits = keyOrZone ? 2048 : 1024;
      else {
        if(algorithm == 12 || algorithm == 13 || algorithm == 250) // ECDSA, GOST, ED25519
          bits = 256;
        else if(algorithm == 14)
          bits = 384;
        else {
          throw runtime_error("Can't guess key size for algorithm "+std::to_string(algorithm));
        }
      }
    }
    dpk->create(bits); 
    dspk.setKey(dpk); 
    dspk.d_algorithm = algorithm; 
    dspk.d_flags = keyOrZone ? 257 : 256; 

    // print key to stdout 
    cout << "Flags: " << dspk.d_flags << endl << 
             dspk.getKey()->convertToISC() << endl; 
  } else if (cmds[0]=="generate-tsig-key") {
     if (cmds.size() < 3) {
        cerr << "Syntax: " << cmds[0] << " name (hmac-md5|hmac-sha1|hmac-sha224|hmac-sha256|hmac-sha384|hmac-sha512)" << endl;
        return 0;
     }
     DNSName name(cmds[1]);
     string algo = cmds[2];
     string key;
     char tmpkey[64];

     size_t klen = 0;
     if (algo == "hmac-md5") {
       klen = 32;
     } else if (algo == "hmac-sha1") {
       klen = 32;
     } else if (algo == "hmac-sha224") {
       klen = 32;
     } else if (algo == "hmac-sha256") {
       klen = 64;
     } else if (algo == "hmac-sha384") {
       klen = 64;
     } else if (algo == "hmac-sha512") {
       klen = 64;
     } else {
       cerr << "Cannot generate key for " << algo << endl;
       return 1;
     }

     cerr << "Generating new key with " << klen << " bytes (this can take a while)" << endl;
     seedRandom(::arg()["entropy-source"]);
     for(size_t i = 0; i < klen; i+=4) {
        *(unsigned int*)(tmpkey+i) = dns_random(0xffffffff);
     }
     key = Base64Encode(std::string(tmpkey, klen));

     UeberBackend B("default");
     if (B.setTSIGKey(name, DNSName(algo), key)) { // you are feeling bored, put up DNSName(algo) up earlier
       cout << "Create new TSIG key " << name << " " << algo << " " << key << endl;
     } else {
       cout << "Failure storing new TSIG key " << name << " " << algo << " " << key << endl;
       return 1;
     }
     return 0;
  } else if (cmds[0]=="import-tsig-key") {
     if (cmds.size() < 4) {
        cerr << "Syntax: " << cmds[0] << " name algorithm key" << endl;
        return 0;
     }
     DNSName name(cmds[1]);
     string algo = cmds[2];
     string key = cmds[3];

     UeberBackend B("default");
     if (B.setTSIGKey(name, DNSName(algo), key)) {
       cout << "Imported TSIG key " << name << " " << algo << endl;
     } else {
       cout << "Failure importing TSIG key " << name << " " << algo << endl;
       return 1;
     }
     return 0;
  } else if (cmds[0]=="delete-tsig-key") {
     if (cmds.size() < 2) {
        cerr << "Syntax: " << cmds[0] << " name" << endl;
        return 0;
     }
     DNSName name(cmds[1]);

     UeberBackend B("default");
     if (B.deleteTSIGKey(name)) {
       cout << "Deleted TSIG key " << name << endl;
     } else {
       cout << "Failure deleting TSIG key " << name << endl;
       return 1;
     }
     return 0;
  } else if (cmds[0]=="list-tsig-keys") {
     std::vector<struct TSIGKey> keys;
     UeberBackend B("default");
     if (B.getTSIGKeys(keys)) {
        for(const struct TSIGKey &key :  keys) {
           cout << key.name.toString() << " " << key.algorithm.toString() << " " << key.key << endl;
        }
     }
     return 0;
  } else if (cmds[0]=="activate-tsig-key") {
     string metaKey;
     if (cmds.size() < 4) {
        cerr << "Syntax: " << cmds[0] << " ZONE NAME {master|slave}" << endl;
        return 0;
     }
     DNSName zname(cmds[1]);
     string name = cmds[2];
     if (cmds[3] == "master")
        metaKey = "TSIG-ALLOW-AXFR";
     else if (cmds[3] == "slave")
        metaKey = "AXFR-MASTER-TSIG";
     else {
        cerr << "Invalid parameter '" << cmds[3] << "', expected master or slave" << endl;
        return 1;
     }
     UeberBackend B("default");
     std::vector<std::string> meta; 
     if (!B.getDomainMetadata(zname, metaKey, meta)) {
       cout << "Failure enabling TSIG key " << name << " for " << zname << endl;
       return 1;
     }
     bool found = false;
     for(std::string tmpname :  meta) {
          if (tmpname == name) { found = true; break; }
     }
     if (!found) meta.push_back(name);
     if (B.setDomainMetadata(zname, metaKey, meta)) {
       cout << "Enabled TSIG key " << name << " for " << zname << endl;
     } else {
       cout << "Failure enabling TSIG key " << name << " for " << zname << endl;
       return 1;
     }
     return 0;
  } else if (cmds[0]=="deactivate-tsig-key") {
     string metaKey;
     if (cmds.size() < 4) {
        cerr << "Syntax: " << cmds[0] << " ZONE NAME {master|slave}" << endl;
        return 0;
     }
     DNSName zname(cmds[1]);
     string name = cmds[2];
     if (cmds[3] == "master")
        metaKey = "TSIG-ALLOW-AXFR";
     else if (cmds[3] == "slave")
        metaKey = "AXFR-MASTER-TSIG";
     else {
        cerr << "Invalid parameter '" << cmds[3] << "', expected master or slave" << endl;
        return 1;
     }

     UeberBackend B("default");
     std::vector<std::string> meta;
     if (!B.getDomainMetadata(zname, metaKey, meta)) {
       cout << "Failure disabling TSIG key " << name << " for " << zname << endl;
       return 1;
     }
     std::vector<std::string>::iterator iter = meta.begin();
     for(;iter != meta.end(); iter++) if (*iter == name) break;
     if (iter != meta.end()) meta.erase(iter);
     if (B.setDomainMetadata(zname, metaKey, meta)) {
       cout << "Disabled TSIG key " << name << " for " << zname << endl;
     } else {
       cout << "Failure disabling TSIG key " << name << " for " << zname << endl;
       return 1;
     }
     return 0;
  } else if (cmds[0]=="get-meta") {
    UeberBackend B("default");
    if (cmds.size() < 2) {
       cerr << "Syntax: " << cmds[0] << " zone [kind kind ..]" << endl;
       return 1;
    }
    DNSName zone(cmds[1]);
    vector<string> keys;
    DomainInfo di;

    if (!B.getDomainInfo(zone, di)) {
       cerr << "Invalid zone '" << zone << "'" << endl;
       return 1;
    }

    if (cmds.size() > 2) {
      keys.assign(cmds.begin() + 2, cmds.end());
      std::cout << "Metadata for '" << zone << "'" << endl;
      for(const string kind :  keys) {
        vector<string> meta;
        meta.clear();
        if (B.getDomainMetadata(zone, kind, meta)) {
          cout << kind << " = " << boost::join(meta, ", ") << endl;
        }
      }
    } else {
      std::map<std::string, std::vector<std::string> > meta;
      std::cout << "Metadata for '" << zone << "'" << endl;
      B.getAllDomainMetadata(zone, meta);
      for(std::map<std::string, std::vector<std::string> >::const_iterator each_meta = meta.begin(); each_meta != meta.end(); each_meta++) {
        cout << each_meta->first << " = " << boost::join(each_meta->second, ", ") << endl;
      }
    }  
    return 0;

  } else if (cmds[0]=="set-meta") {
    UeberBackend B("default");
    if (cmds.size() < 3) {
       cerr << "Syntax: " << cmds[0] << " zone kind [value value ..]" << endl;
       return 1;
    }
    DNSName zone(cmds[1]);
    string kind = cmds[2];
    vector<string> meta(cmds.begin() + 3, cmds.end());

    if (!B.setDomainMetadata(zone, kind, meta)) {
      cerr << "Unable to set meta for '" << zone << "'" << endl;
      return 1;
    } else {
      cout << "Set '" << zone.toStringNoDot() << "' meta " << kind << " = " << boost::join(meta, ", ") << endl;
    }
  } else if (cmds[0]=="hsm") {
#ifdef HAVE_P11KIT1
    UeberBackend B("default");
    if (cmds[1] == "assign") {
      DNSCryptoKeyEngine::storvector_t storvect;
      DomainInfo di;
      std::vector<DNSBackend::KeyData> keys;

      if (cmds.size() < 9) {
        std::cout << "Usage: pdnsutil hsm assign ZONE ALGORITHM {ksk|zsk} MODULE TOKEN PIN LABEL" << std::endl;
        return 1;
      }

      DNSName zone(cmds[2]);

      // verify zone
      if (!B.getDomainInfo(zone, di)) {
        cerr << "Unable to assign module to unknown zone '" << zone << "'" << std::endl;
        return 1;
      }

      int algorithm = shorthand2algorithm(cmds[3]);
      if (algorithm<0) {
        cerr << "Unable to use unknown algorithm '" << cmds[3] << "'" << std::endl;
        return 1;
      }

      int id;
      bool keyOrZone = (cmds[4] == "ksk" ? true : false);
      string module = cmds[5];
      string slot = cmds[6];
      string pin = cmds[7];
      string label = cmds[8];

      std::ostringstream iscString;
      iscString << "Private-key-format: v1.2" << std::endl << 
        "Algorithm: " << algorithm << std::endl << 
        "Engine: " << module << std::endl <<
        "Slot: " << slot << std::endl <<
        "PIN: " << pin << std::endl << 
        "Label: " << label << std::endl;

     DNSKEYRecordContent drc; 
     DNSSECPrivateKey dpk;
     dpk.d_flags = (keyOrZone ? 257 : 256);
     dpk.setKey(shared_ptr<DNSCryptoKeyEngine>(DNSCryptoKeyEngine::makeFromISCString(drc, iscString.str())));
 
     // make sure this key isn't being reused.
     B.getDomainKeys(zone, 0, keys);
     id = -1;

     for(DNSBackend::KeyData& kd :  keys) {
       if (kd.content == iscString.str()) {
         // it's this one, I guess...
         id = kd.id;
         break;
       }
     }

     if (id > -1) {
       cerr << "You have already assigned this key with ID=" << id << std::endl;
       return 1;
     }

     if (!(id = dk.addKey(zone, dpk))) {
       cerr << "Unable to assign module slot to zone" << std::endl;
       return 1;
     }

     // figure out key id.

     B.getDomainKeys(zone, 0, keys);

     // validate which one got the key...
     for(DNSBackend::KeyData& kd :  keys) {
       if (kd.content == iscString.str()) {
         // it's this one, I guess...
         id = kd.id;
         break;
       }
     }

     cerr << "Module " << module << " slot " << slot << " assigned to " << zone << " with key id " << id << endl;

     return 0;
    } else if (cmds[1] == "create-key") {

      if (cmds.size() < 4) {
        cerr << "Usage: pdnsutil hsm create-key ZONE KEY-ID [BITS]" << endl;
        return 1;
      }
      DomainInfo di;
      DNSName zone(cmds[2]);
      unsigned int id;
      int bits = 2048;
      // verify zone
      if (!B.getDomainInfo(zone, di)) {
        cerr << "Unable to create key for unknown zone '" << zone << "'" << std::endl;
        return 1;
      }
 
      id = pdns_stou(cmds[3]);
      std::vector<DNSBackend::KeyData> keys; 
      if (!B.getDomainKeys(zone, 0, keys)) {
        cerr << "No keys found for zone " << zone << std::endl;
        return 1;
      } 

      DNSCryptoKeyEngine *dke = NULL;
      // lookup correct key      
      for(DNSBackend::KeyData &kd :  keys) {
        if (kd.id == id) {
          // found our key. 
          DNSKEYRecordContent dkrc;
          dke = DNSCryptoKeyEngine::makeFromISCString(dkrc, kd.content);
        }
      }

      if (!dke) {
        cerr << "Could not find key with ID " << id << endl;
        return 1;
      }
      if (cmds.size() > 4) {
        bits = pdns_stou(cmds[4]);
      }
      if (bits < 1) {
        cerr << "Invalid bit size " << bits << "given, must be positive integer";
        return 1;
      }
      try {
        dke->create(bits);
      } catch (PDNSException& e) {
         cerr << e.reason << endl;
         return 1;
      }

      cerr << "Key of size " << bits << " created" << std::endl;
      return 0;
    }
#else
    cerr<<"PKCS#11 support not enabled"<<endl;
    return 1; 
#endif
  } else if (cmds[0] == "b2b-migrate") {
    if (cmds.size() < 3) {
      cerr<<"Usage: b2b-migrate OLD NEW"<<endl;
      return 1;
    }

    DNSBackend *src,*tgt;
    src = tgt = NULL;

    for(DNSBackend *b : BackendMakers().all()) {
      if (b->getPrefix() == cmds[1]) src = b;
      if (b->getPrefix() == cmds[2]) tgt = b;
    }
    if (!src) {
      cerr<<"Unknown source backend '"<<cmds[1]<<"'"<<endl;
      return 1;
    }
    if (!tgt) {
      cerr<<"Unknown target backend '"<<cmds[2]<<"'"<<endl;
      return 1;
    }

    cout<<"Moving zone(s) from "<<src->getPrefix()<<" to "<<tgt->getPrefix()<<endl;

    vector<DomainInfo> domains;

    tgt->getAllDomains(&domains, true);
    if (domains.size()>0)
      throw PDNSException("Target backend has domain(s), please clean it first");

    src->getAllDomains(&domains, true);
    // iterate zones
    for(const DomainInfo& di: domains) {
      size_t nr,nc,nm,nk;
      DNSResourceRecord rr;
      cout<<"Processing '"<<di.zone.toString()<<"'"<<endl;
      // create zone
      if (!tgt->createDomain(di.zone)) throw PDNSException("Failed to create zone");
      tgt->setKind(di.zone, di.kind);
      tgt->setAccount(di.zone,di.account);
      for(const string& master: di.masters) {
        tgt->setMaster(di.zone, master);
      }
      // move records
      if (!src->list(di.zone, di.id, true)) throw PDNSException("Failed to list records");
      nr=0;
      while(src->get(rr)) {
        if (!tgt->feedRecord(rr)) throw PDNSException("Failed to feed record");
        nr++;
      }
      // move comments
      nc=0;
      if (src->listComments(di.id)) {
        Comment c;
        while(src->getComment(c)) {
          tgt->feedComment(c);
          nc++;
        }
      }
      // move metadata
      nm=0;
      std::map<std::string, std::vector<std::string> > meta;
      if (src->getAllDomainMetadata(di.zone, meta)) {
        std::map<std::string, std::vector<std::string> >::iterator i;
        for(i=meta.begin(); i != meta.end(); i++) {
          if (!tgt->setDomainMetadata(di.zone, i->first, i->second)) throw PDNSException("Failed to feed domain metadata");
          nm++;
        }
      }
      // move keys
      nk=0;
      std::vector<DNSBackend::KeyData> keys;
      if (src->getDomainKeys(di.zone, 0, keys)) {
        for(const DNSBackend::KeyData& k: keys) {
          tgt->addDomainKey(di.zone, k);
          nk++;
        }
      }
      cout<<"Moved "<<nr<<" record(s), "<<nc<<" comment(s), "<<nm<<" metadata(s) and "<<nk<<" cryptokey(s)"<<endl;
    }

    int ntk=0;
    // move tsig keys
    std::vector<struct TSIGKey> tkeys;
    if (src->getTSIGKeys(tkeys)) {
      for(const struct TSIGKey& tk: tkeys) {
        if (!tgt->setTSIGKey(tk.name, tk.algorithm, tk.key)) throw PDNSException("Failed to feed TSIG key");
        ntk++;
      }
    }
    cout<<"Moved "<<ntk<<" TSIG key(s)"<<endl;

    cout<<"Remember to drop the old backend and run rectify-all-zones"<<endl;

    return 0;
  } else if (cmds[0] == "backend-cmd") {
    if (cmds.size() < 3) {
      cerr<<"Usage: backend-cmd BACKEND CMD [CMD..]"<<endl;
      return 1;
    }

    DNSBackend *db;
    db = NULL;

    for(DNSBackend *b : BackendMakers().all()) {
      if (b->getPrefix() == cmds[1]) db = b;
    }

    if (!db) {
      cerr<<"Unknown backend '"<<cmds[1]<<"'"<<endl;
      return 1;
    }

    for(auto i=next(begin(cmds),2); i != end(cmds); ++i) {
      cerr<<"== "<<*i<<endl;
      cout<<db->directBackendCmd(*i);
    }

    return 0;
  } else {
    cerr<<"Unknown command '"<<cmds[0] <<"'"<< endl;
    return 1;
  }
  return 0;
}
catch(PDNSException& ae) {
  cerr<<"Error: "<<ae.reason<<endl;
  return 1;
}
catch(std::exception& e) {
  cerr<<"Error: "<<e.what()<<endl;
  return 1;
}
catch(...)
{
  cerr<<"Caught an unknown exception"<<endl;
  return 1;
}
