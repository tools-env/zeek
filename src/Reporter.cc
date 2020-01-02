//
// See the file "COPYING" in the main distribution directory for copyright.
//

#include <unistd.h>
#include <syslog.h>

#include "zeek-config.h"
#include "Reporter.h"
#include "Event.h"
#include "NetVar.h"
#include "Net.h"
#include "Conn.h"
#include "Timer.h"
#include "plugin/Plugin.h"
#include "plugin/Manager.h"
#include "file_analysis/File.h"

//#include "3rdparty/fmt/include/fmt/format.h"

#ifdef SYSLOG_INT
extern "C" {
int openlog(const char* ident, int logopt, int facility);
int syslog(int priority, const char* message_fmt, ...);
int closelog();
}
#endif

Reporter* reporter = 0;

Reporter::Reporter()
	{
	errors = 0;
	via_events = false;
	in_error_handler = 0;

	// Always use stderr at startup/init before scripts have been fully parsed
	// and zeek_init() processed.
	// Messages may otherwise be missed if an error occurs that prevents events
	// from ever being dispatched.
	info_to_stderr = true;
	warnings_to_stderr = true;
	errors_to_stderr = true;
	after_zeek_init = false;

	weird_count = 0;
	weird_sampling_rate = 0;
	weird_sampling_duration = 0;
	weird_sampling_threshold = 0;

	openlog("bro", 0, LOG_LOCAL5);
	}

Reporter::~Reporter()
	{
	closelog();
	}

void Reporter::InitOptions()
	{
	info_to_stderr = internal_val("Reporter::info_to_stderr")->AsBool();
	warnings_to_stderr = internal_val("Reporter::warnings_to_stderr")->AsBool();
	errors_to_stderr = internal_val("Reporter::errors_to_stderr")->AsBool();
	weird_sampling_rate = internal_val("Weird::sampling_rate")->AsCount();
	weird_sampling_threshold = internal_val("Weird::sampling_threshold")->AsCount();
	weird_sampling_duration = internal_val("Weird::sampling_duration")->AsInterval();
	auto wl_val = internal_val("Weird::sampling_whitelist")->AsTableVal();
	auto wl_table = wl_val->AsTable();

	HashKey* k;
	IterCookie* c = wl_table->InitForIteration();
	TableEntryVal* v;

	while ( (v = wl_table->NextEntry(k, c)) )
		{
		auto index = wl_val->RecoverIndex(k);
		string key = index->Index(0)->AsString()->CheckString();
		weird_sampling_whitelist.emplace(move(key));
		Unref(index);
		delete k;
		}
	}

void Reporter::SetAnalyzerSkip(analyzer::Analyzer* a)
	{
	if ( a )
		a->SetSkip(true);
	}

void Reporter::DoSyslog(const char* msg)
	{
	syslog(LOG_NOTICE, "%s", msg);
	}

void Reporter::UpdateWeirdStats(const char* name)
	{
	++weird_count;
	++weird_count_by_type[name];
	}

class NetWeirdTimer : public Timer {
public:
	NetWeirdTimer(double t, const char* name, double timeout)
	: Timer(t + timeout, TIMER_NET_WEIRD_EXPIRE), weird_name(name)
		{}

	void Dispatch(double t, int is_expire) override
		{ reporter->ResetNetWeird(weird_name); }

	std::string weird_name;
};

class FlowWeirdTimer : public Timer {
public:
	using IPPair = std::pair<IPAddr, IPAddr>;

	FlowWeirdTimer(double t, IPPair p, double timeout)
	: Timer(t + timeout, TIMER_FLOW_WEIRD_EXPIRE), endpoints(p)
		{}

	void Dispatch(double t, int is_expire) override
		{ reporter->ResetFlowWeird(endpoints.first, endpoints.second); }

	IPPair endpoints;
};

void Reporter::ResetNetWeird(const std::string& name)
	{
	net_weird_state.erase(name);
	}

void Reporter::ResetFlowWeird(const IPAddr& orig, const IPAddr& resp)
	{
	flow_weird_state.erase(std::make_pair(orig, resp));
	}

bool Reporter::PermitNetWeird(const char* name)
	{
	auto& count = net_weird_state[name];
	++count;

	if ( count == 1 )
		timer_mgr->Add(new NetWeirdTimer(network_time, name,
		                                 weird_sampling_duration));

	if ( count <= weird_sampling_threshold )
		return true;

	auto num_above_threshold = count - weird_sampling_threshold;
	if ( weird_sampling_rate )
		return num_above_threshold % weird_sampling_rate == 0;
	else
		return false;
	}

bool Reporter::PermitFlowWeird(const char* name,
                               const IPAddr& orig, const IPAddr& resp)
	{
	auto endpoints = std::make_pair(orig, resp);
	auto& map = flow_weird_state[endpoints];

	if ( map.empty() )
		timer_mgr->Add(new FlowWeirdTimer(network_time, endpoints,
		                                  weird_sampling_duration));

	auto& count = map[name];
	++count;

	if ( count <= weird_sampling_threshold )
		return true;

	auto num_above_threshold = count - weird_sampling_threshold;
	if ( weird_sampling_rate )
		return num_above_threshold % weird_sampling_rate == 0;
	else
		return false;
	}

void Reporter::Weird(const char* name, const char* addl)
	{
	UpdateWeirdStats(name);

	if ( ! WeirdOnSamplingWhiteList(name) )
		{
		if ( ! PermitNetWeird(name) )
			return;
		}

	WeirdHelper(net_weird, {new StringVal(addl)}, "%s", name);
	}

void Reporter::Weird(file_analysis::File* f, const char* name, const char* addl)
	{
	UpdateWeirdStats(name);

	if ( ! WeirdOnSamplingWhiteList(name) )
		{
		if ( ! f->PermitWeird(name, weird_sampling_threshold,
		                      weird_sampling_rate, weird_sampling_duration) )
			return;
		}

	WeirdHelper(file_weird, {f->GetVal()->Ref(), new StringVal(addl)},
	            "%s", name);
	}

void Reporter::Weird(Connection* conn, const char* name, const char* addl)
	{
	UpdateWeirdStats(name);

	if ( ! WeirdOnSamplingWhiteList(name) )
		{
		if ( ! conn->PermitWeird(name, weird_sampling_threshold,
		                         weird_sampling_rate, weird_sampling_duration) )
			return;
		}

	WeirdHelper(conn_weird, {conn->BuildConnVal(), new StringVal(addl)},
	            "%s", name);
	}

void Reporter::Weird(const IPAddr& orig, const IPAddr& resp, const char* name, const char* addl)
	{
	UpdateWeirdStats(name);

	if ( ! WeirdOnSamplingWhiteList(name) )
		{
		if ( ! PermitFlowWeird(name, orig, resp) )
			 return;
		}

	WeirdHelper(flow_weird,
	            {new AddrVal(orig), new AddrVal(resp), new StringVal(addl)},
	            "%s", name);
	}

string Reporter::BuildLogLocationString(bool location)
	{
	string loc_str;
	if ( location )
		{
		string loc_file = "";
		int loc_line = 0;

		if ( locations.size() )
			{
			ODesc d;

			std::pair<const Location*, const Location*> locs = locations.back();

			if ( locs.first )
				{
				if ( locs.first != &no_location )
					locs.first->Describe(&d);

				else
					d.Add("<no location>");

				if ( locs.second )
					{
					d.Add(" and ");

					if ( locs.second != &no_location )
						locs.second->Describe(&d);

					else
						d.Add("<no location>");

					}
				}

			loc_str = d.Description();
			}

		else if ( filename && *filename )
			{
			// Take from globals.
			loc_str = filename;
			char tmp[32];
			snprintf(tmp, 32, "%d", line_number);
			loc_str += string(", line ") + string(tmp);
			}
		}

	return loc_str;
	}

void Reporter::DoLogEvents(const char* prefix, EventHandlerPtr event, Connection* conn,
                           val_list* addl, bool location, bool time, char* buffer,
                           const string& loc_str)
	{
	bool raise_event = true;

	if ( via_events && ! in_error_handler )
		{
		if ( locations.size() )
			{
			auto locs = locations.back();
			raise_event = PLUGIN_HOOK_WITH_RESULT(HOOK_REPORTER,
							      HookReporter(prefix, event, conn, addl, location,
									  locs.first, locs.second, time, buffer), true);
			}
		else
			raise_event = PLUGIN_HOOK_WITH_RESULT(HOOK_REPORTER,
							      HookReporter(prefix, event, conn, addl, location,
									   nullptr, nullptr, time, buffer), true);
		}

	if ( raise_event && event && via_events && ! in_error_handler )
		{
		auto vl_size = 1 + (bool)time + (bool)location + (bool)conn +
		               (addl ? addl->length() : 0);

		val_list vl(vl_size);

		if ( time )
			vl.push_back(new Val(network_time ? network_time : current_time(), TYPE_TIME));

		vl.push_back(new StringVal(buffer));

		if ( location )
			vl.push_back(new StringVal(loc_str));

		if ( conn )
			vl.push_back(conn->BuildConnVal());

		if ( addl )
			std::copy(addl->begin(), addl->end(), std::back_inserter(vl));

		if ( conn )
			conn->ConnectionEventFast(event, 0, std::move(vl));
		else
			mgr.QueueEventFast(event, std::move(vl));
		}
	else
		{
		if ( addl )
			{
			for ( const auto& av : *addl )
				Unref(av);
			}
		}

	}
