// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#ifndef K8PSH_SOCKET_HXX
#define K8PSH_SOCKET_HXX

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
	#include <winsock2.h>
#endif

namespace k8psh {

class Socket
{
	// Sockets cannot be copied
	Socket(const Socket&);
	Socket &operator=(const Socket&);

public:
	struct Initializer
	{
		Initializer();
		~Initializer();
	};

#ifdef _WIN32
	typedef SOCKET Handle;
	typedef HANDLE Event;

	static constexpr Handle INVALID_HANDLE = INVALID_SOCKET;
#else
	typedef int Handle;
	typedef int Event;

	static constexpr Handle INVALID_HANDLE = Handle(-1);
#endif
	static const unsigned short RANDOM_PORT = 0;

private:
	Handle _handle;
#ifdef _WIN32
	Event _readEvent;
#endif

#ifdef _WIN32
	Socket(Handle handle) : _handle(handle), _readEvent() { }
#else
	Socket(Handle handle) : _handle(handle) { }
#endif

public:
	// Creates a new server socket listening on the specified port.
	static Socket listen(unsigned short port);

	// Creates a new socket by connecting to the specified port. This call may return an invalid socket if a recoverable error is encountered.
	static Socket connect(unsigned short port, bool failOnError = true);

	Socket() : _handle(INVALID_HANDLE) { }
#ifdef _WIN32
	Socket(Socket &&other) : _handle(other._handle), _readEvent(other._readEvent) { other._handle = INVALID_HANDLE; }
#else
	Socket(Socket &&other) : _handle(other._handle) { other._handle = INVALID_HANDLE; }
#endif
	~Socket() { close(); }

	Socket &operator=(Socket &&other)
	{
		close();
		_handle = other._handle;
#ifdef _WIN32
		_readEvent = other._readEvent;
#endif
		other._handle = INVALID_HANDLE;

		return *this;
	}

	// Abandons the socket, returning the abandoned handle.
	Handle abandon();

	// Accepts a new socket on a server socket. This call may return an invalid socket if a recoverable error is encountered.
	Socket accept();

	// Closes the socket.
	void close();

	// Creates a read event, if it does not already exist, that can be used to poll the socket for new data.
	// (On Windows the socket will automatically be placed in non-blocking mode, and the event is subject to spurious wake-ups since it is reset after a wait rather than after a read.)
	Event createReadEvent();

	// Gets the bound port of the socket, or zero if the socket is not valid.
	unsigned short getPort() const;

	// Checks if the socket has data to read.
	bool hasData() const;

	// Checks if the socket is valid.
	bool isValid() const { return _handle != INVALID_HANDLE; }

	/** Reads data from the socket, up to the size of the vector.
	 *
	 * @param data a vector to fill with data from the socket
	 * @param offset the offset into the vector to read data into
	 * @param waitAll true to wait until the buffer is completely filled with data before returning
	 * @return the number of bytes read from the socket
	 */
	std::size_t read(std::vector<std::uint8_t> &data, std::size_t offset = 0, bool waitAll = false);

	// Reads a string from the socket.
	std::string read(std::size_t length);

	/** Writes data to the socket, up to the size of the vector.
	 *
	 * @param data a vector of data to write to the socket
	 * @param offset the offset into the vector to write data from
	 * @return the number of bytes written to the socket
	 */
	std::size_t write(const std::vector<std::uint8_t> &data, std::size_t offset = 0);
};

} // k8psh

#endif // K8PSH_SOCKET_HXX
