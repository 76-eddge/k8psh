
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
	#include <functional>
	#include <thread>

	#include <windows.h>
#else
	#include <fcntl.h>
	#include <signal.h>
	#include <sys/stat.h>
	#include <unistd.h>
#endif

#include "Configuration.hxx"
#include "Utilities.hxx"
#include "Process.hxx"

static const std::string clientName = "k8psh";
static const std::string serverName = clientName + "d";
static const std::string environmentPrefix = "K8PSH_";

#ifdef GIT_VERSION
	#define QUOTE_(X) #X
	#define QUOTE(X) QUOTE_(X)
static const std::string version = QUOTE(GIT_VERSION);
	#undef QUOTE_
	#undef QUOTE
#else
static const std::string version = "???";
#endif

// Gets the base name of the command.
static std::string getBaseCommandName(const char *command)
{
#ifdef _WIN32
	std::string name = k8psh::Utilities::getBasename(command);

	return (name.length() < 4 || name.compare(name.length() - 4, 4, ".exe") != 0) ? name : name.substr(0, name.length() - 4);
#else
	return k8psh::Utilities::getBasename(command);
#endif
}

// Gets the configuration.
static k8psh::Configuration getConfiguration(const k8psh::OptionalString &config)
{
	const auto configurationSpecified = config ? config : k8psh::Utilities::getEnvironmentVariable(environmentPrefix + "CONFIG");
	const auto configurationFile = configurationSpecified ? static_cast<const std::string &>(configurationSpecified) : (clientName + ".conf");
	const auto configurationString = k8psh::Utilities::readFile(configurationFile);

	if (!configurationString)
		LOG_ERROR << "Configuration could not be loaded from " << configurationFile;

	LOG_DEBUG << "Loading configuration from file " << configurationFile;
	return k8psh::Configuration::load(configurationString, k8psh::Utilities::getParentDirectory(configurationFile));
}

/** Parses a command line option from the arguments provided.
 * 
 * @param argument the current argument being evaluated
 * @param shortOption the short representation of this option
 * @param option the long representation of this option
 * @param expecting the pretty string representing the expected value to parse as an additional argument
 * @param index a reference to the current index of the argument being parsed
 * @param argumentCount the total number of arguments
 * @param arguments the arguments
 * @param value a reference to the value that will be set to the option's argument
 * @return true if the option is parsed successfully, otherwise false
 */
static bool parseOption(const std::string &argument, const std::string &shortOption, const std::string &option, const std::string &expecting, int &index, int argumentCount, const char *arguments[], std::string &value)
{
	if ((argument == shortOption && !shortOption.empty()) || argument == option)
	{
		if (index + 1 == argumentCount)
			LOG_ERROR << "Expecting " << expecting << " after argument " << argument;

		value = arguments[++index];
		return true;
	}

	if (argument.length() > option.length() && argument.substr(0, option.length() + 1) == option + "=")
	{
		value = argument.substr(option.length() + 1);
		return true;
	}

	return false;
}

// The main() for connecting to the server and starting the process
static int mainClient(int argc, const char *argv[])
{
	std::string commandName = getBaseCommandName(argv[0]);
	k8psh::OptionalString config;
	int i = 0;

	// Parse command line arguments
	if (commandName == clientName)
	{
		for (i++; i < argc; i++)
		{
			std::string arg = argv[i];
			LOG_DEBUG << "Parsing command line argument " << arg;

			if (arg == "-v" || arg == "--version")
			{
				std::cout << commandName << ' ' << version << std::endl;
				return 0;
			}

			else if (parseOption(arg, "-c", "--config", "[config]", i, argc, argv, config))
				config.exists();

			else if (arg == "-h" || arg == "--help")
			{
				std::cout << "Usage: " << commandName << " [options] command..." << std::endl;
				std::cout << "  Executes a " << commandName << " client command" << std::endl;
				std::cout << std::endl;
				std::cout << "Options:" << std::endl;
				std::cout << "  -c, --config [file]" << std::endl;
				std::cout << "      The configuration file loaded by " << commandName << ". Defaults to $" << environmentPrefix << "CONFIG." << std::endl;
				std::cout << "  -h, --help" << std::endl;
				std::cout << "      Displays usage and exits." << std::endl;
				std::cout << "  -v, --version" << std::endl;
				std::cout << "      Prints the version and exits." << std::endl;
				return 0;
			}

			else
			{
				commandName = argv[i];
				break;
			}
		}
	}

	// Load the configuration and start the process
	const k8psh::Configuration configuration = getConfiguration(config);
	auto commandIt = configuration.getCommands().find(commandName);

	if (commandIt == configuration.getCommands().end())
		LOG_ERROR << "Failed to find command \"" << commandName << "\" in configuration";

	LOG_DEBUG << "Starting command " << commandName;
	return k8psh::Process::runRemoteCommand(k8psh::Utilities::relativize(configuration.getWorkingDirectory(), k8psh::Utilities::getWorkingDirectory()), commandIt->second, argc - i, argv + i);
}

#ifdef _WIN32
static HANDLE exitRequested;
#else
static k8psh::Pipe exitRequested;
#endif

// Handles signals by requesting that the application exit
extern "C" void handleSignal(int signal)
{
	LOG_DEBUG << "Handling signal " << signal;
#ifdef _WIN32
	(void)SetEvent(exitRequested);
#else
	exitRequested.closeInput();
#endif
}

#ifdef _WIN32
BOOL WINAPI handleCtrlC(DWORD)
{
	handleSignal(SIGINT);
	return TRUE;
}
#endif

// The main() for the server that waits requests to run executables in the configuration
static int mainServer(int argc, const char *argv[])
{
	std::string commandName = getBaseCommandName(argv[0]);
	bool daemonize = false;
	bool disableClientExecutables = false;
	bool generateLocalExecutables = false;
	bool overwriteClientExecutables = false;
	k8psh::OptionalString config;
	std::string directory;
	std::string name;
	std::string pidFilename = "/var/run/" + serverName + ".pid";

	// Parse command line arguments
	for (int i = 1; i < argc; i++)
	{
		std::string arg = argv[i];
		LOG_DEBUG << "Parsing command line argument " << arg;

		if (arg == "-b" || arg == "--background")
			daemonize = true;

		else if (arg == "-d" || arg == "--disable-client-executables")
			disableClientExecutables = true;

		else if (arg == "-l" || arg == "--generate-local-executables")
			generateLocalExecutables = true;

		else if (arg == "-o" || arg == "--overwrite-client-executables")
			overwriteClientExecutables = true;

		else if (arg == "-v" || arg == "--version")
		{
			std::cout << commandName << ' ' << version << std::endl;
			return 0;
		}

		else if (parseOption(arg, "-c", "--config", "[file]", i, argc, argv, config))
			config.exists();

		else if (parseOption(arg, "-e", "--executable-directory", "[directory]", i, argc, argv, directory))
			directory += '/';

		else if (parseOption(arg, "-n", "--name", "[name]", i, argc, argv, name) ||
				parseOption(arg, "-p", "--pidfile", "[file]", i, argc, argv, pidFilename))
			; // Option recognized and parsed

		else if (arg == "-h" || arg == "--help")
		{
			std::cout << "Usage: " << commandName << " [options]" << std::endl;
			std::cout << "  Starts the " << commandName << " server" << std::endl;
			std::cout << std::endl;
			std::cout << "Options:" << std::endl;
			std::cout << "  -b, --background" << std::endl;
			std::cout << "      Daemonize the server by sending it to the background." << std::endl;
			std::cout << "  -c, --config [file]" << std::endl;
			std::cout << "      The configuration file loaded by " << commandName << ". Defaults to $" << environmentPrefix << "CONFIG." << std::endl;
			std::cout << "  -d, --disable-client-executables" << std::endl;
			std::cout << "      Disables generating client executables so only local executables can be run." << std::endl;
			std::cout << "  -e, --executable-directory [directory]" << std::endl;
			std::cout << "      The directory used to create the client executables." << std::endl;
			std::cout << "  -h, --help" << std::endl;
			std::cout << "      Displays usage and exits." << std::endl;
			std::cout << "  -l, --generate-local-executables" << std::endl;
			std::cout << "      Generate client executables for local executables." << std::endl;
			std::cout << "  -n, --name [name]" << std::endl;
			std::cout << "      The name used to identify the server. Defaults to $" << environmentPrefix << "NAME or hostname." << std::endl;
			std::cout << "  -o, --overwrite-client-executables" << std::endl;
			std::cout << "      Overwrite client executables rather than fail with error." << std::endl;
			std::cout << "  -p, --pidfile [file]" << std::endl;
			std::cout << "      The file to store the PID of the server." << std::endl;
			std::cout << "  -v, --version" << std::endl;
			std::cout << "      Prints the version and exits." << std::endl;
			return 0;
		}

		else
			LOG_WARNING << "Ignoring unrecognized option " << arg;
	}

	// Check name
	if (name.empty())
	{
		const auto envVariable = k8psh::Utilities::getEnvironmentVariable(environmentPrefix + "NAME");

		if (envVariable)
			name = envVariable;
		else
		{
			name = k8psh::Utilities::getHostname();

			if (name.empty())
				LOG_ERROR << "Hostname could not be determined, --name must be specified";
		}
	}

	// Process server configuration
	const auto configuration = getConfiguration(config);
	const auto serverCommands = configuration.getCommands(name);
	k8psh::Socket listener;

	if (!serverCommands || serverCommands->empty())
		LOG_WARNING << "No server commands found in configuration, sleeping";
	else
		listener = k8psh::Socket::listen(serverCommands->begin()->second.getHost().getPort());

	// Generate the appropriate client symlinks / executables
#ifdef _WIN32
	std::string clientCommand;
#else
	std::string clientCommand = k8psh::Utilities::getExecutablePath();
#endif
	const auto commands = configuration.getCommands();

	for (auto it = commands.begin(); it != commands.end(); ++it)
	{
#ifdef _WIN32
		const std::string filename = directory + it->second.getName() + ".exe";
#else
		const std::string filename = directory + it->second.getName();
#endif

		if (disableClientExecutables || overwriteClientExecutables)
			(void)k8psh::Utilities::deleteFile(filename.c_str());

		if (!disableClientExecutables && (generateLocalExecutables || name != it->second.getHost().getHostname()))
		{
#ifdef _WIN32
			if (clientCommand.empty())
				clientCommand = k8psh::Utilities::getExecutablePath(); // Only use executable when copying (hardlinks can't be deleted while in use, so overwrite client executables option will fail)
			else if (CreateHardLinkA(filename.c_str(), clientCommand.c_str(), NULL) != 0)
				continue; // Successfully hardlinked

			if (CopyFileA(clientCommand.c_str(), filename.c_str(), overwriteClientExecutables ? FALSE : TRUE) != 0)
				clientCommand = filename; // Try to hardlink to copied file from now on
			else
#else
			if (symlink(clientCommand.c_str(), filename.c_str()) != 0)
#endif
				LOG_ERROR << "Failed to create client executable for command " << it->second.getName();
		}
	}

	// Daemonize, if desired
	if (daemonize)
	{
#ifdef _WIN32
		LOG_ERROR << "Daemon not supported";
#else
		LOG_DEBUG << "Starting daemon";

		switch (fork())
		{
		case 0:
			break;

		case -1:
			LOG_ERROR << "Failed to fork daemon";

		default:
			return 0;
		}

		if (setsid() == -1)
			return -1;

		signal(SIGHUP, SIG_IGN);

		(void)k8psh::Utilities::changeWorkingDirectory("/");
		(void)umask(0);

		int devNull = open("/dev/null", O_RDWR, 0);

		if (devNull == -1)
			LOG_ERROR << "Failed to open /dev/null for daemon";

		(void)dup2(devNull, STDIN_FILENO);
		(void)dup2(devNull, STDOUT_FILENO);
		(void)dup2(devNull, STDERR_FILENO);
		(void)close(devNull);
#endif
	}
#ifdef SIGHUP
	else
		signal(SIGHUP, handleSignal);
#endif

	signal(SIGTERM, handleSignal);
	signal(SIGINT, handleSignal);

#ifdef _WIN32
	(void)SetConsoleCtrlHandler(handleCtrlC, TRUE);
#else
	// Create PID file
	FILE *pidFile = pidFilename.empty() ? NULL : fopen(pidFilename.c_str(), "w");

	if (pidFile)
	{
		(void)fprintf(pidFile, "%d\n", getpid());
		(void)fclose(pidFile);
	}
#endif

	// Main loop
#ifdef _WIN32
	HANDLE waitSet[2];

	waitSet[0] = exitRequested = CreateEventA(NULL, FALSE, FALSE, "ServerExit");
	waitSet[1] = listener.createReadEvent();
#else
	struct pollfd pollSet[2] = { };

	pollSet[0].fd = exitRequested.getOutput();
	pollSet[0].events = POLLIN;

	pollSet[1].fd = listener.createReadEvent();
	pollSet[1].events = POLLIN;
#endif

	LOG_DEBUG << "Entering server connection listener loop";

	for (;;)
	{
#ifdef _WIN32
		DWORD waitResult = WaitForMultipleObjects(DWORD(sizeof(waitSet) / sizeof(waitSet[0]) - (listener.isValid() ? 0 : 1)), waitSet, FALSE, INFINITE);

		if (waitResult == WAIT_FAILED)
			LOG_ERROR << "Failed to wait on multiple objects for new clients: " << GetLastError();
		else if (waitResult == WAIT_OBJECT_0)
			break;
#else
		int pollResult;

		do pollResult = poll(pollSet, nfds_t(sizeof(pollSet) / sizeof(pollSet[0]) - (listener.isValid() ? 0 : 1)), -1);
		while (pollResult < 0 && (errno == EAGAIN || errno == EINTR));

		if (pollResult < 0 || (pollSet[1].revents & POLLERR) != 0)
			LOG_ERROR << "Failed to poll for new clients: " << errno;
		else if ((pollSet[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
			break;
#endif

		if (!listener.isValid())
			continue;

		k8psh::Socket client = listener.accept();

		// Start the child thread / process
		if (client.isValid())
		{
			LOG_DEBUG << "Accepted connection from new client";
#ifdef _WIN32
			std::thread(k8psh::Process::start, configuration.getWorkingDirectory(), *serverCommands, std::move(client)).detach();
#else
			switch (fork())
			{
			case 0:
				(void)close(listener.abandon());
				k8psh::Process::start(configuration.getWorkingDirectory(), *serverCommands, std::move(client));
				return 0;

			case -1:
				LOG_ERROR << "Failed to fork client";

			default:
				(void)close(client.abandon());
				break;
			}
#endif
		}
	}

	LOG_DEBUG << "Shutting down the server";

#ifndef _WIN32
	// Remove PID file
	if (pidFile && !k8psh::Utilities::deleteFile(pidFilename.c_str()))
		LOG_WARNING << "Failed to remove pidfile " << pidFilename;
#endif

	for (auto it = commands.begin(); it != commands.end(); ++it)
	{
		if (!disableClientExecutables && (generateLocalExecutables || name != it->second.getHost().getHostname()) &&
#ifdef _WIN32
				!k8psh::Utilities::deleteFile(directory + it->second.getName() + ".exe")
#else
				!k8psh::Utilities::deleteFile(directory + it->second.getName())
#endif
				)
			LOG_WARNING << "Failed to remove client executable for command " << it->second.getName();
	}

	return 0;
}

int main(int argc, const char *argv[])
{
	try
	{
		k8psh::Socket::Initializer socketInit;
		std::string commandName = getBaseCommandName(argv[0]);

		if (commandName == serverName)
			return mainServer(argc, argv);
		else
			return mainClient(argc, argv);
	}
	catch (...)
	{
		return 1;
	}
}
