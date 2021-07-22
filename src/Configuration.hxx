
#ifndef K8PSH_CONFIGURATION_HXX
#define K8PSH_CONFIGURATION_HXX

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

	public:
		// Gets the name of the host.
		const std::string &getHostname() const { return _hostname; }

		// Gets the port of the host.
		unsigned short getPort() const { return _port; }
	};

	class Command
	{
		friend class Configuration;

		Host _host;
		std::string _name;
		std::vector<std::string> _executable;
		std::vector<std::string> _environmentVariables;

	public:
		// Gets the host of the command.
		const Host &getHost() const { return _host; }

		// Gets the name of the command.
		const std::string &getName() const { return _name; }

		// Gets the command executable.
		const std::vector<std::string> &getExecutable() const { return _executable; }

		// Gets the environment variables used by the command.
		const std::vector<std::string> &getEnvironmentVariables() const { return _environmentVariables; }
	};

	typedef std::unordered_map<std::string, Command> CommandMap;
	typedef std::unordered_map<std::string, CommandMap> HostCommandsMap;

private:
	std::string _workingDirectory;
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

	// Gets the working directory for the configuration.
	const std::string &getWorkingDirectory() const { return _workingDirectory; }
};

} // k8psh

#endif // K8PSH_CONFIGURATION_HXX
