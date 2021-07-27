// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#include "Process.hxx"

#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "Socket.hxx"
#include "Utilities.hxx"

#ifdef _WIN32
	#include <condition_variable>
	#include <mutex>
	#include <thread>

	#include <fcntl.h>
	#include <io.h>
	#include <windows.h>
#else
	#include <cstdlib>

	#include <poll.h>
	#include <sys/wait.h>
	#include <unistd.h>

	#ifdef __APPLE__
		#include <crt_externs.h>

		#define environ (*_NSGetEnviron())
	#endif
#endif

/**
 * Payloads are 5 bytes: 1-byte payload type, and if strings (4 byte little-endian length prefixed), exit code (4 byte little-endian signed int), or 4 byte zeros
 */
enum PayloadType {
	WORKING_DIRECTORY = 0, // string, client -> server
	ENVIRONMENT_VARIABLE,  // string, client -> server
	COMMAND_ARGUMENT,      // string, client -> server

	START_COMMAND,         // string - argv[0], client -> server (nothing above can be sent after this, nothing below can be sent before this)

	STDIN_DATA,            // string, client <-> server (server to client always zeros and indicates close)
	STDOUT_DATA,           // string, server -> client
	STDERR_DATA,           // string, server -> client
	TERMINATE_COMMAND,     // zeros, client -> server
	EXIT_CODE              // exit code, server -> client
};

static const std::size_t DATA_BUFFER_SIZE = 1024 * 64;

#ifdef _WIN32
class SynchronousData
{
	// Disable copying synchronous data
	SynchronousData(const SynchronousData&);

public:
	std::string _buffer;
	DWORD _length;
	HANDLE _dataAvailable;
	HANDLE _dataRead;

	SynchronousData(const std::string &name) : _dataAvailable(), _dataRead()
	{
		_buffer.resize(DATA_BUFFER_SIZE - 1);
		_dataAvailable = CreateEventA(NULL, FALSE, FALSE, (name + "Available").c_str());

		if (!_dataAvailable)
			LOG_ERROR << "Failed to create " << (name + "Available") << " event: " << GetLastError();

		_dataRead = CreateEventA(NULL, FALSE, FALSE, (name + "Read").c_str());

		if (!_dataRead)
			LOG_ERROR << "Failed to create " << (name + "Read") << " event: " << GetLastError();
	}

	~SynchronousData()
	{
		(void)CloseHandle(_dataRead);
		(void)CloseHandle(_dataAvailable);
	}
};

// Reads data from a handle
static void readData(HANDLE handle, SynchronousData &data)
{
	while (ReadFile(handle, &data._buffer[0], DWORD(data._buffer.length()), &data._length, NULL) != 0 && data._length > 0)
	{
		LOG_DEBUG << "Read " << data._length << " bytes on handle " << handle;
		SetEvent(data._dataAvailable);

		// Wait until the buffer has been consumed before reading more data
		if (WaitForSingleObject(data._dataRead, INFINITE) != WAIT_OBJECT_0)
			return;
	}

	data._length = 0;
	SetEvent(data._dataAvailable);
}
#endif

// Gets the length of the payload's string
static std::size_t getPayloadStringLength(const std::vector<std::uint8_t> &socketData)
{
	return std::size_t(socketData[1]) + (std::size_t(socketData[2]) << 8) + (std::size_t(socketData[3]) << 16) + (std::size_t(socketData[4]) << 24);
}

// Sends a payload and a string
static void sendSocketPayloadString(k8psh::Socket &socket, PayloadType type, const std::string &string, std::size_t length)
{
	std::vector<std::uint8_t> data = { std::uint8_t(type), std::uint8_t(length), std::uint8_t(length >> 8), std::uint8_t(length >> 16), std::uint8_t(length >> 24) };

	data.insert(data.end(), string.data(), string.data() + length);

	if (socket.write(data) != data.size())
		LOG_ERROR << "Failed to write data to socket, payload type " << type;
}

// Sends a payload and a string
static void sendSocketPayloadString(k8psh::Socket &socket, PayloadType type, const std::string &string)
{
	sendSocketPayloadString(socket, type, string, string.length());
}

/** Runs a process remotely on the configured host.
 *
 * @param workingDirectory the relative working directory used to start the process
 * @param command the command to start
 * @param argc the number of additional arguments used to start the process
 * @param argv additional arguments used to start the process
 * @return the exit code of the process
 */
int k8psh::Process::runRemoteCommand(const std::string &workingDirectory, const k8psh::Configuration::Command &command, int argc, const char *argv[])
{
	k8psh::Socket::Initializer socketInit;
	Socket socket;

	// Connect to the server
	while (!socket.isValid())
		socket = Socket::connect(command.getHost().getPort());

	// Build the process information (working directory, environment variables, command line)
	LOG_DEBUG << "Sending working directory (\"" << workingDirectory << "\") to server";
	sendSocketPayloadString(socket, WORKING_DIRECTORY, workingDirectory);

	for (int i = 1; i < argc; i++)
	{
		LOG_DEBUG << "Sending argument (\"" << argv[i] << "\") to server";
		sendSocketPayloadString(socket, COMMAND_ARGUMENT, argv[i]);
	}

	for (auto it = command.getEnvironmentVariables().begin(); it != command.getEnvironmentVariables().end(); ++it)
	{
		const char first = (*it)[0];

		if (first != '=')
		{
			const std::string name = first == '?' ? it->substr(1) : *it;
			const auto value = Utilities::getEnvironmentVariable(name);

			if (value)
			{
				LOG_DEBUG << "Sending environment variable (\"" << (name + "=" + value) << "\") to server";
				sendSocketPayloadString(socket, ENVIRONMENT_VARIABLE, name + "=" + value);
			}
		}
	}

	LOG_DEBUG << "Sending start command (\"" << argv[0] << "\") to server";
	sendSocketPayloadString(socket, START_COMMAND, argv[0]);

	// Process stdin, socket data, and wait for exit code
#ifdef _WIN32
	HANDLE waitSet[2];
	SynchronousData stdInData("StdInData");

	waitSet[0] = stdInData._dataAvailable;

	(void)_setmode(_fileno(stdin), _O_BINARY);
	(void)_setmode(_fileno(stdout), _O_BINARY);
	(void)_setmode(_fileno(stderr), _O_BINARY);
	std::thread(readData, GetStdHandle(STD_INPUT_HANDLE), std::ref(stdInData)).detach();

	waitSet[1] = socket.createReadEvent();
#else
	struct pollfd pollSet[2] = { };

	pollSet[0].fd = STDIN_FILENO;
	pollSet[0].events = POLLIN;

	pollSet[1].fd = socket.createReadEvent();
	pollSet[1].events = POLLIN;
#endif

	std::string stdInBuffer(DATA_BUFFER_SIZE - 1, '\0');

	std::vector<std::uint8_t> socketData;
	socketData.resize(5);

	// Wait for and process data from stdin and the socket
	for (;;)
	{
#ifdef _WIN32
		// Check for new data
		DWORD waitResult = WaitForMultipleObjects(DWORD(sizeof(waitSet) / sizeof(waitSet[0])), waitSet, FALSE, INFINITE);

		if (waitResult == WAIT_FAILED)
			LOG_ERROR << "Failed to wait on multiple objects for new data: " << GetLastError();
#else
		int pollResult;

		do pollResult = poll(pollSet, nfds_t(sizeof(pollSet) / sizeof(pollSet[0])), -1);
		while (pollResult < 0 && (errno == EAGAIN || errno == EINTR));

		if (pollResult < 0)
			LOG_ERROR << "Failed to poll for new data: " << errno;
#endif

		// Check for stdin data
#ifdef _WIN32
		else if (waitResult == WAIT_OBJECT_0)
#else
		if ((pollSet[0].revents & POLLERR) != 0)
			LOG_ERROR << "Failed to poll data from stdin";
		else if ((pollSet[0].revents & (POLLIN | POLLHUP)) != 0)
#endif
		{
#ifdef _WIN32
			std::size_t received = stdInData._length;

			stdInBuffer.swap(stdInData._buffer);
			SetEvent(stdInData._dataRead);

			LOG_DEBUG << "Sending stdin data (" << received << " bytes) to server";

			if (received > 0)
				sendSocketPayloadString(socket, STDIN_DATA, stdInBuffer, received);
			else
				socket.write({ std::uint8_t(STDIN_DATA), 0, 0, 0, 0 });
#else
			ssize_t received = read(pollSet[0].fd, &stdInBuffer[0], stdInBuffer.size());

			LOG_DEBUG << "Sending stdin data (" << received << " bytes) to server";

			if (received > 0)
				sendSocketPayloadString(socket, STDIN_DATA, stdInBuffer, received);
			else if (received == 0)
			{
				pollSet[0].fd = -1; // No more data, so ignore it from now on
				socket.write({ std::uint8_t(STDIN_DATA), 0, 0, 0, 0 });
			}
			else
				LOG_ERROR << "Failed to read data from stdin: " << errno;
#endif
		}

		// Check for socket data
#ifdef _WIN32
		else if (waitResult == WAIT_OBJECT_0 + 1)
#else
		if ((pollSet[1].revents & POLLERR) != 0)
			LOG_ERROR << "Failed to poll data from socket";
		else if ((pollSet[1].revents & (POLLIN | POLLHUP)) != 0)
#endif
		{
			while (socket.hasData())
			{
				std::size_t readLength = socket.read(socketData, 0, true);

				if (readLength == 0) // Closed socket indicates abnormal termination
				{
					LOG_DEBUG << "Socket has been closed without exit code, aborting";
					std::abort();
				}
				else if (readLength != socketData.size())
					LOG_ERROR << "Failed to read data from socket";

				switch (socketData[0])
				{
				case STDIN_DATA:
					LOG_DEBUG << "Received stdin close command from server";
#ifdef _WIN32
					(void)CloseHandle(GetStdHandle(STD_INPUT_HANDLE));
#else
					(void)close(STDIN_FILENO);
#endif
					break;

				case STDOUT_DATA:
					LOG_DEBUG << "Received stdout data (" << getPayloadStringLength(socketData) << " bytes) from server";
					std::cout << socket.read(getPayloadStringLength(socketData));
					break;

				case STDERR_DATA:
					LOG_DEBUG << "Received stderr data (" << getPayloadStringLength(socketData) << " bytes) from server";
					std::cerr << socket.read(getPayloadStringLength(socketData));
					break;

				case EXIT_CODE:
					LOG_DEBUG << "Received exit code (" << int(socketData[1]) + (int(socketData[2]) << 8) + (int(socketData[3]) << 16) + (int(socketData[4]) << 24) << ") from server";
					return int(socketData[1]) + (int(socketData[2]) << 8) + (int(socketData[3]) << 16) + (int(socketData[4]) << 24);

				default:
					LOG_ERROR << "Read invalid payload type (" << socketData[0] << ") from socket";
				}
			}
		}
	}
}

#ifdef _WIN32
static std::mutex workingDirectoryMutex;
#endif

/** Starts the process requested by the remote socket channel. This call is non-blocking and will spawn a new thread or process to handle communications on the socket. The socket does not need to be closed.
 *
 * @param workingDirectory the relative working directory used to start the process
 * @param commands the map of commands for this server node
 * @param socket the open socket used to communicate with the client
 */
void k8psh::Process::start(const std::string &workingDirectory, const k8psh::Configuration::CommandMap &commands, k8psh::Socket socket)
{
#ifdef _WIN32
	HANDLE process = HANDLE();

	try
#endif
	{
		// Read all the data for the command to execute
		std::string processDirectory;
		std::unordered_map<std::string, std::string> environmentVariables;
		std::vector<std::string> arguments;
		std::string commandName;
		std::vector<std::uint8_t> socketData;
		socketData.resize(5);

		while (socketData[0] != START_COMMAND)
		{
			if (socket.read(socketData, 0, true) != socketData.size())
				LOG_ERROR << "Failed to read data from socket";

			switch (socketData[0])
			{
			case WORKING_DIRECTORY:
				{
					std::string newDirectory = socket.read(getPayloadStringLength(socketData));
					processDirectory = (workingDirectory.empty() ? std::string() : workingDirectory + "/") + newDirectory;
					LOG_DEBUG << "Received working directory (\"" << newDirectory << "\") from client, process directory set to \"" << processDirectory << "\"";
					break;
				}

			case ENVIRONMENT_VARIABLE:
				{
					std::string environmentVariable = socket.read(getPayloadStringLength(socketData));
					LOG_DEBUG << "Received environment variable (\"" << environmentVariable << "\") from client";
					std::size_t equals = environmentVariable.find("=");

					if (equals == std::string::npos)
						environmentVariables.emplace(std::make_pair(environmentVariable, Utilities::getEnvironmentVariable(environmentVariable)));
					else
					{
						std::string newValue = Utilities::substituteEnvironmentVariables(environmentVariable.substr(equals + 1), environmentVariables);
						environmentVariables[environmentVariable.substr(0, equals)] = std::move(newValue);
					}

					break;
				}

			case COMMAND_ARGUMENT:
				arguments.push_back(socket.read(getPayloadStringLength(socketData)));
				LOG_DEBUG << "Received command argument (\"" << arguments.back() << "\") from client";
				break;

			case START_COMMAND:
				commandName = socket.read(getPayloadStringLength(socketData));
				LOG_DEBUG << "Received start command (\"" << commandName << "\") from client";
				break;

			default:
				LOG_ERROR << "Read invalid payload type (" << socketData[0] << ") from socket";
			}
		}

		auto commandIt = commands.find(commandName);

		if (commandIt == commands.end())
			LOG_ERROR << "Failed to find command \"" << commandName << "\" in configuration";

		// Build the process
		auto command = commandIt->second;
		std::vector<std::string> environment;

		for (auto it = command.getEnvironmentVariables().begin(); it != command.getEnvironmentVariables().end(); ++it)
		{
			// Optional environment variable
			if ((*it)[0] == '?')
			{
				const std::string name = it->substr(1);
				auto valueIt = environmentVariables.find(name);

				if (valueIt != environmentVariables.end())
					environment.emplace_back(name + '=' + valueIt->second);
				else
				{
					const auto value = Utilities::getEnvironmentVariable(name);

					if (value)
						environment.emplace_back(name + "=" + value);
				}
			}

			// Inherited environment variable
			else if ((*it)[0] == '=')
			{
				const std::string name = it->substr(1);
				const auto value = Utilities::getEnvironmentVariable(name);

				if (value)
					environment.emplace_back(name + "=" + value);
			}

			// Required environment variable
			else
			{
				auto valueIt = environmentVariables.find(*it);

				if (valueIt != environmentVariables.end())
					environment.emplace_back(*it + '=' + valueIt->second);
			}
		}

		arguments.insert(arguments.begin(), command.getExecutable().begin(), command.getExecutable().end());

		Pipe stdInPipe;
		Pipe stdOutPipe;
		Pipe stdErrPipe;

#ifdef _WIN32
		{
			std::string env;
			std::string cmd;
			STARTUPINFOA si = { };
			PROCESS_INFORMATION pi;

			env.reserve(1024 * 32);
			cmd.reserve(1024 * 32);

			for (auto it = environment.begin(); it != environment.end(); ++it)
				env.append(it->c_str()) += '\0';

			env += '\0';

			// Create the command line string (the rules here are very interesting)
			for (size_t i = 0; i < arguments.size(); i++)
			{
				const char *arg = arguments[i].c_str();
				bool quoted = false;

				// Arguments are separated with spaces
				if (i)
					cmd += ' ';

				// Empty arguments are in double quotes
				if (!*arg)
				{
					cmd += "\"\"";
					continue;
				}

				for (const char *s = arg; *s; s++)
				{
					// A double quote has to be escaped with a backslash
					// Spaces and tabs require that they be wrapped in quotes, which implies that all preceding backslashes must be escaped
					if (*s == '"' || ((*s == ' ' || *s == '\t') && !quoted))
					{
						// Backslashes immediately preceding a double quote have to be escaped with an additional backslash
						for (const char *j = s - 1; j >= arg && (*j == '\\' || (i == 0 && *j == '/')); j--)
							cmd += '\\';

						if (*s == '"')
							cmd += "\\\"";
						else
						{
							cmd += '"';
							cmd += *s;
							quoted = true;
						}
					}
					// A forward slash in the command name has to be converted to a backslash
					else if (i == 0 && *s == '/')
						cmd += '\\';
					// All other characters (including backslashes) are treated like normal characters
					else
						cmd += *s;
				}

				if (quoted)
					cmd += '"';
			}

			si.cb = DWORD(sizeof(si));
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdInput = stdInPipe.getOutput();
			si.hStdOutput = stdOutPipe.getInput();
			si.hStdError = stdErrPipe.getInput();

			(void)SetHandleInformation(stdInPipe.getInput(), HANDLE_FLAG_INHERIT, 0);
			(void)SetHandleInformation(stdOutPipe.getOutput(), HANDLE_FLAG_INHERIT, 0);
			(void)SetHandleInformation(stdErrPipe.getOutput(), HANDLE_FLAG_INHERIT, 0);

			std::unique_lock<std::mutex> lock(workingDirectoryMutex);
			std::string oldDirectory = Utilities::getWorkingDirectory();

			if (!Utilities::changeWorkingDirectory(processDirectory))
				LOG_ERROR << "Failed to change directory to " << processDirectory;

			LOG_DEBUG << "Starting " << cmd << ",\n" <<
				"  working directory: \"" << Utilities::getWorkingDirectory() << "\",\n" <<
				"  environment: \"" << [](std::string value)
					{
						if (value.length() < 2)
							return std::string();

						for (std::size_t i = value.find('\0'); i < value.length() - 2; i = value.find('\0', i + 4))
							value.replace(i, 1, "\", \"");

						value.resize(value.length() - 2);
						return value;
					}(env) << "\"";

			if (CreateProcessA(NULL, &cmd[0], NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP, &env[0], NULL, &si, &pi) == 0)
				LOG_ERROR << "Failed to start " << arguments[0] << ": error " << GetLastError();

			(void)Utilities::changeWorkingDirectory(oldDirectory);
			lock.unlock();
			process = pi.hProcess;
			(void)CloseHandle(pi.hThread);
		}
#else
		int process = fork();

		switch (process)
		{
		case 0:
			{
				// Setup the environment
				std::vector<const char *> env;
				std::vector<const char *> argv;

				for (auto it = environment.begin(); it != environment.end(); ++it)
					env.push_back(it->c_str());

				env.push_back(NULL);
				environ = const_cast<char **>(env.data());

				for (auto it = arguments.begin(); it != arguments.end(); ++it)
					argv.push_back(it->c_str());

				argv.push_back(NULL);

				if (!Utilities::changeWorkingDirectory(processDirectory))
					LOG_ERROR << "Failed to change directory to " << processDirectory;

				stdInPipe.closeInput();
				stdOutPipe.closeOutput();
				stdErrPipe.closeOutput();

				auto concat = [](const std::vector<const char *> &list, bool quotes)
					{
						std::string value;

						for (std::size_t i = 0; i < list.size() - 1; i++)
						{
							if (i)
								value.append(quotes ? "\", \"" : " ");

							if (!quotes)
							{
								const char *s = list[i];

								while (*s && !Utilities::isWhitespace(*s))
									s++;

								if (*s)
								{
									value.append("\"").append(list[i]).append("\"");
									continue;
								}
							}

							value.append(list[i]);
						}

						return value;
					};
				LOG_DEBUG << "Starting " << concat(argv, false) << ",\n" <<
					"  working directory: \"" << Utilities::getWorkingDirectory() << "\",\n" <<
					"  environment: \"" << concat(env, true) << "\"";

				if (!stdInPipe.remapOutput(STDIN_FILENO) || !stdOutPipe.remapInput(STDOUT_FILENO) || !stdErrPipe.remapInput(STDERR_FILENO))
					LOG_ERROR << "Failed to map stdin, stdout, and stderr";

				// First, try the working directory (this provides compatibility across different platforms), then try the search path
				(void)execv(argv[0], (char * const *)argv.data());
				(void)execvp(argv[0], (char * const *)argv.data());

				LOG_ERROR << "Failed to start " << arguments[0] << ": error " << errno;
			}

		case -1:
			LOG_ERROR << "Failed to fork process";

		default:
			signal(SIGPIPE, SIG_IGN);
			break;
		}
#endif

		stdInPipe.closeOutput();
		stdOutPipe.closeInput();
		stdErrPipe.closeInput();

		// Process stdin, stdout, stderr
		bool processHasExited = false;
		std::string stdInData;
		bool closeStdIn = false;
		std::string pipeData(DATA_BUFFER_SIZE, '\0');

		if (!stdInPipe.setInputNonblocking())
			LOG_ERROR << "Failed to set stdin to use non-blocking writes";

#ifdef _WIN32
		HANDLE waitSet[4];
		SynchronousData stdOutData("StdOutData");
		SynchronousData stdErrData("StdErrData");

		waitSet[0] = stdOutData._dataAvailable;
		std::thread(readData, stdOutPipe.getOutput(), std::ref(stdOutData)).detach();

		waitSet[1] = stdErrData._dataAvailable;
		std::thread(readData, stdErrPipe.getOutput(), std::ref(stdErrData)).detach();

		waitSet[2] = socket.createReadEvent();
		waitSet[3] = process;
#else
		int exitStatus = 0;
		struct pollfd pollSet[4] = { };

		pollSet[0].events = POLLOUT;
		pollSet[1].events = POLLIN;
		pollSet[2].events = POLLIN;

		pollSet[3].fd = socket.createReadEvent();
		pollSet[3].events = POLLIN;
#endif

		while (stdInPipe.getInput() != Pipe::INVALID_HANDLE || stdOutPipe.getOutput() != Pipe::INVALID_HANDLE || stdErrPipe.getOutput() != Pipe::INVALID_HANDLE)
		{
			// Check for new data
#ifdef _WIN32
			DWORD waitResult = WaitForMultipleObjects(DWORD(sizeof(waitSet) / sizeof(waitSet[0])), waitSet, FALSE, stdInData.empty() ? INFINITE : 16);

			if (waitResult == WAIT_FAILED)
				LOG_ERROR << "Failed to wait on multiple objects for new data: " << GetLastError();
			else if (waitResult == WAIT_OBJECT_0 + 3)
#else
			if (!processHasExited && waitpid(process, &exitStatus, WNOHANG) > 0 && (WIFEXITED(exitStatus) || WIFSIGNALED(exitStatus)))
#endif
			{
				LOG_DEBUG << "Process terminated, closing stdin, transfering remaining stdout and stderr data";

				stdInData.clear();
				stdInPipe.closeInput();
				processHasExited = true;
			}

#ifndef _WIN32
			int pollResult;

			pollSet[0].fd = stdInData.empty() ? Pipe::INVALID_HANDLE : stdInPipe.getInput();
			pollSet[1].fd = stdOutPipe.getOutput();
			pollSet[2].fd = stdErrPipe.getOutput();

			do pollResult = poll(pollSet, nfds_t(sizeof(pollSet) / sizeof(pollSet[0])), 250);
			while (pollResult < 0 && (errno == EAGAIN || errno == EINTR));

			if (pollResult < 0)
				LOG_ERROR << "Failed to poll for new data: " << errno;
#endif

			// Check for stdout data
#ifdef _WIN32
			else if (waitResult == WAIT_OBJECT_0)
#else
			if ((pollSet[1].revents & POLLERR) != 0)
				LOG_ERROR << "Failed to poll data from stdout";
			else if ((pollSet[1].revents & (POLLIN | POLLHUP)) != 0)
#endif
			{
#ifdef _WIN32
				std::size_t received = stdOutData._length;

				pipeData.swap(stdOutData._buffer);
				SetEvent(stdOutData._dataRead);
#else
				std::size_t received = stdOutPipe.read(pipeData);
#endif

				LOG_DEBUG << "Sending stdout data (" << received << " bytes) to client";

				if (received > 0)
					sendSocketPayloadString(socket, STDOUT_DATA, pipeData, received);
				else
				{
					socket.write({ std::uint8_t(STDOUT_DATA), 0, 0, 0, 0 });
					stdOutPipe.closeOutput();
				}
			}

			// Check for stderr data
#ifdef _WIN32
			else if (waitResult == WAIT_OBJECT_0 + 1)
#else
			if ((pollSet[2].revents & POLLERR) != 0)
				LOG_ERROR << "Failed to poll data from stderr";
			else if ((pollSet[2].revents & (POLLIN | POLLHUP)) != 0)
#endif
			{
#ifdef _WIN32
				std::size_t received = stdErrData._length;

				pipeData.swap(stdErrData._buffer);
				SetEvent(stdErrData._dataRead);
#else
				std::size_t received = stdErrPipe.read(pipeData);
#endif

				LOG_DEBUG << "Sending stderr data (" << received << " bytes) to client";

				if (received > 0)
					sendSocketPayloadString(socket, STDERR_DATA, pipeData, received);
				else
				{
					socket.write({ std::uint8_t(STDERR_DATA), 0, 0, 0, 0 });
					stdErrPipe.closeOutput();
				}
			}

			// Check for socket data
#ifdef _WIN32
			else if (waitResult == WAIT_OBJECT_0 + 2)
#else
			if ((pollSet[3].revents & POLLERR) != 0)
				LOG_ERROR << "Failed to poll data from socket";
			else if ((pollSet[3].revents & (POLLIN | POLLHUP)) != 0)
#endif
			{
				while (socket.hasData())
				{
					std::size_t readLength = socket.read(socketData, 0, true);

					if (readLength == 0) // Closed socket indicates abnormal termination
						socketData[0] = std::uint8_t(TERMINATE_COMMAND);
					else if (readLength != socketData.size())
						LOG_ERROR << "Failed to read data from socket";

					switch (socketData[0])
					{
					case STDIN_DATA:
						{
							LOG_DEBUG << "Received stdin data (" << getPayloadStringLength(socketData) << " bytes) from client";
							std::string data = socket.read(getPayloadStringLength(socketData));

							if (stdInPipe.getInput() == Pipe::INVALID_HANDLE)
								LOG_DEBUG << "Ignoring " << data.length() << " received bytes due to closed stdin"; // Ignore data on stdin after close
							else if (data.empty())
								closeStdIn = true;
							else
								stdInData += data;

							break;
						}

					default:
#ifdef _WIN32
						(void)TerminateProcess(process, UINT(-1));
#else
						(void)kill(process, 15); // Send TERM
#endif

						if (readLength == 0)
							LOG_ERROR << "Socket was closed unexpectedly";
						else if (socketData[0] == TERMINATE_COMMAND)
							LOG_DEBUG << "Received terminate command from client, halting process";
						else
							LOG_ERROR << "Read invalid payload type (" << socketData[0] << ") from socket";

						goto terminateServer;
					}
				}
			}

			// Handle stdin data out-of-band, so we don't end up in a pipe-write deadlock
			if (!stdInData.empty())
			{
				(void)stdInData.erase(0, stdInPipe.write(stdInData));

				// Check if the pipe has been closed
				if (stdInPipe.getInput() == Pipe::INVALID_HANDLE)
				{
					LOG_DEBUG << "Process has closed stdin, lost " << stdInData.length() << " bytes";
					stdInData.clear();
					socket.write({ std::uint8_t(STDIN_DATA), 0, 0, 0, 0 });
				}
			}
			else if (closeStdIn)
			{
				LOG_DEBUG << "Closing stdin";
				closeStdIn = false;
				stdInPipe.closeInput();
			}
		}

		// Wait for application to exit
		if (!processHasExited)
			LOG_DEBUG << "Waiting for process to terminate";

#ifdef _WIN32
		if (processHasExited || WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0)
		{
			DWORD exitCode = 0;

			if (GetExitCodeProcess(process, &exitCode) != 0)
			{
				LOG_DEBUG << "Sending exit code (" << exitCode << ") to client";
				socket.write({ std::uint8_t(EXIT_CODE), std::uint8_t(exitCode), std::uint8_t(exitCode >> 8), std::uint8_t(exitCode >> 16), std::uint8_t(exitCode >> 24) });
			}
		}
#else
		if (!processHasExited)
		{
			pid_t pid = 0;

			while (pid <= 0 || !(WIFEXITED(exitStatus) || WIFSIGNALED(exitStatus)))
			{
				pid = waitpid(process, &exitStatus, 0);

				if (pid < 0 && errno != EINTR)
					LOG_ERROR << "Failed to get exit code of process: " << errno;
			}
		}

		if (WIFEXITED(exitStatus)) // Only send exit code on normal termination
		{
			int exitCode = WEXITSTATUS(exitStatus);

			LOG_DEBUG << "Sending exit code (" << exitCode << ") to client";
			socket.write({ std::uint8_t(EXIT_CODE), std::uint8_t(exitCode), std::uint8_t(exitCode >> 8), std::uint8_t(exitCode >> 16), std::uint8_t(exitCode >> 24) });
		}
#endif

terminateServer:
		;
	}
#ifdef _WIN32
	catch (...) { }

	// Clean up all resources, since we are part of the long-running server process
	(void)CloseHandle(process);
#else
	socket.close();
	exit(0);
#endif
}
