
#include "Utilities.hxx"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
	#include <direct.h>
	#include <io.h>
	#include <winsock2.h>
	#include <windows.h>

	#pragma comment(lib, "ws2_32.lib")

	#define K8PSH_PATH_MAX MAX_PATH
	#define FILE_READ_MODE "rb"
#else
	#include <fcntl.h>
	#include <unistd.h>
	#include <sys/stat.h>

	#define K8PSH_PATH_MAX 4096
	#define FILE_READ_MODE "r"
#endif

// Gets the name of the file.
const char *k8psh::DebugInformation::getFilename() const
{
	return _file;
}

// Gets the line within the file.
std::size_t k8psh::DebugInformation::getLine() const
{
	return _line;
}

// Gets the name of the function.
const char *k8psh::DebugInformation::getFunction() const
{
	return _function;
}

// Checks if a list ("a, b,c,d") contains the specified item.
static bool listContainsCaseInsensitive(const std::string &list, const std::string &item)
{
	if (item.empty())
		return false;

	auto caseInsensitiveCompare = [](char c1, char c2) { return std::toupper(c1) == std::toupper(c2); };

	for (auto it = std::search(list.begin(), list.end(), item.begin(), item.end(), caseInsensitiveCompare); it != list.end(); it = std::search(it + item.length(), list.end(), item.begin(), item.end(), caseInsensitiveCompare))
	{
		bool matches = true;

		if (it != list.begin())
		{
			for (auto backIt = it - 1;; --backIt)
			{
				if (*backIt == ',' || *backIt == ';')
					break;
				else if (!k8psh::Utilities::isWhitespace(*backIt))
				{
					matches = false;
					break;
				}
				else if (backIt == list.begin())
					break;
			}
		}

		if (!matches)
			continue;

		// Check for trailing characters
		for (auto fwdIt = it + item.length();; ++fwdIt)
		{
			if (fwdIt == list.end() || *fwdIt == ',' || *fwdIt == ';')
				return true;
			else if (!k8psh::Utilities::isWhitespace(*fwdIt))
				break;
		}
	}

	return false;
}

// Gets the debug name (derived from the filename).
static std::string getDebugName(const char *filename)
{
	std::string basename = k8psh::Utilities::getBasename(filename);
	std::size_t dotIndex = basename.find(".");
	return dotIndex == std::string::npos ? basename : basename.substr(0, dotIndex);
}

// Checks if debug logging is enabled for the specified filename.
bool k8psh::Logger::shouldLogDebug(const char *filename)
{
	auto toDebug = Utilities::getEnvironmentVariable("K8PSH_DEBUG");

#ifdef K8PSH_DEBUG
	#define K8PSH_QUOTE_(X) #X
	#define K8PSH_QUOTE(X) K8PSH_QUOTE_(X)
	if (!toDebug)
		toDebug = K8PSH_QUOTE(K8PSH_DEBUG);
	#undef K8PSH_QUOTE_
	#undef K8PSH_QUOTE
#endif

	return listContainsCaseInsensitive(toDebug, getDebugName(filename)) || listContainsCaseInsensitive(toDebug, "all");
}

k8psh::Logger::~Logger() noexcept(false)
{
	static auto atomicWrite = [](std::FILE *stream, const std::string &content) { (void)std::fwrite(&content[0], content.length(), 1, stream); };

	std::string message = _logger.str();
	std::string level5Chars;
	std::stringstream ss;
#ifdef _WIN32
	auto id = std::this_thread::get_id();
#else
	auto id = std::uint32_t(getpid() * 16777619U + std::hash<std::thread::id>()(std::this_thread::get_id()));
#endif
	auto now = std::chrono::system_clock::now();
	auto time = std::chrono::system_clock::to_time_t(now);
	auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());

	switch (_level)
	{
	case LEVEL_DEBUG:   level5Chars = "DEBUG, "; break;
	case LEVEL_WARNING: level5Chars = "WARN,  "; break;
	default:            level5Chars = "ERROR, "; break;
	}

	ss << "[" << std::put_time(std::gmtime(&time), "%F %T.") << std::setfill('0') << std::setw(6) << std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch() - seconds).count() << std::setfill(' ') << ", " <<
		level5Chars <<
		id << "] ";

	switch (_level)
	{
	case LEVEL_DEBUG:
		ss << "(" << getDebugName(getFilename()) << ") " << message << std::endl;
		atomicWrite(stdout, ss.str());
		break;

	case LEVEL_WARNING:
		ss << message << std::endl;
		atomicWrite(stderr, ss.str());
		break;

	default: // Error
		{
			std::string filename = getFilename();
#ifdef _WIN32
			std::size_t filenameIndex = filename.rfind("\\src\\") + 1;
#else
			std::size_t filenameIndex = filename.rfind("/src/") + 1;
#endif
			ss << message << " (" << filename.substr(filenameIndex) << ":" << getLine() << ")" << std::endl;
			atomicWrite(stderr, ss.str());
			throw std::runtime_error(message);
		}
	}
}

// Gets the stream used to log the message
std::stringstream &k8psh::Logger::getStream() const
{
	return const_cast<std::stringstream &>(_logger);
}

// Constructs a new anonymous pipe
k8psh::Pipe::Pipe()
{
#ifdef _WIN32
	SECURITY_ATTRIBUTES sa = { };

	sa.nLength = DWORD(sizeof(sa));
	sa.bInheritHandle = TRUE;

	if (CreatePipe(&_output, &_input, &sa, 0) == 0)
		LOG_ERROR << "Failed to create pipe: " << GetLastError();
#else
	Handle fds[2];

	if (pipe(fds) != 0)
		LOG_ERROR << "Failed to create pipe: " << errno;

	_input = fds[1];
	_output = fds[0];
#endif
}

// Closes the specified pipe handle
static void closePipeHandle(k8psh::Pipe::Handle handle)
{
#ifdef _WIN32
	if (handle)
		(void)CloseHandle(handle);
#else
	if (handle >= 0)
		close(handle);
#endif
}

// Destructs the anonymous pipe
k8psh::Pipe::~Pipe()
{
	closePipeHandle(_input);
	closePipeHandle(_output);
}

// Closes the input handle.
void k8psh::Pipe::closeInput()
{
	closePipeHandle(_input);
	_input = INVALID_HANDLE;
}

// Closes the output handle.
void k8psh::Pipe::closeOutput()
{
	closePipeHandle(_output);
	_output = INVALID_HANDLE;
}

/** Reads data from the pipe.
 *
 * @param buffer the buffer to fill with data from the pipe
 * @return the number of bytes read from the pipe
 */
std::size_t k8psh::Pipe::read(std::string &buffer)
{
#ifdef _WIN32
	DWORD read = 0;

	if (ReadFile(_output, &buffer[0], DWORD(buffer.length()), &read, NULL) == 0 && GetLastError() != ERROR_BROKEN_PIPE)
		LOG_ERROR << "Failed to read data from pipe: " << GetLastError();
#else
	ssize_t read = -1;

	while (read == -1)
	{
		read = ::read(_output, &buffer[0], buffer.length());

		if (read == -1 && errno != EINTR)
			LOG_ERROR << "Failed to read data from pipe: " << errno;
	}
#endif

	return read;
}

// Remaps the specified pipe handle to the new handle
static bool remapPipeHandle(k8psh::Pipe::Handle &handle, k8psh::Pipe::Handle newHandle)
{
	if (newHandle == k8psh::Pipe::INVALID_HANDLE)
	{
		closePipeHandle(handle);
		handle = newHandle;
		return true;
	}

#ifdef _WIN32
	return handle == newHandle;
#else
	if (handle == newHandle)
		return true;
	else if (handle == k8psh::Pipe::INVALID_HANDLE || dup2(handle, newHandle) < 0)
		return false;

	(void)close(handle);
	handle = newHandle;
	return true;
#endif
}

// Remaps the input to the specified handle.
bool k8psh::Pipe::remapInput(k8psh::Pipe::Handle handle)
{
	return remapPipeHandle(_input, handle);
}

// Remaps the output to the specified handle.
bool k8psh::Pipe::remapOutput(k8psh::Pipe::Handle handle)
{
	return remapPipeHandle(_output, handle);
}

// Sets the input as non-blocking.
bool k8psh::Pipe::setInputNonblocking()
{
#ifdef _WIN32
	DWORD mode = PIPE_NOWAIT;

	return SetNamedPipeHandleState(_input, &mode, NULL, NULL) != 0;
#else
	int flags = fcntl(_input, F_GETFL);

	return flags != -1 && fcntl(_input, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

/** Writes data to the pipe.
 *
 * @param buffer the buffer to write to the pipe
 * @return the number of bytes written to the pipe, which may be less than the size of the buffer if the reading end of the pipe is closed
 */
std::size_t k8psh::Pipe::write(const std::string &buffer)
{
	if (_input == INVALID_HANDLE)
		return 0;

#ifdef _WIN32
	DWORD bytesWritten = 0;

	if (WriteFile(_input, &buffer[0], DWORD(buffer.length()), &bytesWritten, NULL) != 0)
		return bytesWritten;
#else
	auto wrote = ::write(_input, &buffer[0], buffer.length());

	if (wrote >= 0)
		return wrote;
	else if (errno == EINTR || errno == EAGAIN)
		return 0;
#endif

	closeInput();
	return 0;
}

/** Changes the working directory of the process.
 *
 * @param directory the new working directory of the process, which can be relative to the current working directory or absolute
 * @return true if the working directory was changed to the specified directory, otherwise false
 */
bool k8psh::Utilities::changeWorkingDirectory(const std::string &directory)
{
#ifdef _WIN32
	return SetCurrentDirectoryA(directory.c_str()) != 0;
#else
	return chdir(directory.c_str()) == 0;
#endif
}

// Deletes the file, returning true if the file exists and was deleted.
bool k8psh::Utilities::deleteFile(const std::string &filename)
{
#ifdef _WIN32
	return DeleteFileA(filename.c_str()) != 0;
#else
	return unlink(filename.c_str()) == 0;
#endif
}

// Tests if the character is a path separator
static bool isPathSeparator(char c)
{
#ifdef _WIN32
	return c == '/' || c == '\\';
#else
	return c == '/';
#endif
}

// Gets the absolute path of the file.
std::string k8psh::Utilities::getAbsolutePath(const std::string &filename)
{
	if (filename.empty())
		return getWorkingDirectory();

#ifdef _WIN32
	std::string buffer;
	DWORD requiredSize = MAX_PATH + 1;

	do
	{
		if (requiredSize == 0)
			LOG_ERROR << "Cannot get absolute path of \"" << filename << "\": " << GetLastError();

		buffer.resize(requiredSize - 1);
		requiredSize = GetFullPathNameA(filename.c_str(), DWORD(buffer.size() + 1), &buffer[0], NULL);
	} while (requiredSize > buffer.size());

	buffer.resize(requiredSize);
	return buffer;
#else
	if (isPathSeparator(filename[0]))
		return filename;

	char buffer[K8PSH_PATH_MAX + 1];
	char *absolutePath = realpath(filename.c_str(), buffer);

	if (!absolutePath)
		LOG_ERROR << "Cannot get absolute path of \"" << filename << "\": " << errno;

	return absolutePath;
#endif
}

// Gets the basename of the file.
std::string k8psh::Utilities::getBasename(const std::string &filename)
{
	std::size_t end = filename.length();

	if (end == 0)
		return ".";

	while (end > 1 && isPathSeparator(filename[end - 1]))
		end--;

	for (std::size_t i = end - 1; i > 0; i--)
	{
		if (isPathSeparator(filename[i - 1]))
			return filename.substr(i, end - i);
	}

	return filename.substr(0, end);
}

// Gets the full path of the current executable.
std::string k8psh::Utilities::getExecutablePath()
{
	static std::size_t initialBufferSize = K8PSH_PATH_MAX;
	std::string buffer = std::string(initialBufferSize, '\0');

#ifdef _WIN32
	while (GetModuleFileNameA(NULL, &buffer[0], DWORD(buffer.size())) == buffer.size())
#else
	while (std::size_t(readlink("/proc/self/exe", &buffer[0], buffer.size())) == buffer.size())
#endif
		buffer.resize(buffer.size() * 2);

	std::string executablePath = buffer.c_str();
	initialBufferSize = initialBufferSize > executablePath.length() ? initialBufferSize : executablePath.length();

	return executablePath;
}

// Gets the environment variable with the specified name.
k8psh::OptionalString k8psh::Utilities::getEnvironmentVariable(const std::string &name)
{
	return OptionalString(getenv(name.c_str()));
}

#ifndef _WIN32
/** Gets the line of text starting at the specified position.
 *
 * @param value the string used to get the line
 * @param position the starting position for the line
 * @return the line of text starting at the specified position
 */
static std::string getLine(const std::string &value, std::size_t position = 0)
{
	return value.substr(position, value.find_first_of('\n', position));
}
#endif

// Gets the name of the host.
std::string k8psh::Utilities::getHostname()
{
	static std::string name;

#ifdef _WIN32
	if (name.empty())
	{
		WORD wVersionRequested = MAKEWORD(2, 0);
		WSADATA wsaData;

		if (WSAStartup(wVersionRequested, &wsaData) == 0)
		{
			std::string buffer = std::string(MAX_PATH, '\0');

			while (gethostname(&buffer[0], int(buffer.size())) != 0 && GetLastError() == WSAEFAULT)
				buffer.resize(buffer.size() * 2);

			name = &buffer[0];
			WSACleanup();
		}
	}
#else
	if (name.empty())
		name = getLine(readFile("/etc/hostname"));
#endif

	return name;
}

// Gets the parent directory of the file.
std::string k8psh::Utilities::getParentDirectory(const std::string &filename)
{
	std::size_t end = filename.length();

#ifdef _WIN32
	if (end == 2 && filename[1] == ':')
		return filename;
#endif

	while (end > 0 && !isPathSeparator(filename[end - 1]))
		end--;

	while (end > 1 && isPathSeparator(filename[end - 1]))
		end--;

	return filename.substr(0, end);
}

// Gets the working directory of the current process. An empty string will be returned if the working directory could not be determined.
std::string k8psh::Utilities::getWorkingDirectory()
{
	static std::size_t initialBufferSize = K8PSH_PATH_MAX;
	std::string buffer = std::string(initialBufferSize, '\0');

#ifdef _WIN32
	while (!_getcwd(&buffer[0], int(buffer.size())) && errno == ERANGE)
#else
	while (!getcwd(&buffer[0], buffer.size()) && errno == ERANGE)
#endif
		buffer.resize(buffer.size() * 2);

	std::string workingDirectory = buffer.c_str();
	initialBufferSize = initialBufferSize > workingDirectory.length() ? initialBufferSize : workingDirectory.length();

	return workingDirectory;
}

// Checks if the filename is an absolute path.
bool k8psh::Utilities::isAbsolutePath(const std::string &filename)
{
#ifdef _WIN32
	if ((filename.length() == 2 && filename[1] == ':') || (filename.length() > 2 && filename[1] == ':' && isPathSeparator(filename[2])))
		return true;
#endif

	if (isPathSeparator(filename[0]))
		return true;

	return false;
}

/** Gets the contents of a file.
 *
 * @param filename the name of the file
 * @return the contents of the file
 */
k8psh::OptionalString k8psh::Utilities::readFile(const std::string &filename)
{
	std::string contents;
	std::FILE *file = std::fopen(filename.c_str(), FILE_READ_MODE);

	// Treat this as normal and continue
	if (!file)
		return OptionalString();

	(void)std::fseek(file, 0, SEEK_END);
	const long size = std::ftell(file);
	(void)std::fseek(file, 0, SEEK_SET);
	contents.resize(size);
	std::size_t read = std::fread(&contents[0], 1, size, file);
	(void)std::fclose(file);

	if (read != std::size_t(size))
		LOG_ERROR << "Failed to load complete file; loaded " << read << " / " << size << " bytes from " << filename;

	return contents;
}

/** Relativizes the path based on the parent.
 *
 * @param parent the parent directory used to relativize the path
 * @param path the path to relativize
 * @return the relativized path
 */
std::string k8psh::Utilities::relativize(const std::string &parent, const std::string &path)
{
	std::size_t i = 0, j = 0;

	for (;;)
	{
		if (j == parent.length() || isPathSeparator(parent[j]))
		{
			// Skip any '/' or '/./'
			while (isPathSeparator(parent[j]) || (parent[j] == '.' && (j + 1 == parent.length() || isPathSeparator(parent[j + 1]))))
				j++;

			if (i < path.length() && !isPathSeparator(path[i]))
				LOG_ERROR << "Cannot relativize unrelated paths \"" << parent << "\", \"" << path << "\"";

			// Skip any '/' or '/./'
			while (isPathSeparator(path[i]) || (path[i] == '.' && (i + 1 == path.length() || isPathSeparator(path[i + 1]))))
				i++;

			if (j == parent.length())
				return path.substr(i);
		}

#ifdef _WIN32
		if (std::toupper(parent[j]) != std::toupper(path[i]))
#else
		if (parent[j] != path[i])
#endif
			LOG_ERROR << "Cannot relativize unrelated paths \"" << parent << "\", \"" << path << "\"";

 		i++;
		j++;
	}
}

/** Sets the environment variable with the specified name to the specified value.
 *
 * @param name the name of the environment variable to set
 * @param value the value to assign to the environment variable
 * @return true if the environment variable was set, otherwise false
 */
bool k8psh::Utilities::setEnvironmentVariable(const std::string &name, const k8psh::OptionalString &value)
{
#ifdef _WIN32
	return _putenv((name + "=" + value).c_str()) == 0;
#else
	return (value ? setenv(name.c_str(), value.c_str(), 1) : unsetenv(name.c_str())) == 0;
#endif
}

// Gets either the override (if it exists) or the environment variable associated with the specified key.
static k8psh::OptionalString getOverrideOrEnvironmentVariable(const std::unordered_map<std::string, std::string> &overrides, const std::string key)
{
	auto it = overrides.find(key);

	if (it != overrides.end())
		return it->second;

	return k8psh::Utilities::getEnvironmentVariable(key);
}

/** Substitutes environment variables into the given string. Environment variables must take the form ${VAR:-default value} with the default value being optional.
 *
 * @param in the string to substitute environment variables into
 * @param overrides a map of overrides that will be used during the substitution
 * @return the string with the environment variable substitutions made
 */
std::string k8psh::Utilities::substituteEnvironmentVariables(const std::string &in, const std::unordered_map<std::string, std::string> &overrides)
{
	static constexpr std::uint8_t VALID_ENV_NAME_CHARS[256] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
		0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
		0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0
	};

	std::string result;
	std::size_t at = 0;

	result.reserve(in.length());

	for (std::size_t i = 0; i < in.length(); )
	{
		const std::size_t dollar = in.find("${", i);
		result.append(in, at, dollar - at);

		if (dollar >= in.length())
			return result;

		at = dollar;
		const std::size_t envNameOffset = i = dollar + 2;

		for (; i < in.length(); i++)
		{
			const unsigned char c = in[i];

			if (!VALID_ENV_NAME_CHARS[c])
				break;

			// Find the default value (":-") of the environment variable
			else if (c == ':')
			{
				if (in[i + 1] != '-')
					break;

				const std::size_t defaultOffset = i + 2;
				const std::size_t end = in.find('}', defaultOffset); // Find the closing brace ("}") of the environment variable

				if (end >= in.length())
				{
					i = end;
					break;
				}

				std::string envName = in.substr(envNameOffset, i - dollar - 2);
				const auto value = getOverrideOrEnvironmentVariable(overrides, envName);

				if (value)
					result.append(value);
				else
					result.append(in.substr(defaultOffset, end - defaultOffset));

				at = i = end + 1;
				break;
			}

			// Find the closing brace ("}") of the environment variable
			else if (c == '}')
			{
				std::string envName = in.substr(envNameOffset, i - dollar - 2);
				const auto value = getOverrideOrEnvironmentVariable(overrides, envName);

				if (value)
					result.append(value);
				else
					LOG_WARNING << "Environment variable \"" << envName << "\" is not defined, substituting an empty string";

				at = ++i;
				break;
			}
		}
	}

	result.append(in, at, std::string::npos);

	return result;
}
