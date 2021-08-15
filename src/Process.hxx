// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#ifndef K8PSH_PROCESS_HXX
#define K8PSH_PROCESS_HXX

#include <string>

#include "Configuration.hxx"
#include "Socket.hxx"

namespace k8psh {

class Process
{
	// Process class cannot be instantiated.
	Process();

public:
	/** Runs a process remotely on the configured host.
	 *
	 * @param workingDirectory the relative working directory used to start the process
	 * @param command the command to start
	 * @param argc the number of additional arguments used to start the process
	 * @param argv additional arguments used to start the process
	 * @param configuration the global configuration
	 * @return the exit code of the process
	 */
	static int runRemoteCommand(const std::string &workingDirectory, const Configuration::Command &command, std::size_t argc, const char *argv[], const Configuration &configuration);

	/** Runs the process requested by the remote socket channel.
	 *
	 * @param workingDirectory the relative working directory used to start the process
	 * @param commands the map of commands for this server node
	 * @param socket the open socket used to communicate with the client
	 */
	static void run(const std::string &workingDirectory, const Configuration::CommandMap &commands, Socket &&socket);
};

} // k8psh

#endif // K8PSH_PROCESS_HXX
