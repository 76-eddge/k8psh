// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#include "Configuration.cxx"

#include <iostream>
#include <string>
#include <vector>

#include "Test.hxx"

#include "Utilities.cxx"

namespace std {

static std::ostream &operator<<(std::ostream &out, const std::vector<std::string> &vector)
{
	for (auto it = vector.begin(); it != vector.end(); ++it)
		out << " '" << *it << '\'';

	return out;
}

} // std

namespace k8psh {

static std::ostream &operator<<(std::ostream &out, const k8psh::Configuration::Command &command)
{
	return out << command.getName() << '@' << command.getHost().getHostname() << ':' << command.getHost().getPort() << command.getEnvironmentVariables() << " |" << command.getExecutable();
}

} // k8psh

static bool equals(const k8psh::Configuration::Command &command, const std::string &name, const std::vector<std::string> &environmentVariables, const std::vector<std::string> &executable)
{
	std::cout << ' ' << command << std::endl;
	return command.getName() == name &&
		command.getEnvironmentVariables() == environmentVariables &&
		command.getExecutable() == executable;
}

int main()
{
	std::vector<std::string> badConfigurations = {
		"badoption=value",
		"workingDirectory=bad value",
		"workingDirectory=\"good val\tue\" extra\n# This is the second line",
		"workingDirectory=\"bad value",
		"workingDirectory=\"bad value\\k\"",
		"workingDirectory='bad value",
		"workingDirectory=${PATH}\n"
			"[\nblah]",
		"workingDirectory=${PATH}\n"
			"[ blah:65536 ]",
		"workingDirectory=${PATH}\n"
			"[ blah:65_36 ]",
		"workingDirectory=${PATH}\n"
			"[blah\n]",
	};

	for (auto it = badConfigurations.begin(); it != badConfigurations.end(); ++it)
		TEST_THROWS(k8psh::Configuration::load(*it));

	TEST_THAT(k8psh::Utilities::setEnvironmentVariable("TEST_ENV_1", "blah"));
	TEST_THAT(k8psh::Utilities::setEnvironmentVariable("TEST_ENV_2", "blah2"));
	k8psh::Configuration config = k8psh::Configuration::load("workingDirectory = ${TEST_ENV_1}/${TEST_ENV_2} # The directory that all relative working directories will be based on\n"
		"\n"
		"# Test comment\n"
		"[empty]\n"
		"\n"
		"[ blah:1895 ] # section tags are strings, so spaces can be inside []\n"
		"blah A= ?B= test blah-real 'First Arg' \"\\\"Escaped\\\"\\tArg \"\"\"\n"
		"some_exe theExe\n"
		"['blah 2']\n"
		"blah ENV= # Only name is required");

	TEST_THAT(config.getWorkingDirectory() == k8psh::Utilities::getAbsolutePath(k8psh::Utilities::getWorkingDirectory() + "/blah/blah2"));
	TEST_THAT(!config.getCommands("non-existant"));

	// Test client commands
	std::cout << std::endl << "[Client Commands]:" << std::endl;
	auto commandsMap = config.getCommands();
	TEST_THAT(equals(commandsMap["blah"], "blah", { "ENV" }, { "blah" }));
	TEST_THAT(equals(commandsMap["some_exe"], "some_exe", { }, { "theExe" }));

	// Test server-specific commands
	std::cout << std::endl << "blah:" << std::endl;
	auto blahMap = *config.getCommands("blah");
	TEST_THAT(equals(blahMap["blah"], "blah", { "A", "?B" }, { "test", "blah-real", "First Arg", "\"Escaped\"\tArg \"" }));
	TEST_THAT(equals(blahMap["some_exe"], "some_exe", { }, { "theExe" }));

	std::cout << std::endl << "blah 2:" << std::endl;
	auto blah2Map = *config.getCommands("blah 2");
	TEST_THAT(equals(blah2Map["blah"], "blah", { "ENV" }, { "blah" }));

	std::cout << "Finished testing configuration" << std::endl;
}
