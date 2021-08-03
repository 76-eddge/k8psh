// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#include "Configuration.hxx"

#include <unordered_map>
#include <utility>

#include "Utilities.hxx"

// Gets the remaining string in a line.
static std::string getRestOfLine(const std::string &in, std::size_t offset)
{
	std::size_t i = offset;

	while (in[i] && in[i] != '\r' && in[i] != '\n')
		i++;

	return in.substr(offset, i - offset);
}

// Gets the value of a hexadecimal character
static char parseHexValue(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	else if (c >= 'a' && c <= 'f')
		return c - 'a';
	else if (!(c >= 'A' && c <= 'F'))
		LOG_ERROR << "Invalid hex character in configuration: " << c;

	return c - 'A';
}

// Parses a string from the configuration.
static std::size_t parseString(const std::string &configurationString, std::size_t offset, std::string &value, const char terminator = 0)
{
	const std::size_t start = offset;
	std::size_t substituteStart = 0;
	value.clear();

	while (configurationString[offset] && !k8psh::Utilities::isWhitespace(configurationString[offset]) && configurationString[offset] != '#' && configurationString[offset] != terminator)
	{
		if (configurationString[offset] == '\'')
		{
			value.replace(substituteStart, value.length(), k8psh::Utilities::substituteEnvironmentVariables(value.substr(substituteStart)));

			for (offset++; configurationString[offset] != '\'' || configurationString[++offset] == '\''; offset++)
			{
				if (!configurationString[offset])
					LOG_ERROR << "Unterminated quoted string in configuration: " << configurationString.substr(start);

				value += configurationString[offset];
			}

			substituteStart = value.length();
		}
		else if (configurationString[offset] == '\"')
		{
			for (offset++; configurationString[offset] != '\"' || configurationString[++offset] == '\"'; offset++)
			{
				if (!configurationString[offset])
					LOG_ERROR << "Unterminated double quoted string in configuration: " << configurationString.substr(start);
				else if (configurationString[offset] != '\\')
					value += configurationString[offset];
				else
				{
					switch (configurationString[++offset])
					{
						case '"': value += '"'; break;
						case '\\': value += '\\'; break;
						case '\'': value += '\''; break;

						case 'b': value += '\b'; break;
						case 't': value += '\t'; break;
						case 'n': value += '\n'; break;
						case 'f': value += '\f'; break;
						case 'r': value += '\r'; break;
						case '0': value += '\0'; break;

						case 'x':
							value += (parseHexValue(configurationString[offset + 1]) << 4) + parseHexValue(configurationString[offset + 2]);
							offset += 2;
							break;

						default: LOG_ERROR << "Unrecognized escape sequence (\\" << configurationString[offset] << ") in string " << configurationString.substr(start, offset + 1 - start) << "...";
					}
				}
			}
		}
		else
			value += configurationString[offset++];
	}

	value.replace(substituteStart, value.length(), k8psh::Utilities::substituteEnvironmentVariables(value.substr(substituteStart)));
	LOG_DEBUG << "Parsed string: " << value;

	return offset;
}

// Skips all content that is part of a comment.
static std::size_t skipComment(const std::string &configurationString, std::size_t offset)
{
	while (configurationString[offset] && configurationString[offset] != '\n')
		offset++;

	return offset;
}

// Skips all whitespace content.
static std::size_t skipWhitespace(const std::string &configurationString, std::size_t offset)
{
	while (k8psh::Utilities::isWhitespace(configurationString[offset]))
		offset++;

	return offset;
}

// Skips all non-newline whitespace content.
static std::size_t skipNonNewlineWhitespace(const std::string &configurationString, std::size_t offset)
{
	while (configurationString[offset] == '\t' || configurationString[offset] == ' ')
		offset++;

	return offset;
}

// Ensures that the remainder of the line does not contain any more content.
static std::size_t ensureRestOfLineEmpty(const std::string &configurationString, std::size_t offset)
{
	offset = skipNonNewlineWhitespace(configurationString, offset);

	if (configurationString[offset] == '#')
		return skipComment(configurationString, offset + 1);

	while (configurationString[offset] && configurationString[offset] != '\n')
	{
		if (!k8psh::Utilities::isWhitespace(configurationString[offset]))
			LOG_ERROR << "Expecting end of line, but found \"" << getRestOfLine(configurationString, offset) << "\"";

		offset++;
	}

	return offset;
}

// Gets a configuration key/value.
static std::size_t getConfigurationValue(const std::string &configurationString, std::size_t offset, std::string &key, std::string &value)
{
	// Find key
	offset = parseString(configurationString, offset, key, '=');

	// Find equals
	bool foundEquals = false;

	while (configurationString[offset] == '\t' || configurationString[offset] == ' ' || (!foundEquals && configurationString[offset] == '='))
	{
		foundEquals |= configurationString[offset] == '=';
		offset++;
	}

	if (!foundEquals)
		return offset;

	// Find value
	return parseString(configurationString, offset, value);
}

// Parses the host section value.
static std::size_t parseHost(const std::string &configurationString, std::size_t offset, std::string &host)
{
	// Parse until the ending section block (])
	offset = skipNonNewlineWhitespace(configurationString, parseString(configurationString, offset, host, ']'));

	if (host.empty())
		LOG_ERROR << "Expecting hostname, but found \"" << getRestOfLine(configurationString, offset) << "\"";
	else if (configurationString[offset] != ']')
		LOG_ERROR << "Expecting host section close tag (]), but found \"" << getRestOfLine(configurationString, offset) << "\"";

	return skipNonNewlineWhitespace(configurationString, offset + 1);
}

// Parses a line containing whitespace delimited arguments.
static std::size_t parseArguments(const std::string &configurationString, std::size_t offset, std::vector<std::string> &values)
{
	// Parse until a comment or end of line
	while (configurationString[offset] && !k8psh::Utilities::isWhitespace(configurationString[offset]) && configurationString[offset] != '#')
	{
		std::string value;
		offset = skipNonNewlineWhitespace(configurationString, parseString(configurationString, offset, value));
		values.emplace_back(value);
	}

	return offset;
}

// Loads the configuration from a string.
k8psh::Configuration k8psh::Configuration::load(const std::string &configurationString, const std::string &workingPath)
{
	k8psh::Configuration configuration;
	std::string absoluteWorkingPath = Utilities::getAbsolutePath(workingPath);
	std::size_t i = 0;

	configuration._baseDirectory = absoluteWorkingPath;

	// Parse client settings
	for (;;)
	{
		i = skipWhitespace(configurationString, i);

		if (!configurationString[i] || configurationString[i] == '[') // Start server settings
			break;
		else if (configurationString[i] == '#') // Comment
			i = skipComment(configurationString, i + 1);
		else
		{
			std::string key, value;

			i = ensureRestOfLineEmpty(configurationString, getConfigurationValue(configurationString, i, key, value));

			if (key == "baseDirectory")
				configuration._baseDirectory = Utilities::isAbsolutePath(value) ? value : Utilities::getAbsolutePath(absoluteWorkingPath + '/' + value);
			else
				LOG_ERROR << "Unrecognized configuration key \"" << key << '"';
		}
	}

	// Parse server settings
	std::shared_ptr<Host> currentHost;
	unsigned short currentPort = DEFAULT_STARTING_PORT;

	for (;;)
	{
		i = skipWhitespace(configurationString, i);

		if (!configurationString[i])
			break;
		else if (configurationString[i] == '#') // Comment
			i = skipComment(configurationString, i + 1);
		else if (configurationString[i] == '[') // Host section
		{
			currentHost = std::make_shared<Host>();
			std::string host;
			i = ensureRestOfLineEmpty(configurationString, parseArguments(configurationString, parseHost(configurationString, skipNonNewlineWhitespace(configurationString, i + 1), host), currentHost->_options));

			// Parse the port
			std::size_t colon = host.find(":");

			if (colon != std::string::npos)
			{
				std::string port = host.substr(colon + 1);

				for (std::size_t j = 0; j < port.length(); j++)
				{
					if (port[j] < '0' || port[j] > '9')
						LOG_ERROR << "Invalid port number \"" << port << '"';
				}

				unsigned long portValue = std::stoul(port);

				if (portValue >= 65536)
					LOG_ERROR << "Port out of range: " << portValue;

				currentPort = static_cast<unsigned short>(portValue);
			}

			// Assign the new host information
			currentHost->_hostname = host.substr(0, colon);
			currentHost->_port = currentPort++;
		}
		else // Executable
		{
			Command command;
			std::vector<std::string> values;
			i = ensureRestOfLineEmpty(configurationString, parseArguments(configurationString, i, values));

			// Create the command
			command._host = currentHost;
			command._name = std::move(values[0]);

			for (std::size_t j = 1; j < values.size(); j++)
			{
				std::size_t equals;

				if (!command._executable.empty() || values[j].empty() || (equals = values[j].find("=", 1)) == std::string::npos)
					command._executable.emplace_back(std::move(values[j]));
				else
					command._environmentVariables.emplace_back(std::make_pair(values[j].substr(0, equals), values[j].substr(equals + 1)));
			}

			if (command._executable.empty())
				command._executable.emplace_back(command.getName());

			// Add the command to the configuration
			configuration._hostCommands[currentHost->getHostname()][command.getName()] = command;
			configuration._commands[command.getName()] = std::move(command);
		}
	}

	return configuration;
}
