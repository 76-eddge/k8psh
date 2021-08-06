// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
	#include <functional>
	#include <thread>

	#include <windows.h>
#else
	#include <fcntl.h>
	#include <poll.h>
	#include <signal.h>
	#include <sys/stat.h>
	#include <sys/wait.h>
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
 * @param deferredArgs an optional deferred arguments list to be populated with the resulting arguments instead of filling the value
 * @return true if the option is parsed successfully, otherwise false
 */
template <typename ArgListT> static bool parseOption(const std::string &argument, const std::string &shortOption, const std::string &option, const std::string &expecting, std::size_t &index, std::size_t argumentCount, const ArgListT &arguments, std::string &value, std::vector<std::string> *deferredArgs = nullptr)
{
	if ((argument == shortOption && !shortOption.empty()) || argument == option)
	{
		if (index + 1 == argumentCount)
			LOG_ERROR << "Expecting " << expecting << " after argument " << argument;

		if (deferredArgs)
		{
			deferredArgs->emplace_back(argument);
			deferredArgs->emplace_back(arguments[++index]);
		}
		else
			value = arguments[++index];

		return true;
	}

	if (argument.length() > option.length() && argument.substr(0, option.length() + 1) == option + "=")
	{
		if (deferredArgs)
		{
			deferredArgs->emplace_back(option);
			deferredArgs->emplace_back(argument.substr(option.length() + 1));
		}
		else
			value = argument.substr(option.length() + 1);

		return true;
	}

	return false;
}

// The main() for connecting to the server and starting the process
static int mainClient(int argc, const char *argv[])
{
	std::string commandName = k8psh::Utilities::getExecutableBasename(argv[0]);
	k8psh::OptionalString config;
	std::size_t i = 1;

	// Parse command line arguments
	if (commandName == clientName)
	{
		for (; i < std::size_t(argc); i++)
		{
			std::string arg = argv[i];
			LOG_DEBUG << "Parsing command line argument " << arg;

			if (parseOption(arg, "-c", "--config", "[config]", i, argc, argv, config))
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
			else if (arg == "-v" || arg == "--version")
			{
				std::cout << commandName << ' ' << version << std::endl;
				return 0;
			}
			else
			{
				commandName = argv[i++];
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
	return k8psh::Process::runRemoteCommand(k8psh::Utilities::relativizePath(configuration.getBaseDirectory(), k8psh::Utilities::getWorkingDirectory()), commandIt->second, std::size_t(argc) - i, argv + i, configuration);
}

#ifdef _WIN32
static HANDLE exitRequested;
#else
static k8psh::Pipe exitRequested;
#endif

// Clamps a value between 0 and some maximum value.
template <typename T> static T clampPositive(T value, T max)
{
	if (value < 0)
		return 0;
	else if (value > max)
		return max;

	return value;
}

// Handles signals by requesting that the application exit.
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

static const std::string defaultPidFilename = "/run/" + serverName + ".pid";

// The main() for the server that waits requests to run executables in the configuration
static int mainServer(int argc, const char *argv[])
{
	k8psh::Socket::Initializer socketInit;
	std::string commandName = k8psh::Utilities::getExecutableBasename(argv[0]);
	std::vector<std::string> deferredArgs;
	bool daemonize = false;
	bool disableClientExecutables = false;
	bool generateLocalExecutables = false;
	bool ignoreInvalidArguments = false;
	bool keepClientExecutables = false;
	bool overwriteClientExecutables = false;
	bool waitOnClientConnections = true;
	k8psh::OptionalString config;
	std::string directory;
	std::string name;
	std::string connections = "-1";
	std::string pidFilename = defaultPidFilename;
	std::string timeout = "-1";

	// Parse command line arguments
	for (std::size_t i = 1; i < std::size_t(argc); i++)
	{
		std::string arg = argv[i];
		LOG_DEBUG << "Parsing command line argument " << arg;

		if (arg == "-b" || arg == "--background" ||
				arg == "-d" || arg == "--disable-client-executables" ||
				arg == "-k" || arg == "--keep-client-executables" ||
				arg == "-l" || arg == "--generate-local-executables" ||
				arg == "-o" || arg == "--overwrite-client-executables" ||
				arg == "-w" || arg == "--no-wait")
			deferredArgs.emplace_back(arg);
		else if (parseOption(arg, "-e", "--executable-directory", "[directory]", i, argc, argv, directory, &deferredArgs) ||
				parseOption(arg, "-m", "--max-connections", "[connections]", i, argc, argv, connections, &deferredArgs) ||
				parseOption(arg, "-p", "--pidfile", "[file]", i, argc, argv, pidFilename, &deferredArgs) ||
				parseOption(arg, "-t", "--timeout", "[ms]", i, argc, argv, timeout, &deferredArgs))
			; // Option recognized and parsed
		else if (parseOption(arg, "-c", "--config", "[file]", i, argc, argv, config))
			config.exists();
		else if (parseOption(arg, "-n", "--name", "[name]", i, argc, argv, name))
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
			std::cout << "  -i, --ignore-invalid-arguments" << std::endl;
			std::cout << "      Invalid arguments will generate a warning rather than an error." << std::endl;
			std::cout << "  -k, --keep-client-executables" << std::endl;
			std::cout << "      Keeps client executables instead of removing them on exit." << std::endl;
			std::cout << "  -l, --generate-local-executables" << std::endl;
			std::cout << "      Generate client executables for local executables." << std::endl;
			std::cout << "  -m, --max-connections [connections]" << std::endl;
			std::cout << "      The maximum number of connections to accept before the server exits. Defaults to -1 (no limit)." << std::endl;
			std::cout << "  -n, --name [name]" << std::endl;
			std::cout << "      The name used to identify the server. Defaults to $" << environmentPrefix << "NAME or hostname." << std::endl;
			std::cout << "  -o, --overwrite-client-executables" << std::endl;
			std::cout << "      Overwrite client executables rather than fail with error." << std::endl;
			std::cout << "  -p, --pidfile [file]" << std::endl;
			std::cout << "      The file to store the PID of the server. Defaults to " << defaultPidFilename << "." << std::endl;
			std::cout << "  -t, --timeout [ms]" << std::endl;
			std::cout << "      The time in milliseconds before the server exits. Defaults to -1 (run forever)." << std::endl;
			std::cout << "  -v, --version" << std::endl;
			std::cout << "      Prints the version and exits." << std::endl;
			std::cout << "  -w, --no-wait" << std::endl;
			std::cout << "      Do not wait for client connections to terminate." << std::endl;
			return 0;
		}
		else if (arg == "-i" || arg == "--ignore-invalid-arguments")
			ignoreInvalidArguments = true;
		else if (arg == "-v" || arg == "--version")
		{
			std::cout << commandName << ' ' << version << std::endl;
			return 0;
		}
		else if (ignoreInvalidArguments)
			LOG_WARNING << "Ignoring unrecognized argument " << arg;
		else
			LOG_ERROR << "Invalid argument " << arg;
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
	std::size_t deferredArgc = deferredArgs.size();
	const auto configuration = getConfiguration(config);
	const auto serverCommands = configuration.getCommands(name);
	k8psh::Socket listener;

	if (!serverCommands || serverCommands->empty())
	{
		LOG_DEBUG << "No server commands found in configuration, creating client executables and exiting";
		keepClientExecutables = true;
	}
	else
	{
		const auto host = serverCommands->begin()->second.getHost();

		// Update deferred arguments and set limit to prevent overlapping config file and command line arguments (prepend the config file arguments, so command line will be processed last)
		deferredArgc = host.getOptions().empty() ? deferredArgc : host.getOptions().size();
		deferredArgs.insert(deferredArgs.begin(), host.getOptions().begin(), host.getOptions().end());
		listener = k8psh::Socket::listen(host.getPort());
	}

	// Parse deferred command line arguments
	for (std::size_t i = 0; i < deferredArgc; i++)
	{
		const std::string &arg = deferredArgs[i];
		LOG_DEBUG << "Parsing deferred command line argument " << arg;

		if (arg == "-b" || arg == "--background")
			daemonize = true;
		else if (arg == "-d" || arg == "--disable-client-executables")
			disableClientExecutables = true;
		else if (parseOption(arg, "-e", "--executable-directory", "[directory]", i, deferredArgc, deferredArgs, directory))
			directory += '/';
		else if (arg == "-i" || arg == "--ignore-invalid-arguments")
			ignoreInvalidArguments = true;
		else if (arg == "-k" || arg == "--keep-client-executables")
			keepClientExecutables = true;
		else if (arg == "-l" || arg == "--generate-local-executables")
			generateLocalExecutables = true;
		else if (parseOption(arg, "-m", "--max-connections", "[connections]", i, deferredArgc, deferredArgs, connections) ||
				parseOption(arg, "-p", "--pidfile", "[file]", i, deferredArgc, deferredArgs, pidFilename) ||
				parseOption(arg, "-t", "--timeout", "[ms]", i, deferredArgc, deferredArgs, timeout))
			; // Option recognized and parsed
		else if (arg == "-o" || arg == "--overwrite-client-executables")
			overwriteClientExecutables = true;
		else if (arg == "-w" || arg == "--no-wait")
			waitOnClientConnections = false;
		else if (ignoreInvalidArguments)
			LOG_WARNING << "Ignoring unrecognized argument " << arg;
		else
			LOG_ERROR << "Invalid argument " << arg;

		// Update limit to full size after processing all arguments from config file
		if (i + 1 >= deferredArgc)
			deferredArgc = deferredArgs.size();
	}

	// Generate the appropriate client symlinks / executables
	std::string clientCommand = k8psh::Utilities::getExecutablePath();
	const auto commands = configuration.getCommands();
	std::vector<std::string> createdExecutables;
#ifndef _WIN32
	int pidFile = -1;
#endif
	auto cleanup = [&]
	{
		if (!keepClientExecutables)
		{
			for (auto it = createdExecutables.begin(); it != createdExecutables.end(); ++it)
			{
#ifdef _WIN32
				// Hardlinks are used on Windows, so give additional time to the client processes to complete so that the executables can be deleted
				int attemptsRemaining = (it == createdExecutables.begin() ? 8 : 1);

				while (!k8psh::Utilities::deleteFile(*it) && --attemptsRemaining)
					Sleep(16);

				if (!attemptsRemaining)
#else
				if (!k8psh::Utilities::deleteFile(*it))
#endif
					LOG_WARNING << "Failed to remove client executable " << *it;
			}
		}

#ifndef _WIN32
		// Remove PID file
		if (pidFile >= 0 && !k8psh::Utilities::deleteFile(pidFilename.c_str()))
			LOG_WARNING << "Failed to remove PID file " << pidFilename;
#endif
	};

	try
	{
		for (auto it = commands.begin(); it != commands.end(); ++it)
		{
#ifdef _WIN32
			const std::string filename = directory + it->second.getName() + ".exe";
#else
			const std::string filename = directory + it->second.getName();
#endif

			if (disableClientExecutables || overwriteClientExecutables)
				(void)k8psh::Utilities::deleteFile(filename.c_str());

			if (disableClientExecutables || (!generateLocalExecutables && name == it->second.getHost().getHostname()))
				continue;

#ifdef _WIN32
			// Attempt to link or copy executable (hardlinks can't be deleted while in use, so don't hardlink to executable otherwise overwrite client executables option will fail)
			if (CreateSymbolicLinkA(filename.c_str(), clientCommand.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0 ||
					(it != commands.begin() && CreateHardLinkA(filename.c_str(), clientCommand.c_str(), NULL) != 0))
				; // Successfully linked
			else if (CopyFileA(clientCommand.c_str(), filename.c_str(), overwriteClientExecutables ? FALSE : TRUE) != 0)
				clientCommand = filename; // Try to link to copied file from now on
			else
#else
			if (symlink(clientCommand.c_str(), filename.c_str()) != 0)
#endif
				LOG_ERROR << "Failed to create client executable for command " << it->second.getName();

			createdExecutables.emplace_back(filename);
		}

		if (listener.isValid())
		{
			long long connectionCount = 0;
			long long maxConnections = 0;
			long long timeoutMs = 0;

			try { maxConnections = std::stoll(connections); }
			catch (const std::exception &e) { LOG_ERROR << "Failed to parse max connections: " << e.what(); }

			try { timeoutMs = std::stoll(timeout); }
			catch (const std::exception &e) { LOG_ERROR << "Failed to parse timeout (" << timeout << "): " << e.what(); }

			auto endTime = timeoutMs >= 0 ? std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs) : std::chrono::steady_clock::time_point::max();

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
					(void)close(listener.abandon());
					return 0;
				}

				// Create PID file
				if (!pidFilename.empty())
				{
					std::string pid = std::to_string(getpid()) + '\n';
					pidFile = open(pidFilename.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);

					if (pidFile < 0 || write(pidFile, &pid[0], pid.size()) != ssize_t(pid.size()))
						LOG_ERROR << "Failed to write PID file " << pidFilename;

					(void)close(pidFile);
				}

				if (setsid() == -1)
					return -1;

				signal(SIGHUP, SIG_IGN);
				signal(SIGCHLD, SIG_IGN);

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
#ifndef _WIN32
			else
			{
				signal(SIGHUP, handleSignal);
				signal(SIGCHLD, SIG_IGN);
			}
#endif

			signal(SIGTERM, handleSignal);
			signal(SIGINT, handleSignal);

#ifdef _WIN32
			(void)SetConsoleCtrlHandler(handleCtrlC, TRUE);
#endif

			// Main loop
#ifdef _WIN32
			std::thread clientThread;
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

			while (connectionCount < maxConnections || maxConnections < 0)
			{
				auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime -
					(timeoutMs >= 0 ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point::max())).count();
#ifdef _WIN32
				auto waitMs = clampPositive(remainingMs, std::chrono::milliseconds::rep(INFINITE - 1));
				DWORD waitResult = WaitForMultipleObjects(DWORD(sizeof(waitSet) / sizeof(waitSet[0]) - (listener.isValid() ? 0 : 1)), waitSet, FALSE, timeoutMs >= 0 ? DWORD(waitMs) : INFINITE);

				if (waitResult == WAIT_FAILED)
					LOG_ERROR << "Failed to wait on multiple objects for new clients: " << GetLastError();
				else if (waitResult == WAIT_OBJECT_0)
					break;
				else if (waitResult == WAIT_TIMEOUT)
#else
				auto waitMs = clampPositive(remainingMs, std::chrono::milliseconds::rep(unsigned(-1) >> 1));
				int pollResult;

				do pollResult = poll(pollSet, nfds_t(sizeof(pollSet) / sizeof(pollSet[0]) - (listener.isValid() ? 0 : 1)), timeoutMs >= 0 ? int(waitMs) : -1);
				while (pollResult < 0 && (errno == EAGAIN || errno == EINTR));

				if (pollResult < 0 || (pollSet[1].revents & POLLERR) != 0)
					LOG_ERROR << "Failed to poll for new clients: " << errno;
				else if ((pollSet[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
					break;
				else if (pollResult == 0)
#endif
				{
					if (remainingMs <= waitMs)
						break;
					else
						continue;
				}

				if (!listener.isValid())
					continue;

				k8psh::Socket client = listener.accept();

				// Start the child thread / process
				if (client.isValid())
				{
					LOG_DEBUG << "Accepted connection from new client";

#ifdef _WIN32
					clientThread = std::thread(std::bind([&](std::thread &chainedThread, k8psh::Socket &client)
					{
						k8psh::Process::run(configuration.getBaseDirectory(), *serverCommands, client);
						client.close();

						if (chainedThread.joinable())
							chainedThread.join();
					}, std::move(clientThread), std::move(client)));
#else
					pid_t child = fork();

					if (child == 0)
					{
						(void)close(listener.abandon());
						signal(SIGCHLD, SIG_DFL);
						k8psh::Process::run(configuration.getBaseDirectory(), *serverCommands, client);
						return 0;
					}

					(void)close(client.abandon());

					if (child == -1)
						LOG_ERROR << "Failed to fork client: " << errno;
#endif

					connectionCount++;
				}
			}

			LOG_DEBUG << "Shutting down the server, handled " << connectionCount << " connection(s)";

#ifdef _WIN32
			if (waitOnClientConnections && connectionCount)
			{
				LOG_DEBUG << "Waiting for all connections to terminate";
				clientThread.join();
				LOG_DEBUG << "All connections terminated";
			}
			else if (connectionCount)
				clientThread.detach();
#else
			if (waitOnClientConnections && connectionCount)
			{
				int status;

				LOG_DEBUG << "Waiting for all connections to terminate";
				signal(SIGCHLD, SIG_DFL);

				while (wait(&status) >= 0 || errno != ECHILD)
					; // Continue until all children have been reaped

				LOG_DEBUG << "All connections terminated";
			}
#endif
		}
	}
	catch (...)
	{
		cleanup();
		throw;
	}

	cleanup();
	std::exit(0); // Skip local destructors, since some local data may be shared with active threads
}

int main(int argc, const char *argv[])
{
	std::string commandName = k8psh::Utilities::getExecutableBasename(argv[0]);

	if (commandName == serverName)
		return mainServer(argc, argv);
	else
		return mainClient(argc, argv);
}
