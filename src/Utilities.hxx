
#ifndef K8PSH_UTILITIES_HXX
#define K8PSH_UTILITIES_HXX

#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#ifdef _WIN32
	#include <windows.h>
#else
	#include <unistd.h>
#endif

namespace k8psh {

class DebugInformation
{
	const char *_file;
	const std::size_t _line;
	const char *_function;

public:
	DebugInformation(const char *file, std::size_t line, const char *function) : _file(file), _line(line), _function(function) { }

	// Gets the name of the file.
	const char *getFilename() const;

	// Gets the line within the file.
	std::size_t getLine() const;

	// Gets the name of the function.
	const char *getFunction() const;
};

#if __cplusplus >= 201103L
#define PREPROCESSOR_FUNCTION_IDENTIFIER __func__
#elif defined(_MSC_VER) || defined(__GNUC__) || defined(__INTEL_COMPILER)
#define PREPROCESSOR_FUNCTION_IDENTIFIER __FUNCTION__
#else
#define PREPROCESSOR_FUNCTION_IDENTIFIER __func__
#endif

class Logger : private DebugInformation
{
public:
	enum Level
	{
		LEVEL_DEBUG,
		LEVEL_WARNING,
		LEVEL_ERROR
	};

private:
	const Level _level;
	std::stringstream _logger;

public:
	// Checks if debug logging is enabled for the specified filename.
	static bool shouldLogDebug(const char *filename);

	Logger(const char *filename, std::size_t line, const char *function, Level level) : DebugInformation(filename, line, function), _level(level) { }
	~Logger();

	// Gets the stream used to log the message
	std::stringstream &getStream() const;
};

// Logs a debug statement (if not enabled, statement has no side effect). Example: `LOG_DEBUG << "Here's some debug info: " << a << ", " << b;`
#if defined(DEBUG) || defined(_DEBUG) || defined(K8PSH_DEBUG)
	#define LOG_DEBUG for (bool _k8psh_shouldLog_ = ::k8psh::Logger::shouldLogDebug(__FILE__); _k8psh_shouldLog_; _k8psh_shouldLog_ = !_k8psh_shouldLog_) \
		::k8psh::Logger(__FILE__, __LINE__, PREPROCESSOR_FUNCTION_IDENTIFIER, ::k8psh::Logger::LEVEL_DEBUG).getStream()
#else
struct NullStream
{
	template <typename T> NullStream &operator<<(const T&) { return *this; }
};

	#define LOG_DEBUG while (false) ::k8psh::NullStream()
#endif

// Logs a warning and continues. Example: `LOG_WARNING << "Something potentially unexpected occured";`
#define LOG_WARNING ::k8psh::Logger(__FILE__, __LINE__, PREPROCESSOR_FUNCTION_IDENTIFIER, ::k8psh::Logger::LEVEL_WARNING).getStream()

// Logs an error and throws an exception. Example: `LOG_ERROR << "An error occured";`
#define LOG_ERROR for (::k8psh::Logger logger(__FILE__, __LINE__, PREPROCESSOR_FUNCTION_IDENTIFIER, ::k8psh::Logger::LEVEL_ERROR); true; throw std::runtime_error(logger.getStream().str())) logger.getStream()

class Pipe
{
public:
#ifdef _WIN32
	typedef HANDLE Handle;

	static constexpr Handle INVALID_HANDLE = Handle();
#else
	typedef int Handle;

	static constexpr Handle INVALID_HANDLE = Handle(-1);
#endif

private:
	Handle _input;
	Handle _output;

public:
	Pipe();
	~Pipe();

	// Closes the input handle.
	void closeInput();

	// Closes the output handle.
	void closeOutput();

	// Gets the input handle.
	Handle getInput() const { return _input; }

	// Gets the output handle.
	Handle getOutput() const { return _output; }

	/** Reads data from the pipe.
	 *
	 * @param buffer the buffer to fill with data from the pipe
	 * @return the number of bytes read from the pipe
	 */
	std::size_t read(std::string &buffer);

	// Remaps the input to the specified handle.
	bool remapInput(Handle handle);

	// Remaps the output to the specified handle.
	bool remapOutput(Handle handle);

	// Sets the input as non-blocking.
	bool setInputNonblocking();

	/** Writes data to the pipe.
	 *
	 * @param buffer the buffer to write to the pipe
	 * @return the number of bytes written to the pipe, which may be less than the size of the buffer if the reading end of the pipe is closed
	 */
	std::size_t write(const std::string &buffer);
};

class OptionalString : public std::string
{
	bool _exists;

public:
	OptionalString(const char *value = NULL) : std::string(value ? value : ""), _exists(value ? true : false) { }
	OptionalString(const std::string &value) : std::string(value), _exists(true) { }
	OptionalString(std::string &&value) : std::string(value), _exists(true) { }

	// Sets whether or not the string exists
	void exists(bool exists = true) { _exists = exists; }

	// Checks if the string is present
	operator bool() const { return _exists; }
};

class Utilities
{
	// Utilities class cannot be instantiated.
	Utilities();

public:
	/** Changes the working directory of the process.
	 *
	 * @param directory the new working directory of the process, which can be relative to the current working directory or absolute
	 * @return true if the working directory was changed to the specified directory, otherwise false
	 */
	static bool changeWorkingDirectory(const std::string &directory);

	// Deletes the file, returning true if the file exists and was deleted.
	static bool deleteFile(const std::string &filename);

	// Gets the absolute path of the file.
	static std::string getAbsolutePath(const std::string &filename);

	// Gets the basename of the file.
	static std::string getBasename(const std::string &filename);

	// Gets the full path of the current executable.
	static std::string getExecutablePath();

	// Gets the environment variable with the specified name. An empty string will be returned if the working directory could not be determined.
	static OptionalString getEnvironmentVariable(const std::string &name);

	// Gets the name of the host.
	static std::string getHostname();

	// Gets the parent directory of the file.
	static std::string getParentDirectory(const std::string &filename);

	// Gets the working directory of the current process.
	static std::string getWorkingDirectory();

	// Checks if the filename is an absolute path.
	static bool isAbsolutePath(const std::string &filename);

	// Checks if the character is a whitespace character.
	static bool isWhitespace(char c) { return (c >= 9 && c <= 13) || c == ' '; }

	/** Gets the contents of a file.
	 *
	 * @param filename the name of the file
	 * @return the contents of the file
	 */
	static OptionalString readFile(const std::string &filename);

	/** Relativizes the path based on the parent.
	 *
	 * @param parent the parent directory used to relativize the path
	 * @param path the path to relativize
	 * @return the relativized path
	 */
	static std::string relativize(const std::string &parent, const std::string &path);

	/** Sets the environment variable with the specified name to the specified value.
	 *
	 * @param name the name of the environment variable to set
	 * @param value the value to assign to the environment variable
	 * @return true if the environment variable was set, otherwise false
	 */
	static bool setEnvironmentVariable(const std::string &name, const OptionalString &value);

	/** Substitutes environment variables into the given string. Environment variables must take the form ${VAR:-default value} with the default value being optional.
	 *
	 * @param in the string to substitute environment variables into
	 * @param overrides a map of overrides that will be used during the substitution
	 * @return the string with the environment variable substitutions made
	 */
	static std::string substituteEnvironmentVariables(const std::string &in, const std::unordered_map<std::string, std::string> &overrides = std::unordered_map<std::string, std::string>());
};

} // k8psh

#endif // K8PSH_UTILITIES_HXX
