#include "dnsdist.hh"
#include "dnsrulactions.hh"
#include <thread>
#include "dolog.hh"
#include "sodcrypto.hh"
#include "base64.hh"
#include <fstream>
#include "dnswriter.hh"
#include "lock.hh"

using std::thread;

static vector<std::function<void(void)>>* g_launchWork;

class LuaAction : public DNSAction
{
public:
  typedef std::function<std::tuple<int, string>(const ComboAddress& remote, const DNSName& qname, uint16_t qtype, dnsheader* dh, int len)> func_t;
  LuaAction(LuaAction::func_t func) : d_func(func)
  {}

  Action operator()(const ComboAddress& remote, const DNSName& qname, uint16_t qtype, dnsheader* dh, uint16_t& len, string* ruleresult) const override
  {
    auto ret = d_func(remote, qname, qtype, dh, len);
    if(ruleresult)
      *ruleresult=std::get<1>(ret);
    return (Action)std::get<0>(ret);
  }

  string toString() const 
  {
    return "Lua script";
  }

private:
  func_t d_func;
};

typedef boost::variant<string,vector<pair<int, string>>, std::shared_ptr<DNSRule> > luadnsrule_t;
std::shared_ptr<DNSRule> makeRule(const luadnsrule_t& var)
{
  if(auto src = boost::get<std::shared_ptr<DNSRule>>(&var))
    return *src;
  
  SuffixMatchNode smn;
  NetmaskGroup nmg;

  auto add=[&](string src) {
    try {
      nmg.addMask(src); // need to try mask first, all masks are domain names!
    } catch(...) {
      smn.add(DNSName(src));
    }
  };
  if(auto src = boost::get<string>(&var))
    add(*src);
  else {
    for(auto& a : boost::get<vector<pair<int, string>>>(var)) {
      add(a.second);
    }
  }
  if(nmg.empty())
    return std::make_shared<SuffixMatchNodeRule>(smn);
  else
    return std::make_shared<NetmaskGroupRule>(nmg);
}

vector<std::function<void(void)>> setupLua(bool client, const std::string& config)
{
  g_launchWork= new vector<std::function<void(void)>>();
  typedef std::unordered_map<std::string, boost::variant<bool, std::string, vector<pair<int, std::string> > > > newserver_t;

  g_lua.writeVariable("DNSAction", std::unordered_map<string,int>{
      {"Drop", (int)DNSAction::Action::Drop}, 
      {"Nxdomain", (int)DNSAction::Action::Nxdomain}, 
      {"Spoof", (int)DNSAction::Action::Spoof}, 
      {"Allow", (int)DNSAction::Action::Allow}, 
      {"HeaderModify", (int)DNSAction::Action::HeaderModify},
      {"Pool", (int)DNSAction::Action::Pool}, 
      {"None",(int)DNSAction::Action::None},
      {"Delay", (int)DNSAction::Action::Delay}}
    );
  
  g_lua.writeFunction("newServer", 
		      [client](boost::variant<string,newserver_t> pvars, boost::optional<int> qps)
		      { 
                        setLuaSideEffect();
			if(client) {
			  return std::make_shared<DownstreamState>(ComboAddress());
			}
			if(auto address = boost::get<string>(&pvars)) {
			  std::shared_ptr<DownstreamState> ret;
			  try {
			    ret=std::make_shared<DownstreamState>(ComboAddress(*address, 53));
			  }
			  catch(std::exception& e) {
			    g_outputBuffer="Error creating new server: "+string(e.what());
			    errlog("Error creating new server with address %s: %s", *address, e.what());
			    return ret;
			  }

			  if(qps) {
			    ret->qps=QPSLimiter(*qps, *qps);
			  }
			  g_dstates.modify([ret](servers_t& servers) { 
			      servers.push_back(ret); 
			      std::stable_sort(servers.begin(), servers.end(), [](const decltype(ret)& a, const decltype(ret)& b) {
				  return a->order < b->order;
				});

			    });

			  if(g_launchWork) {
			    g_launchWork->push_back([ret]() {
				ret->tid = move(thread(responderThread, ret));
			      });
			  }
			  else {
			    ret->tid = move(thread(responderThread, ret));
			  }

			  return ret;
			}
			auto vars=boost::get<newserver_t>(pvars);
			std::shared_ptr<DownstreamState> ret;
			try {
			  ret=std::make_shared<DownstreamState>(ComboAddress(boost::get<string>(vars["address"]), 53));
			}
			catch(std::exception& e) {
			  g_outputBuffer="Error creating new server: "+string(e.what());
			  errlog("Error creating new server with address %s: %s", boost::get<string>(vars["address"]), e.what());
			  return ret;
			}
			
			if(vars.count("qps")) {
			  int qps=std::stoi(boost::get<string>(vars["qps"]));
			  ret->qps=QPSLimiter(qps, qps);
			}

			if(vars.count("pool")) {
			  if(auto* pool = boost::get<string>(&vars["pool"]))
			    ret->pools.insert(*pool);
			  else {
			    auto* pools = boost::get<vector<pair<int, string> > >(&vars["pool"]);
			    for(auto& p : *pools)
			      ret->pools.insert(p.second);
			  }
			}

			if(vars.count("order")) {
			  ret->order=std::stoi(boost::get<string>(vars["order"]));
			}

			if(vars.count("weight")) {
			  ret->weight=std::stoi(boost::get<string>(vars["weight"]));
			}

			if(vars.count("retries")) {
			  ret->retries=std::stoi(boost::get<string>(vars["retries"]));
			}

			if(vars.count("tcpSendTimeout")) {
			  ret->tcpSendTimeout=std::stoi(boost::get<string>(vars["tcpSendTimeout"]));
			}

			if(vars.count("tcpRecvTimeout")) {
			  ret->tcpRecvTimeout=std::stoi(boost::get<string>(vars["tcpRecvTimeout"]));
			}

			if(vars.count("name")) {
			  ret->name=boost::get<string>(vars["name"]);
			}

			if(vars.count("checkName")) {
			  ret->checkName=DNSName(boost::get<string>(vars["checkName"]));
			}

			if(vars.count("checkType")) {
			  ret->checkType=boost::get<string>(vars["checkType"]);
			}

			if(vars.count("mustResolve")) {
			  ret->mustResolve=boost::get<bool>(vars["mustResolve"]);
			}

			if(vars.count("useClientSubnet")) {
			  ret->useECS=boost::get<bool>(vars["useClientSubnet"]);
			}

			if(g_launchWork) {
			  g_launchWork->push_back([ret]() {
			      ret->tid = move(thread(responderThread, ret));
			    });
			}
			else {
			  ret->tid = move(thread(responderThread, ret));
			}

			auto states = g_dstates.getCopy();
			states.push_back(ret);
			std::stable_sort(states.begin(), states.end(), [](const decltype(ret)& a, const decltype(ret)& b) {
			    return a->order < b->order;
			  });
			g_dstates.setState(states);
			return ret;
		      } );


  g_lua.writeFunction("addAnyTCRule", []() {
      setLuaSideEffect();
      auto rules=g_rulactions.getCopy();
      rules.push_back({ std::make_shared<QTypeRule>(0xff), std::make_shared<TCAction>()});
      g_rulactions.setState(rules);
    });

  g_lua.writeFunction("rmRule", [](unsigned int num) {
      setLuaSideEffect();
      auto rules = g_rulactions.getCopy();
      if(num >= rules.size()) {
	g_outputBuffer = "Error: attempt to delete non-existing rule\n";
	return;
      }
      rules.erase(rules.begin()+num);
      g_rulactions.setState(rules);
    });

  g_lua.writeFunction("topRule", []() {
      setLuaSideEffect();
      auto rules = g_rulactions.getCopy();
      if(rules.empty())
	return;
      auto subject = *rules.rbegin();
      rules.erase(std::prev(rules.end()));
      rules.insert(rules.begin(), subject);
      g_rulactions.setState(rules);
    });
  g_lua.writeFunction("mvRule", [](unsigned int from, unsigned int to) {
      setLuaSideEffect();
      auto rules = g_rulactions.getCopy();
      if(from >= rules.size() || to > rules.size()) {
	g_outputBuffer = "Error: attempt to move rules from/to invalid index\n";
	return;
      }

      auto subject = rules[from];
      rules.erase(rules.begin()+from);
      if(to == rules.size())
	rules.push_back(subject);
      else {
	if(from < to)
	  --to;
	rules.insert(rules.begin()+to, subject);
      }
      g_rulactions.setState(rules);
    });


  g_lua.writeFunction("rmServer", 
		      [](boost::variant<std::shared_ptr<DownstreamState>, int> var)
		      { 
                        setLuaSideEffect();
			auto states = g_dstates.getCopy();
			if(auto* rem = boost::get<shared_ptr<DownstreamState>>(&var))
			  states.erase(remove(states.begin(), states.end(), *rem), states.end());
			else
			  states.erase(states.begin() + boost::get<int>(var));
			g_dstates.setState(states);
		      } );


  g_lua.writeFunction("setServerPolicy", [](ServerPolicy policy)  {
      setLuaSideEffect();
      g_policy.setState(policy);
    });
  g_lua.writeFunction("setServerPolicyLua", [](string name, policy_t policy)  {
      setLuaSideEffect();
      g_policy.setState(ServerPolicy{name, policy});
    });

  g_lua.writeFunction("showServerPolicy", []() {
      setLuaSideEffect();
      g_outputBuffer=g_policy.getLocal()->name+"\n";
    });

  g_lua.writeFunction("truncateTC", [](bool tc) { setLuaSideEffect(); g_truncateTC=tc; });
  g_lua.writeFunction("fixupCase", [](bool fu) { setLuaSideEffect(); g_fixupCase=fu; });

  g_lua.registerMember("name", &ServerPolicy::name);
  g_lua.registerMember("policy", &ServerPolicy::policy);
  g_lua.writeFunction("newServerPolicy", [](string name, policy_t policy) { return ServerPolicy{name, policy};});
  g_lua.writeVariable("firstAvailable", ServerPolicy{"firstAvailable", firstAvailable});
  g_lua.writeVariable("roundrobin", ServerPolicy{"roundrobin", roundrobin});
  g_lua.writeVariable("wrandom", ServerPolicy{"wrandom", wrandom});
  g_lua.writeVariable("whashed", ServerPolicy{"whashed", whashed});
  g_lua.writeVariable("leastOutstanding", ServerPolicy{"leastOutstanding", leastOutstanding});
  g_lua.writeFunction("addACL", [](const std::string& domain) {
      setLuaSideEffect();
      g_ACL.modify([domain](NetmaskGroup& nmg) { nmg.addMask(domain); });
    });

  g_lua.writeFunction("setLocal", [client](const std::string& addr, boost::optional<bool> doTCP) {
      setLuaSideEffect();
      if(client)
	return;
      if (g_configurationDone) {
        g_outputBuffer="setLocal cannot be used at runtime!\n";
        return;
      }
      try {
	ComboAddress loc(addr, 53);
	g_locals.clear();
	g_locals.push_back({loc, doTCP ? *doTCP : true}); /// only works pre-startup, so no sync necessary
      }
      catch(std::exception& e) {
	g_outputBuffer="Error: "+string(e.what())+"\n";
      }
    });

  g_lua.writeFunction("addLocal", [client](const std::string& addr, boost::optional<bool> doTCP) {
      setLuaSideEffect();
      if(client)
	return;
      if (g_configurationDone) {
        g_outputBuffer="addLocal cannot be used at runtime!\n";
        return;
      }
      try {
	ComboAddress loc(addr, 53);
	g_locals.push_back({loc, doTCP ? *doTCP : true}); /// only works pre-startup, so no sync necessary
      }
      catch(std::exception& e) {
	g_outputBuffer="Error: "+string(e.what())+"\n";
      }
    });
  g_lua.writeFunction("setACL", [](boost::variant<string,vector<pair<int, string>>> inp) {
      setLuaSideEffect();
      NetmaskGroup nmg;
      if(auto str = boost::get<string>(&inp)) {
	nmg.addMask(*str);
      }
      else for(const auto& p : boost::get<vector<pair<int,string>>>(inp)) {
	nmg.addMask(p.second);
      }
      g_ACL.setState(nmg);
  });
  g_lua.writeFunction("showACL", []() {
      setLuaNoSideEffect();
      vector<string> vec;

      g_ACL.getCopy().toStringVector(&vec);

      for(const auto& s : vec)
        g_outputBuffer+=s+"\n";

    });
  g_lua.writeFunction("shutdown", []() { _exit(0);} );


  g_lua.writeFunction("addDomainBlock", [](const std::string& domain) { 
      setLuaSideEffect();
      SuffixMatchNode smn;
      smn.add(DNSName(domain));
	g_rulactions.modify([smn](decltype(g_rulactions)::value_type& rulactions) {
	    rulactions.push_back({
				   std::make_shared<SuffixMatchNodeRule>(smn), 
				   std::make_shared<DropAction>()  });
	  });

    });
  g_lua.writeFunction("showServers", []() {  
      setLuaNoSideEffect();
      try {
      ostringstream ret;
      boost::format fmt("%1$-3d %2$-20.20s %|25t|%3% %|55t|%4$5s %|51t|%5$7.1f %|66t|%6$7d %|69t|%7$3d %|78t|%8$2d %|80t|%9$10d %|86t|%10$7d %|91t|%11$5.1f %|109t|%12$5.1f %13%" );
      //             1        2          3       4        5       6       7       8           9        10        11       12
      ret << (fmt % "#" % "Name" % "Address" % "State" % "Qps" % "Qlim" % "Ord" % "Wt" % "Queries" % "Drops" % "Drate" % "Lat" % "Pools") << endl;

      uint64_t totQPS{0}, totQueries{0}, totDrops{0};
      int counter=0;
      auto states = g_dstates.getCopy();
      for(const auto& s : states) {
	string status;
	if(s->availability == DownstreamState::Availability::Up) 
	  status = "UP";
	else if(s->availability == DownstreamState::Availability::Down) 
	  status = "DOWN";
	else 
	  status = (s->upStatus ? "up" : "down");

	string pools;
	for(auto& p : s->pools) {
	  if(!pools.empty())
	    pools+=" ";
	  pools+=p;
	}

	ret << (fmt % counter % s->name % s->remote.toStringWithPort() %
		status % 
		s->queryLoad % s->qps.getRate() % s->order % s->weight % s->queries.load() % s->reuseds.load() % (s->dropRate) % (s->latencyUsec/1000.0) % pools) << endl;

	totQPS += s->queryLoad;
	totQueries += s->queries.load();
	totDrops += s->reuseds.load();
	++counter;
      }
      ret<< (fmt % "All" % "" % "" % ""
		% 
	     (double)totQPS % "" % "" % "" % totQueries % totDrops % "" % "" % "" ) << endl;

      g_outputBuffer=ret.str();
      }catch(std::exception& e) { g_outputBuffer=e.what(); throw; }
    });

  g_lua.writeFunction("addLuaAction", [](luadnsrule_t var, LuaAction::func_t func) 
		      {
                        setLuaSideEffect();
			auto rule=makeRule(var);
			g_rulactions.modify([rule,func](decltype(g_rulactions)::value_type& rulactions){
			    rulactions.push_back({rule,
				  std::make_shared<LuaAction>(func)});
			  });
		      });


  g_lua.writeFunction("NoRecurseAction", []() {
      return std::shared_ptr<DNSAction>(new NoRecurseAction);
    });

  g_lua.writeFunction("PoolAction", [](const string& a) {
      return std::shared_ptr<DNSAction>(new PoolAction(a));
    });

  g_lua.writeFunction("SpoofAction", [](const string& a, boost::optional<string> b) {
      if(b) 
	return std::shared_ptr<DNSAction>(new SpoofAction(ComboAddress(a), ComboAddress(*b)));
      else 
	return std::shared_ptr<DNSAction>(new SpoofAction(ComboAddress(a)));
    });

  g_lua.writeFunction("addDomainSpoof", [](const std::string& domain, const std::string& ip, boost::optional<string> ip6) { 
      setLuaSideEffect();
      SuffixMatchNode smn;
      ComboAddress a, b;
      b.sin6.sin6_family=0;
      try
      {
	smn.add(DNSName(domain));
	a=ComboAddress(ip);
	if(ip6)
	  b=ComboAddress(*ip6);
      }
      catch(std::exception& e) {
	g_outputBuffer="Error parsing parameters: "+string(e.what());
	return;
      }
      g_rulactions.modify([&smn,&a,&b](decltype(g_rulactions)::value_type& rulactions) {
	  rulactions.push_back({
	      std::make_shared<SuffixMatchNodeRule>(smn), 
		std::make_shared<SpoofAction>(a, b)  });
	});

    });


  g_lua.writeFunction("DropAction", []() {
      return std::shared_ptr<DNSAction>(new DropAction);
    });

  g_lua.writeFunction("DelayAction", [](int msec) {
      return std::shared_ptr<DNSAction>(new DelayAction(msec));
    });


  g_lua.writeFunction("TCAction", []() {
      return std::shared_ptr<DNSAction>(new TCAction);
    });

  g_lua.writeFunction("DisableValidationAction", []() {
      return std::shared_ptr<DNSAction>(new DisableValidationAction);
    });

  g_lua.writeFunction("LogAction", [](const std::string& fname) {
      return std::shared_ptr<DNSAction>(new LogAction(fname));
    });


  g_lua.writeFunction("MaxQPSIPRule", [](unsigned int qps, boost::optional<int> ipv4trunc, boost::optional<int> ipv6trunc) {
      return std::shared_ptr<DNSRule>(new MaxQPSIPRule(qps, ipv4trunc.get_value_or(32), ipv6trunc.get_value_or(64)));
    });


  g_lua.writeFunction("MaxQPSRule", [](unsigned int qps, boost::optional<int> burst) {
      if(!burst)
        return std::shared_ptr<DNSRule>(new MaxQPSRule(qps));
      else
        return std::shared_ptr<DNSRule>(new MaxQPSRule(qps, *burst));      
    });


  g_lua.writeFunction("RegexRule", [](const std::string& str) {
      return std::shared_ptr<DNSRule>(new RegexRule(str));
    });


  g_lua.writeFunction("benchRule", [](std::shared_ptr<DNSRule> rule, boost::optional<int> times_, boost::optional<string> suffix_)  {
      setLuaNoSideEffect();
      int times = times_.get_value_or(100000);
      DNSName suffix(suffix_.get_value_or("powerdns.com"));
      struct item {
        vector<uint8_t> packet;        
        ComboAddress rem;
        DNSName qname;
        uint16_t qtype;
      };
      vector<item> items;
      items.reserve(1000);
      for(int n=0; n < 1000; ++n) {
        struct item i;
        i.qname=DNSName(std::to_string(random()));
        i.qname += suffix;
        i.qtype = random() % 0xff;
        i.rem=ComboAddress("127.0.0.1");
        i.rem.sin4.sin_addr.s_addr = random();
        DNSPacketWriter pw(i.packet, i.qname, i.qtype);
        items.push_back(i);
      }

      int matches=0;
      DTime dt;
      dt.set();
      for(int n=0; n < times; ++n) {
        const item& i = items[n % items.size()];
        struct dnsheader* dh = (struct dnsheader*)&i.packet[0];
        if(rule->matches(i.rem, i.qname, i.qtype, dh, i.packet.size()))
          matches++;
      }
      double udiff=dt.udiff();
      g_outputBuffer=(boost::format("Had %d matches out of %d, %.1f qps, in %.1f usec\n") % matches % times % (1000000*(1.0*times/udiff)) % udiff).str();

    });

  g_lua.writeFunction("AllRule", []() {
      return std::shared_ptr<DNSRule>(new AllRule());
    });

  g_lua.writeFunction("QTypeRule", [](boost::variant<int, std::string> str) {
      uint16_t qtype;
      if(auto dir = boost::get<int>(&str)) {
        qtype = *dir;
      }
      else {
        string val=boost::get<string>(str);
        qtype = QType::chartocode(val.c_str());
        if(!qtype)
          throw std::runtime_error("Unable to convert '"+val+"' to a DNS type");
      }
      return std::shared_ptr<DNSRule>(new QTypeRule(qtype));
    });

  g_lua.writeFunction("AndRule", [](vector<pair<int, std::shared_ptr<DNSRule> > >a) {
      return std::shared_ptr<DNSRule>(new AndRule(a));
    });


  g_lua.writeFunction("addAction", [](luadnsrule_t var, std::shared_ptr<DNSAction> ea) 
		      {
                        setLuaSideEffect();
			auto rule=makeRule(var);
			g_rulactions.modify([rule, ea](decltype(g_rulactions)::value_type& rulactions){
			    rulactions.push_back({rule, ea});
			  });
		      });


  g_lua.writeFunction("addPoolRule", [](luadnsrule_t var, string pool) {
      setLuaSideEffect();
      auto rule=makeRule(var);
	g_rulactions.modify([rule, pool](decltype(g_rulactions)::value_type& rulactions) {
	    rulactions.push_back({
		rule,
		  std::make_shared<PoolAction>(pool)  });
	  });
    });

  g_lua.writeFunction("addNoRecurseRule", [](luadnsrule_t var) {
      setLuaSideEffect();
      auto rule=makeRule(var);
	g_rulactions.modify([rule](decltype(g_rulactions)::value_type& rulactions) {
	    rulactions.push_back({
		rule,
		  std::make_shared<NoRecurseAction>()  });
	  });
    });

  g_lua.writeFunction("addDisableValidationRule", [](luadnsrule_t var) {
      setLuaSideEffect();
      auto rule=makeRule(var);
	g_rulactions.modify([rule](decltype(g_rulactions)::value_type& rulactions) {
	    rulactions.push_back({
		rule,
		  std::make_shared<DisableValidationAction>()  });
	  });
    });


  g_lua.writeFunction("addQPSPoolRule", [](luadnsrule_t var, int limit, string pool) {
      setLuaSideEffect();
      auto rule = makeRule(var);
      g_rulactions.modify([rule, pool,limit](decltype(g_rulactions)::value_type& rulactions) {
	  rulactions.push_back({
	      rule, 
		std::make_shared<QPSPoolAction>(limit, pool)  });
	});
    });

  g_lua.writeFunction("setDNSSECPool", [](const std::string& pool) {
      setLuaSideEffect();
      g_rulactions.modify([pool](decltype(g_rulactions)::value_type& rulactions) {
	  rulactions.push_back({std::make_shared<DNSSECRule>(), 
		std::make_shared<PoolAction>(pool)}); 
	});
    });

  g_lua.writeFunction("addQPSLimit", [](luadnsrule_t var, int lim) {
      setLuaSideEffect();
      auto rule = makeRule(var);
      g_rulactions.modify([lim,rule](decltype(g_rulactions)::value_type& rulactions) {
	  rulactions.push_back({rule, 
		std::make_shared<QPSAction>(lim)});
	});
    });
   
  g_lua.writeFunction("addDelay", [](luadnsrule_t var, int msec) {
      setLuaSideEffect();
      auto rule = makeRule(var);
      g_rulactions.modify([msec,rule](decltype(g_rulactions)::value_type& rulactions) {
	  rulactions.push_back({rule, 
		std::make_shared<DelayAction>(msec)});
	});
    });


  g_lua.writeFunction("showRules", []() {
     setLuaNoSideEffect();
     boost::format fmt("%-3d %9d %-50s %s\n");
     g_outputBuffer += (fmt % "#" % "Matches" % "Rule" % "Action").str();
     int num=0;
      for(const auto& lim : g_rulactions.getCopy()) {  
        string name = lim.first->toString();
	g_outputBuffer += (fmt % num % lim.first->d_matches % name % lim.second->toString()).str();
	++num;
      }
    });

  g_lua.writeFunction("getServers", []() {
      setLuaNoSideEffect();
      vector<pair<int, std::shared_ptr<DownstreamState> > > ret;
      int count=1;
      for(const auto& s : g_dstates.getCopy()) {
	ret.push_back(make_pair(count++, s));
      }
      return ret;
    });

  g_lua.writeFunction("getPoolServers", [](string pool) {
      return getDownstreamCandidates(g_dstates.getCopy(), pool);
    });

  g_lua.writeFunction("getServer", [client](int i) {
      if (client)
        return std::make_shared<DownstreamState>(ComboAddress());
      return g_dstates.getCopy().at(i);
    });

  g_lua.registerFunction<void(DownstreamState::*)(int)>("setQPS", [](DownstreamState& s, int lim) { s.qps = lim ? QPSLimiter(lim, lim) : QPSLimiter(); });
  g_lua.registerFunction<void(DownstreamState::*)(string)>("addPool", [](DownstreamState& s, string pool) { s.pools.insert(pool);});
  g_lua.registerFunction<void(DownstreamState::*)(string)>("rmPool", [](DownstreamState& s, string pool) { s.pools.erase(pool);});

  g_lua.registerFunction<void(DownstreamState::*)()>("getOutstanding", [](const DownstreamState& s) { g_outputBuffer=std::to_string(s.outstanding.load()); });


  g_lua.registerFunction("isUp", &DownstreamState::isUp);
  g_lua.registerFunction("setDown", &DownstreamState::setDown);
  g_lua.registerFunction("setUp", &DownstreamState::setUp);
  g_lua.registerFunction("setAuto", &DownstreamState::setAuto);
  g_lua.registerMember("upStatus", &DownstreamState::upStatus);
  g_lua.registerMember("weight", &DownstreamState::weight);
  g_lua.registerMember("order", &DownstreamState::order);
  
  g_lua.writeFunction("infolog", [](const string& arg) {
      infolog("%s", arg);
    });
  g_lua.writeFunction("errlog", [](const string& arg) {
      errlog("%s", arg);
    });
  g_lua.writeFunction("warnlog", [](const string& arg) {
      warnlog("%s", arg);
    });


  g_lua.writeFunction("show", [](const string& arg) {
      g_outputBuffer+=arg;
      g_outputBuffer+="\n";
    });

  g_lua.registerFunction<void(dnsheader::*)(bool)>("setRD", [](dnsheader& dh, bool v) {
      dh.rd=v;
    });

  g_lua.registerFunction<bool(dnsheader::*)()>("getRD", [](dnsheader& dh) {
      return (bool)dh.rd;
    });

  g_lua.registerFunction<void(dnsheader::*)(bool)>("setCD", [](dnsheader& dh, bool v) {
      dh.cd=v;
    });

  g_lua.registerFunction<bool(dnsheader::*)()>("getCD", [](dnsheader& dh) {
      return (bool)dh.cd;
    });


  g_lua.registerFunction<void(dnsheader::*)(bool)>("setTC", [](dnsheader& dh, bool v) {
      dh.tc=v;
      if(v) dh.ra = dh.rd; // you'll always need this, otherwise TC=1 gets ignored
    });

  g_lua.registerFunction<void(dnsheader::*)(bool)>("setQR", [](dnsheader& dh, bool v) {
      dh.qr=v;
    });


  g_lua.registerFunction("tostring", &ComboAddress::toString);

  g_lua.registerFunction("isPartOf", &DNSName::isPartOf);
  g_lua.registerFunction<string(DNSName::*)()>("tostring", [](const DNSName&dn ) { return dn.toString(); });
  g_lua.writeFunction("newDNSName", [](const std::string& name) { return DNSName(name); });
  g_lua.writeFunction("newSuffixMatchNode", []() { return SuffixMatchNode(); });

  g_lua.registerFunction("add",(void (SuffixMatchNode::*)(const DNSName&)) &SuffixMatchNode::add);
  g_lua.registerFunction("check",(bool (SuffixMatchNode::*)(const DNSName&) const) &SuffixMatchNode::check);

  g_lua.writeFunction("carbonServer", [](const std::string& address, boost::optional<string> ourName,
					 boost::optional<int> interval) {
                        setLuaSideEffect();
			auto ours = g_carbon.getCopy();
			ours.server=ComboAddress(address, 2003);
			if(ourName)
			  ours.ourname=*ourName;
			if(interval)
			  ours.interval=*interval;
			if(!ours.interval)
			  ours.interval=1;
			g_carbon.setState(ours);
		      });

  g_lua.writeFunction("webserver", [client](const std::string& address, const std::string& password) {
      setLuaSideEffect();
      if(client)
	return;
      ComboAddress local(address);
      try {
	int sock = socket(local.sin4.sin_family, SOCK_STREAM, 0);
	SSetsockopt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
	SBind(sock, local);
	SListen(sock, 5);
	auto launch=[sock, local, password]() {
	  thread t(dnsdistWebserverThread, sock, local, password);
	  t.detach();
	};
	if(g_launchWork) 
	  g_launchWork->push_back(launch);
	else
	  launch();	    
      }
      catch(std::exception& e) {
	errlog("Unable to bind to webserver socket on %s: %s", local.toStringWithPort(), e.what());
      }

    });
  g_lua.writeFunction("controlSocket", [client](const std::string& str) {
      setLuaSideEffect();
      ComboAddress local(str, 5199);

      if(client) {
	g_serverControl = local;
	return;
      }
      
      try {
	int sock = socket(local.sin4.sin_family, SOCK_STREAM, 0);
	SSetsockopt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
	SBind(sock, local);
	SListen(sock, 5);
	auto launch=[sock, local]() {
	    thread t(controlThread, sock, local);
	    t.detach();
	};
	if(g_launchWork) 
	  g_launchWork->push_back(launch);
	else
	  launch();
	    
      }
      catch(std::exception& e) {
	errlog("Unable to bind to control socket on %s: %s", local.toStringWithPort(), e.what());
      }
    });


  g_lua.writeFunction("topClients", [](boost::optional<unsigned int> top_) {
      setLuaNoSideEffect();
      auto top = top_.get_value_or(10);
      map<ComboAddress, int,ComboAddress::addressOnlyLessThan > counts;
      unsigned int total=0;
      {
        ReadLock rl(&g_rings.queryLock);
        for(const auto& c : g_rings.queryRing) {
          counts[c.requestor]++;
          total++;
        }
      }
      vector<pair<int, ComboAddress>> rcounts;
      rcounts.reserve(counts.size());
      for(const auto& c : counts) 
	rcounts.push_back(make_pair(c.second, c.first));

      sort(rcounts.begin(), rcounts.end(), [](const decltype(rcounts)::value_type& a, 
					      const decltype(rcounts)::value_type& b) {
	     return b.first < a.first;
	   });
      unsigned int count=1, rest=0;
      boost::format fmt("%4d  %-40s %4d %4.1f%%\n");
      for(const auto& rc : rcounts) {
	if(count==top+1)
	  rest+=rc.first;
	else
	  g_outputBuffer += (fmt % (count++) % rc.second.toString() % rc.first % (100.0*rc.first/total)).str();
      }
      g_outputBuffer += (fmt % (count) % "Rest" % rest % (100.0*rest/total)).str();
    });

  g_lua.writeFunction("getTopQueries", [](unsigned int top, boost::optional<int> labels) {
      setLuaNoSideEffect();
      map<DNSName, int> counts;
      unsigned int total=0;
      if(!labels) {
	ReadLock rl(&g_rings.queryLock);
	for(const auto& a : g_rings.queryRing) {
	  counts[a.name]++;
	  total++;
	}
      }
      else {
	unsigned int lab = *labels;
	ReadLock rl(&g_rings.queryLock);
	for(auto a : g_rings.queryRing) {
	  a.name.trimToLabels(lab);
	  counts[a.name]++;
	  total++;
	}
      }
      // cout<<"Looked at "<<total<<" queries, "<<counts.size()<<" different ones"<<endl;
      vector<pair<int, DNSName>> rcounts;
      rcounts.reserve(counts.size());
      for(const auto& c : counts) 
	rcounts.push_back(make_pair(c.second, c.first));

      sort(rcounts.begin(), rcounts.end(), [](const decltype(rcounts)::value_type& a, 
					      const decltype(rcounts)::value_type& b) {
	     return b.first < a.first;
	   });

      std::unordered_map<int, vector<boost::variant<string,double>>> ret;
      unsigned int count=1, rest=0;
      for(const auto& rc : rcounts) {
	if(count==top+1)
	  rest+=rc.first;
	else
	  ret.insert({count++, {rc.second.toString(), rc.first, 100.0*rc.first/total}});
      }
      ret.insert({count, {"Rest", rest, 100.0*rest/total}});
      return ret;

    });
  
  g_lua.executeCode(R"(function topQueries(top, labels) top = top or 10; for k,v in ipairs(getTopQueries(top,labels)) do show(string.format("%4d  %-40s %4d %4.1f%%",k,v[1],v[2], v[3])) end end)");



  g_lua.writeFunction("getResponseRing", []() {
      setLuaNoSideEffect();
      decltype(g_rings.respRing) ring;
      {
	std::lock_guard<std::mutex> lock(g_rings.respMutex);
	ring = g_rings.respRing;
      }
      vector<std::unordered_map<string, boost::variant<string, unsigned int> > > ret;
      ret.reserve(ring.size());
      decltype(ret)::value_type item;
      for(const auto& r : ring) {
	item["name"]=r.name.toString();
	item["qtype"]=r.qtype;
	item["rcode"]=r.dh.rcode;
	item["usec"]=r.usec;
	ret.push_back(item);
      }
      return ret;
    });

  g_lua.writeFunction("getTopResponses", [](unsigned int top, unsigned int kind, boost::optional<int> labels) {
      setLuaNoSideEffect();
      map<DNSName, int> counts;
      unsigned int total=0;
      {
	std::lock_guard<std::mutex> lock(g_rings.respMutex);
	if(!labels) {
	  for(const auto& a : g_rings.respRing) {
	    if(a.dh.rcode!=kind)
	      continue;
	    counts[a.name]++;
	    total++;
	  }
	}
	else {
	  unsigned int lab = *labels;
	  for(auto a : g_rings.respRing) {
	    if(a.dh.rcode!=kind)
	      continue;

	    a.name.trimToLabels(lab);
	    counts[a.name]++;
	    total++;
	  }
	  
	}
      }
      //      cout<<"Looked at "<<total<<" responses, "<<counts.size()<<" different ones"<<endl;
      vector<pair<int, DNSName>> rcounts;
      rcounts.reserve(counts.size());
      for(const auto& c : counts) 
	rcounts.push_back(make_pair(c.second, c.first));

      sort(rcounts.begin(), rcounts.end(), [](const decltype(rcounts)::value_type& a, 
					      const decltype(rcounts)::value_type& b) {
	     return b.first < a.first;
	   });

      std::unordered_map<int, vector<boost::variant<string,double>>> ret;
      unsigned int count=1, rest=0;
      for(const auto& rc : rcounts) {
	if(count==top+1)
	  rest+=rc.first;
	else
	  ret.insert({count++, {rc.second.toString(), rc.first, 100.0*rc.first/total}});
      }
      ret.insert({count, {"Rest", rest, 100.0*rest/total}});
      return ret;

    });

  g_lua.executeCode(R"(function topResponses(top, kind, labels) top = top or 10; kind = kind or 0; for k,v in ipairs(getTopResponses(top, kind, labels)) do show(string.format("%4d  %-40s %4d %4.1f%%",k,v[1],v[2],v[3])) end end)");


  g_lua.writeFunction("showResponseLatency", []() {
      setLuaNoSideEffect();
      map<double, unsigned int> histo;
      double bin=100;
      for(int i=0; i < 15; ++i) {
	histo[bin];
	bin*=2;
      }

      double totlat=0;
      int size=0;
      {
	std::lock_guard<std::mutex> lock(g_rings.respMutex);
	for(const auto& r : g_rings.respRing) {
	  ++size;
	  auto iter = histo.lower_bound(r.usec);
	  if(iter != histo.end())
	    iter->second++;
	  else
	    histo.rbegin()++;
	  totlat+=r.usec;
	}
      }

      if (size == 0) {
        g_outputBuffer = "No traffic yet.\n";
        return;
      }

      g_outputBuffer = (boost::format("Average response latency: %.02f msec\n") % (0.001*totlat/size)).str();
      double highest=0;
      
      for(auto iter = histo.cbegin(); iter != histo.cend(); ++iter) {
	highest=std::max(highest, iter->second*1.0);
      }
      boost::format fmt("%7.2f\t%s\n");
      g_outputBuffer += (fmt % "msec" % "").str();

      for(auto iter = histo.cbegin(); iter != histo.cend(); ++iter) {
	int stars = (70.0 * iter->second/highest);
	char c='*';
	if(!stars && iter->second) {
	  stars=1; // you get 1 . to show something is there..
	  if(70.0*iter->second/highest > 0.5)
	    c=':';
	  else
	    c='.';
	}
	g_outputBuffer += (fmt % (iter->first/1000.0) % string(stars, c)).str();
      }
    });

  g_lua.writeFunction("newQPSLimiter", [](int rate, int burst) { return QPSLimiter(rate, burst); });
  g_lua.registerFunction("check", &QPSLimiter::check);


  g_lua.writeFunction("makeKey", []() {
      setLuaNoSideEffect();
      g_outputBuffer="setKey("+newKey()+")\n";
    });
  
  g_lua.writeFunction("setKey", [](const std::string& key) {
      setLuaSideEffect();
      if(B64Decode(key, g_key) < 0) {
	  g_outputBuffer=string("Unable to decode ")+key+" as Base64";
	  errlog("%s", g_outputBuffer);
	}
    });

  
  g_lua.writeFunction("testCrypto", [](boost::optional<string> optTestMsg)
   {
     setLuaNoSideEffect();
#ifdef HAVE_LIBSODIUM
     try {
       string testmsg;

       if (optTestMsg) {
         testmsg = *optTestMsg;
       }
       else {
         testmsg = "testStringForCryptoTests";
       }

       SodiumNonce sn, sn2;
       sn.init();
       sn2=sn;
       string encrypted = sodEncryptSym(testmsg, g_key, sn);
       string decrypted = sodDecryptSym(encrypted, g_key, sn2);
       
       sn.increment();
       sn2.increment();

       encrypted = sodEncryptSym(testmsg, g_key, sn);
       decrypted = sodDecryptSym(encrypted, g_key, sn2);

       if(testmsg == decrypted)
	 g_outputBuffer="Everything is ok!\n";
       else
	 g_outputBuffer="Crypto failed..\n";
       
     }
     catch(...) {
       g_outputBuffer="Crypto failed..\n";
     }
#else
     g_outputBuffer="Crypto not available.\n";
#endif
   });

  g_lua.writeFunction("setTCPRecvTimeout", [](int timeout) { g_tcpRecvTimeout=timeout; });

  g_lua.writeFunction("setTCPSendTimeout", [](int timeout) { g_tcpSendTimeout=timeout; });

  g_lua.writeFunction("setMaxUDPOutstanding", [](uint16_t max) {
      if (!g_configurationDone) {
        g_maxOutstanding = max;
      } else {
        g_outputBuffer="Max UDP outstanding cannot be altered at runtime!\n";
      }
    });

  g_lua.writeFunction("setMaxTCPClientThreads", [](uint64_t max) { g_maxTCPClientThreads = max; });

  g_lua.writeFunction("setECSSourcePrefixV4", [](uint16_t prefix) { g_ECSSourcePrefixV4=prefix; });

  g_lua.writeFunction("setECSSourcePrefixV6", [](uint16_t prefix) { g_ECSSourcePrefixV6=prefix; });

  g_lua.writeFunction("setECSOverride", [](bool override) { g_ECSOverride=override; });

  g_lua.writeFunction("dumpStats", [] {
      setLuaNoSideEffect();
      vector<string> leftcolumn, rightcolumn;

      boost::format fmt("%-23s\t%+11s");
      g_outputBuffer.clear();
      auto entries = g_stats.entries;
      sort(entries.begin(), entries.end(), 
	   [](const decltype(entries)::value_type& a, const decltype(entries)::value_type& b) {
	     return a.first < b.first;
	   });
      boost::format flt("    %9.1f");
      for(const auto& e : entries) {
	string second;
	if(const auto& val = boost::get<DNSDistStats::stat_t*>(&e.second))
	  second=std::to_string((*val)->load());
	else if (const auto& val = boost::get<double*>(&e.second))
	  second=(flt % (**val)).str();
	else
	  second=std::to_string((*boost::get<DNSDistStats::statfunction_t>(&e.second))(e.first));

	if(leftcolumn.size() < g_stats.entries.size()/2)
	  leftcolumn.push_back((fmt % e.first % second).str());
	else
	  rightcolumn.push_back((fmt % e.first % second).str());
      }

      auto leftiter=leftcolumn.begin(), rightiter=rightcolumn.begin();
      boost::format clmn("%|0t|%1% %|39t|%2%\n");

      for(;leftiter != leftcolumn.end() || rightiter != rightcolumn.end();) {
	string lentry, rentry;
	if(leftiter!= leftcolumn.end()) {
	  lentry = *leftiter;
	  leftiter++;
	}
	if(rightiter!= rightcolumn.end()) {
	  rentry = *rightiter;
	  rightiter++;
	}
	g_outputBuffer += (clmn % lentry % rentry).str();
      }
    });

  moreLua();
  
  std::ifstream ifs(config);
  if(!ifs) 
    warnlog("Unable to read configuration from '%s'", config);
  else
    infolog("Read configuration from '%s'", config);

  g_lua.executeCode(ifs);
  auto ret=*g_launchWork;
  delete g_launchWork;
  g_launchWork=0;
  return ret;
}
