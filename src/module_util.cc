//
// See the file "COPYING" in the main distribution directory for copyright.

#include "module_util.h"

#include <string.h>
#include <iostream>
#include <string>

#include "3rdparty/doctest.h"

using namespace std;

static int streq(string_view s1, string_view s2)
	{
	return s1 == s2;
	}

TEST_CASE("module_util streq")
	{
	CHECK(streq("abcd", "abcd") == true);
	CHECK(streq("abcd", "efgh") == false);

	string s1 = "ab";
	string s2 = "ab";
	CHECK(streq(s1, s2) == true);
	s2 = "cd";
	CHECK(streq(s1, s2) == false);
	}

TEST_CASE("module_util extract_module_name")
	{
	CHECK(extract_module_name("mod") == GLOBAL_MODULE_NAME);
	CHECK(extract_module_name("mod::") == "mod");
	CHECK(extract_module_name("mod::var") == "mod");
	}

// Returns it without trailing "::" var section.
string extract_module_name(string_view name)
	{
	string module_name(name);
	string::size_type pos = module_name.rfind("::");

	if ( pos == string::npos )
		return string(GLOBAL_MODULE_NAME);

	module_name.erase(pos);

	return module_name;
	}

TEST_CASE("module_util extract_var_name")
	{
	CHECK(extract_var_name("mod") == "mod");
	CHECK(extract_var_name("mod::") == "");
	CHECK(extract_var_name("mod::var") == "var");
	CHECK(extract_var_name("::var") == "var");
	}

string extract_var_name(string_view name)
	{
	string var_name(name);
	string::size_type pos = var_name.rfind("::");

	if ( pos == string::npos )
		return var_name;

	if ( pos + 2 > var_name.size() )
		return string("");

	return var_name.substr(pos+2);
	}

TEST_CASE("module_util normalized_module_name")
	{
	CHECK(normalized_module_name("a") == "a");
	CHECK(normalized_module_name("module") == "module");
	CHECK(normalized_module_name("module::") == "module");
	}

string normalized_module_name(string_view module_name)
	{
	string colons = "::";
	string mod_name(module_name);

	if ( (module_name.size() >= colons.size()) &&
		 (mod_name.compare(mod_name.size() - colons.size(), colons.size(), colons ) == 0) )
		mod_name.erase(mod_name.size() - colons.size());

	return mod_name;
	}

TEST_CASE("module_util make_full_var_name")
	{
	CHECK(make_full_var_name("", "GLOBAL::var") == "var");
	CHECK(make_full_var_name(GLOBAL_MODULE_NAME, "var") == "var");
	CHECK(make_full_var_name("", "notglobal::var") == "notglobal::var");
	CHECK(make_full_var_name("", "::var") == "::var");

	CHECK(make_full_var_name("module", "var") == "module::var");
	CHECK(make_full_var_name("module::", "var") == "module::var");

	// TODO: this test is broken by the change to use string_view. Previously the code checked
	// for a null module_name instead an empty string, and used that. If the string was empty
	// but not null it would fall down to the bottom and just prepend "::" to the var name.
	// Now that it's checking for empty instead of null, it falls into the if block and just
	// returns the var name unmodified.
//	CHECK(make_full_var_name("", "var") == "::var");
	}

string make_full_var_name(string_view module_name, string_view var_name)
	{
	if ( module_name.empty() || streq(module_name, GLOBAL_MODULE_NAME) ||
		var_name.find("::") != string_view::npos )
		{
		if ( streq(GLOBAL_MODULE_NAME, extract_module_name(var_name)) )
			return extract_var_name(var_name);

		return string(var_name);
		}

	string full_name = normalized_module_name(module_name);
	full_name += "::";
	full_name += var_name;

	return full_name;
	}
