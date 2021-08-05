// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#ifndef K8PSH_CONFIGURATION_HXX
#define K8PSH_CONFIGURATION_HXX

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace k8psh {

class Configuration
{
	static constexpr unsigned short DEFAULT_STARTING_PORT = 1120;

public:
	class Host
	{
		friend class Configuration;

		std::string _hostname;
		unsigned short _port;
		std::vector<std::string> _options;

	public:
		// Gets the name of the host.
		const std::string &getHostname() const { return _hostname; }

		// Gets the options for the host.
		const std::vector<std::string> &getOptions() const { return _options; }

		// Gets the port of the host.
		unsigned short getPort() const { return _port; }
	};

	class Command
	{
		friend class Configuration;

		std::shared_ptr<Host> _host;
		std::string _name;
		std::vector<std::string> _executable;
		std::vector<std::pair<std::string, std::string> > _environmentVariables;

	public:
		// Gets the host of the command.
		const Host &getHost() const { return *_host; }

		// Gets the name of the command.
		const std::string &getName() const { return _name; }

		// Gets the command executable.
		const std::vector<std::string> &getExecutable() const { return _executable; }

		// Gets the environment variables used by the command.
		const std::vector<std::pair<std::string, std::string> > &getEnvironmentVariables() const { return _environmentVariables; }
	};

	typedef std::unordered_map<std::string, Command> CommandMap;
	typedef std::unordered_map<std::string, CommandMap> HostCommandsMap;

private:
	std::string _baseDirectory;
	long long _connectTimeoutMs;
	HostCommandsMap _hostCommands;
	CommandMap _commands;

public:
	// Loads the configuration from a string.
	static Configuration load(const std::string &configurationString, const std::string &workingPath = "");

	// Gets the commands for the specified host from the configuration.
	const CommandMap *getCommands(const std::string &hostname) const
	{
		typedef const CommandMap *ReturnType;

		auto it = _hostCommands.find(hostname);
		return it == _hostCommands.end() ? ReturnType() : &it->second;
	}

	// Gets all commands from the configuration.
	const CommandMap &getCommands() const { return _commands; }

	// Gets the timeout in milliseconds before failing to connect to the server.
	long long getConnectTimeoutMs() const { return _connectTimeoutMs; }

	// Gets the base directory for the configuration.
	const std::string &getBaseDirectory() const { return _baseDirectory; }
};

} // k8psh

#endif // K8PSH_CONFIGURATION_HXX
