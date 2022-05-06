// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#include "Process.hxx"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <ios>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Socket.hxx"
#include "Utilities.hxx"

#ifdef _WIN32
	#include <condition_variable>
	#include <mutex>

	#include <fcntl.h>
	#include <io.h>
	#include <windows.h>

	// Due to using events to poll for socket input on Windows (which are susceptible to race conditions), the socket must be checked for new data on the first pass
	#define K8PSH_SKIP_FIRST_SOCKET_DATA_CHECK false
#else
	#include <cstdlib>

	#include <poll.h>
	#include <sys/wait.h>
	#include <unistd.h>

	#ifdef __APPLE__
		#include <crt_externs.h>

		#define environ (*_NSGetEnviron())
	#endif

	#define K8PSH_SKIP_FIRST_SOCKET_DATA_CHECK true
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

static const std::size_t INITIAL_MTU_SIZE = 8192; // The loopback default MTU sizes are usually fairly large

class BufferedReceiveSocket
{
	k8psh::Socket &_socket;
	std::vector<std::uint8_t> _data;
	std::size_t _readOffset;
	std::size_t _end;

public:
	BufferedReceiveSocket(k8psh::Socket &socket) : _socket(socket), _data(INITIAL_MTU_SIZE), _readOffset(), _end() { }

	// Checks if the buffer has data
	bool hasBufferedData() const { return _readOffset != _end; }

	// Checks if the socket has data
	bool hasData() const { return hasBufferedData() || _socket.hasData(); }

	// Reads a payload type and value from the socket, returning true if the data was read and false if the socket is closed
	bool read(PayloadType &type, std::uint32_t &value)
	{
		if (_end - _readOffset < 5)
		{
			for (std::size_t i = 0; i < _end - _readOffset; i++)
				_data[i] = _data[_readOffset + i];

			_end -= _readOffset;
			_readOffset = 0;

			do
			{
				std::size_t read = _socket.read(_data, _end);

				if (!read)
					return false;

				_end += read;
			} while (_end < 5);
		}

		type = PayloadType(_data[_readOffset++]);
		value = std::uint32_t(_data[_readOffset]) + (std::uint32_t(_data[_readOffset + 1]) << 8) + (std::uint32_t(_data[_readOffset + 2]) << 16) + (std::uint32_t(_data[_readOffset + 3]) << 24);
		_readOffset += 4;
		return true;
	}

	// Reads a string from the socket
	std::string readString(std::size_t length)
	{
		std::string string;
		string.reserve(length);

		while (_end - _readOffset < length - string.length())
		{
			string.append(reinterpret_cast<const char *>(&_data[_readOffset]), _end - _readOffset);
			_readOffset = 0;
			_end = _socket.read(_data);

			if (!_end)
				LOG_ERROR << "Socket closed after reading " << string.length() << " of " << length << " bytes";
		}

		std::size_t remaining = length - string.length();
		string.append(reinterpret_cast<const char *>(&_data[_readOffset]), remaining);
		_readOffset += remaining;

		if (_readOffset == _end)
			_readOffset = _end = 0;

		return string;
	}
};

class BufferedSendSocket
{
	k8psh::Socket &_socket;
	std::vector<std::uint8_t> _data;
	std::size_t _end;

public:
	BufferedSendSocket(k8psh::Socket &socket) : _socket(socket), _data(INITIAL_MTU_SIZE), _end() { }
	~BufferedSendSocket() { flush(); }

	// Flushes any pending data to the socket
	void flush()
	{
		if (_end)
		{
			if (_socket.write(_data, 0, _end) != _end)
				LOG_ERROR << "Failed to write data to socket";

			_end = 0;
		}
	}

	// Writes a payload to the socket
	void write(PayloadType type, const std::string &string, std::size_t length, bool flush = true)
	{
		// If there is not enough room in the buffer, then flush and resize the buffer if needed
		if (5 + length > _data.size() - _end)
		{
			this->flush();

			if (5 + length > _data.size())
				_data.resize(5 + length);
		}

		_data[_end++] = std::uint8_t(type);
		_data[_end++] = std::uint8_t(length);
		_data[_end++] = std::uint8_t(length >> 8);
		_data[_end++] = std::uint8_t(length >> 16);
		_data[_end++] = std::uint8_t(length >> 24);

		for (std::size_t i = 0; i < length; i++)
			_data[_end++] = std::uint8_t(string[i]);

		if (flush)
			this->flush();
	}

	// Writes a payload to the socket
	void write(PayloadType type, const std::string &string, bool flush = true) { write(type, string, string.length(), flush); }
};

// Outputs data from a socket to a standard stream and handles closing the stream when the length is 0
static void outputStdStreamData(std::ostream &stream, FILE *file, const char *name, BufferedReceiveSocket &receiveSocket, std::size_t length)
{
	if (stream.eof() && length)
		LOG_ERROR << "Unexpected " << name << " data (" << length << " bytes) from server, stream already closed";
	else if (length)
	{
		LOG_DEBUG << "Received " << name << " data (" << length << " bytes) from server";
		std::string data = receiveSocket.readString(length);

		if (!std::fwrite(data.data(), data.length(), 1, file))
			LOG_ERROR << "Failed to write " << name << " data";
	}
	else if (!stream.eof())
	{
		LOG_DEBUG << "Received " << name << " close command from server";
		stream.flush();
		stream.setstate(std::ios_base::eofbit);
		(void)std::fclose(file);
	}
}

/** Runs a process remotely on the configured host.
 *
 * @param workingDirectory the relative working directory used to start the process
 * @param command the command to start
 * @param argc the number of additional arguments used to start the process
 * @param argv additional arguments used to start the process
 * @param configuration the global configuration
 * @return the exit code of the process
 */
int k8psh::Process::runRemoteCommand(const std::string &workingDirectory, const k8psh::Configuration::Command &command, std::size_t argc, const char *argv[], const k8psh::Configuration &configuration)
{
	k8psh::Socket::Initializer socketInit;
	Socket socket;
	BufferedSendSocket sendSocket(socket);
	BufferedReceiveSocket receiveSocket(socket);
	auto endTime = configuration.getConnectTimeoutMs() >= 0 ? std::chrono::steady_clock::now() + std::chrono::milliseconds(configuration.getConnectTimeoutMs()) : std::chrono::steady_clock::time_point::max();
	std::chrono::milliseconds backoff = std::chrono::milliseconds(16);

	// Connect to the server
	while (!(socket = Socket::connect(command.getHost().getPort(), false)).isValid())
	{
		auto now = std::chrono::steady_clock::now();

		if (now >= endTime)
			break;

		backoff = backoff * 2;

		if (backoff > std::chrono::milliseconds(1000))
			backoff = std::chrono::milliseconds(1000);

		auto sleepUntil = now + backoff;
		std::this_thread::sleep_until(sleepUntil < endTime ? sleepUntil : endTime);
	}

	if (!socket.isValid())
		LOG_ERROR << "Failed to connect to server after " << configuration.getConnectTimeoutMs() << "ms";

	// Build the process information (working directory, environment variables, command line)
	LOG_DEBUG << "Sending working directory (\"" << workingDirectory << "\") to server";
	sendSocket.write(WORKING_DIRECTORY, workingDirectory, false);

	for (std::size_t i = 0; i < argc; i++)
	{
		LOG_DEBUG << "Sending argument (\"" << argv[i] << "\") to server";
		sendSocket.write(COMMAND_ARGUMENT, argv[i], false);
	}

	for (auto it = command.getEnvironmentVariables().begin(); it != command.getEnvironmentVariables().end(); ++it)
	{
		const char first = it->first[0];

		if (first != '=')
		{
			const std::string name = first == '?' ? it->first.substr(1) : it->first;
			const auto value = Utilities::getEnvironmentVariable(name);

			if (value)
			{
				LOG_DEBUG << "Sending environment variable (\"" << (name + "=" + value) << "\") to server";
				sendSocket.write(ENVIRONMENT_VARIABLE, name + "=" + value, false);
			}
		}
	}

	LOG_DEBUG << "Sending start command (\"" << command.getName() << "\") to server";
	sendSocket.write(START_COMMAND, command.getName());

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

	// Wait for and process data from stdin and the socket
	for (;;)
	{
#ifdef _WIN32
		// Check for new data
		DWORD waitResult = WaitForMultipleObjects(DWORD(sizeof(waitSet) / sizeof(waitSet[0])), waitSet, FALSE, receiveSocket.hasBufferedData() ? 0 : INFINITE);

		if (waitResult == WAIT_FAILED)
			LOG_ERROR << "Failed to wait on multiple objects for new data: " << GetLastError();
#else
		int pollResult;

		do pollResult = poll(pollSet, nfds_t(sizeof(pollSet) / sizeof(pollSet[0])), receiveSocket.hasBufferedData() ? 0 : -1);
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
			sendSocket.write(STDIN_DATA, stdInBuffer, received);
#else
			ssize_t received = read(pollSet[0].fd, &stdInBuffer[0], stdInBuffer.size());

			if (received < 0)
				LOG_ERROR << "Failed to read data from stdin: " << errno;
			else if (received == 0)
				pollSet[0].fd = -1; // No more data, so ignore it from now on

			LOG_DEBUG << "Sending stdin data (" << received << " bytes) to server";
			sendSocket.write(STDIN_DATA, stdInBuffer, std::size_t(received));
#endif
		}

		// Check for socket data
#ifdef _WIN32
		if (waitResult == WAIT_OBJECT_0 + 1 || receiveSocket.hasBufferedData())
#else
		if ((pollSet[1].revents & POLLERR) != 0)
			LOG_ERROR << "Failed to poll data from socket";
		else if ((pollSet[1].revents & (POLLIN | POLLHUP)) != 0 || receiveSocket.hasBufferedData())
#endif
		{
			for (bool skipCheck = K8PSH_SKIP_FIRST_SOCKET_DATA_CHECK; skipCheck || receiveSocket.hasData(); skipCheck = false)
			{
				PayloadType type;
				std::uint32_t payloadValue;

				if (!receiveSocket.read(type, payloadValue)) // Closed socket indicates abnormal termination
				{
					LOG_DEBUG << "Socket has been closed without exit code, aborting";
					std::abort();
				}

				switch (type)
				{
				case STDIN_DATA:
					if (!std::cin.eof())
					{
						LOG_DEBUG << "Received stdin close command from server";
						std::cin.setstate(std::ios_base::eofbit);
						(void)std::fclose(stdin);
					}

					break;

				case STDOUT_DATA:
					outputStdStreamData(std::cout, stdout, "stdout", receiveSocket, payloadValue);
					break;

				case STDERR_DATA:
					outputStdStreamData(std::cerr, stderr, "stderr", receiveSocket, payloadValue);
					break;

				case EXIT_CODE:
					LOG_DEBUG << "Received exit code (" << int(payloadValue) << ") from server";
					return int(payloadValue);

				default:
					LOG_ERROR << "Read invalid payload type (" << type << ") from socket";
				}
			}
		}
	}
}

#ifdef _WIN32
static std::mutex workingDirectoryMutex;
#endif

/** Runs the process requested by the remote socket channel.
 *
 * @param workingDirectory the relative working directory used to start the process
 * @param commands the map of commands for this server node
 * @param socket the open socket used to communicate with the client
 */
void k8psh::Process::run(const std::string &workingDirectory, const k8psh::Configuration::CommandMap &commands, k8psh::Socket &&socket)
{
	BufferedSendSocket sendSocket(socket);
	BufferedReceiveSocket receiveSocket(socket);
#ifdef _WIN32
	HANDLE process = HANDLE();

	try
#endif
	{
		// Read all the data for the command to execute
		std::string processDirectory;
		std::unordered_map<std::string, std::string> receivedEnvironmentVariables;
		std::unordered_map<std::string, OptionalString> environmentVariables;
		std::vector<std::string> arguments;
		std::string commandName;
		PayloadType type;
		std::uint32_t payloadValue;

		do
		{
			if (!receiveSocket.read(type, payloadValue)) // Closed socket indicates abnormal termination
				LOG_ERROR << "Failed to read data from socket";

			switch (type)
			{
			case WORKING_DIRECTORY:
				{
					std::string newDirectory = receiveSocket.readString(payloadValue);
					processDirectory = (workingDirectory.empty() ? std::string() : workingDirectory + "/") + newDirectory;
					LOG_DEBUG << "Received working directory (\"" << newDirectory << "\") from client, process directory set to \"" << processDirectory << "\"";
					break;
				}

			case ENVIRONMENT_VARIABLE:
				{
					std::string environmentVariable = receiveSocket.readString(payloadValue);
					LOG_DEBUG << "Received environment variable (\"" << environmentVariable << "\") from client";
					std::size_t equals = environmentVariable.find("=");

					if (equals == std::string::npos)
						receivedEnvironmentVariables.emplace(std::make_pair(environmentVariable, Utilities::getEnvironmentVariable(environmentVariable)));
					else
						receivedEnvironmentVariables[environmentVariable.substr(0, equals)] = environmentVariable.substr(equals + 1);

					break;
				}

			case COMMAND_ARGUMENT:
				arguments.push_back(receiveSocket.readString(payloadValue));
				LOG_DEBUG << "Received command argument (\"" << arguments.back() << "\") from client";
				break;

			case START_COMMAND:
				commandName = receiveSocket.readString(payloadValue);
				LOG_DEBUG << "Received start command (\"" << commandName << "\") from client";
				break;

			default:
				LOG_ERROR << "Read invalid payload type (" << type << ") from socket";
			}
		} while (type != START_COMMAND);

		auto commandIt = commands.find(commandName);

		if (commandIt == commands.end())
			LOG_ERROR << "Failed to find command \"" << commandName << "\" in configuration";

		// Build the process
		auto command = commandIt->second;
		std::vector<std::string> environment;

		for (auto it = command.getEnvironmentVariables().begin(); it != command.getEnvironmentVariables().end(); ++it)
		{
			// Inherited environment variable
			if (it->first[0] == '=')
			{
				const std::string name = it->first.substr(1);
				environmentVariables[name] = it->second.empty() ? Utilities::getEnvironmentVariable(name) : OptionalString(Utilities::substituteEnvironmentVariables(it->second, environmentVariables));
			}

			// User-provided environment variable
			else
			{
				const bool isOptional = it->first[0] == '?';
				const std::string name = isOptional ? it->first.substr(1) : it->first;
				auto valueIt = receivedEnvironmentVariables.find(name);

				if (valueIt != receivedEnvironmentVariables.end())
					environmentVariables[name] = valueIt->second;
				else
					environmentVariables[name] = it->second.empty() && isOptional ? Utilities::getEnvironmentVariable(environmentVariables, name) : OptionalString(Utilities::substituteEnvironmentVariables(it->second, environmentVariables));
			}
		}

		// Build the environment strings, only allowing one for any key
		for (auto it = command.getEnvironmentVariables().begin(); it != command.getEnvironmentVariables().end(); ++it)
		{
			const std::string name = it->first[0] == '=' || it->first[0] == '?' ? it->first.substr(1) : it->first;
			auto valueIt = environmentVariables.find(name);

			if (valueIt != environmentVariables.end() && valueIt->second)
			{
				environment.emplace_back(name + "=" + valueIt->second.c_str());
				environmentVariables.erase(valueIt);
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
				env.append(*it) += '\0';

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

				for (auto it = arguments.begin(); it != arguments.end(); ++it)
					argv.push_back(it->c_str());

				argv.push_back(NULL);

				(void)close(socket.abandon());
				stdInPipe.closeInput();
				stdOutPipe.closeOutput();
				stdErrPipe.closeOutput();

				if (!Utilities::changeWorkingDirectory(processDirectory))
					LOG_ERROR << "Failed to change directory to " << processDirectory;

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

				environ = const_cast<char **>(env.data());

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
			DWORD waitResult = WaitForMultipleObjects(DWORD(sizeof(waitSet) / sizeof(waitSet[0])), waitSet, FALSE, receiveSocket.hasBufferedData() ? 0 : stdInData.empty() ? INFINITE : 16);

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

			pollSet[0].fd = stdInData.empty() ? Pipe::INVALID_HANDLE : stdInPipe.getInput(); // Only wait on the stdin pipe if there is data ready to be sent to it
			pollSet[1].fd = stdOutPipe.getOutput();
			pollSet[2].fd = stdErrPipe.getOutput();

			do pollResult = poll(pollSet, nfds_t(sizeof(pollSet) / sizeof(pollSet[0])), receiveSocket.hasBufferedData() ? 0 : 250);
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
				sendSocket.write(STDOUT_DATA, pipeData, received);

				if (!received)
					stdOutPipe.closeOutput();
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
				sendSocket.write(STDERR_DATA, pipeData, received);

				if (!received)
					stdErrPipe.closeOutput();
			}

			// Check for socket data
#ifdef _WIN32
			if (waitResult == WAIT_OBJECT_0 + 2 || receiveSocket.hasBufferedData())
#else
			if ((pollSet[3].revents & POLLERR) != 0)
				LOG_ERROR << "Failed to poll data from socket";
			else if ((pollSet[3].revents & (POLLIN | POLLHUP)) != 0 || receiveSocket.hasBufferedData())
#endif
			{
				for (bool skipCheck = K8PSH_SKIP_FIRST_SOCKET_DATA_CHECK; skipCheck || receiveSocket.hasData(); skipCheck = false)
				{
					bool dataReceived = receiveSocket.read(type, payloadValue);

					if (!dataReceived) // Closed socket indicates abnormal termination
						type = TERMINATE_COMMAND;

					switch (type)
					{
					case STDIN_DATA:
						{
							LOG_DEBUG << "Received stdin data (" << payloadValue << " bytes) from client";

							if (stdInPipe.getInput() == Pipe::INVALID_HANDLE)
								LOG_DEBUG << "Ignoring " << payloadValue << " received bytes due to closed stdin"; // Ignore data on stdin after close
							else if (!payloadValue)
								closeStdIn = true;
							else
								stdInData += receiveSocket.readString(payloadValue);

							break;
						}

					default:
#ifdef _WIN32
						(void)TerminateProcess(process, UINT(-1));
#else
						(void)kill(process, 15); // Send TERM
#endif

						if (!dataReceived)
							LOG_ERROR << "Socket was closed unexpectedly";
						else if (type == TERMINATE_COMMAND)
							LOG_DEBUG << "Received terminate command from client, halting process";
						else
							LOG_ERROR << "Read invalid payload type (" << type << ") from socket";

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
					sendSocket.write(STDIN_DATA, stdInData, std::size_t());
				}
			}

			if (closeStdIn && stdInData.empty())
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
#endif
}
