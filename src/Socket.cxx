// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#include "Socket.hxx"

#ifdef _WIN32
	#include <winsock2.h>
	#include <fcntl.h>
	#include <io.h>
	#include <ws2ipdef.h>
#else
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <poll.h>
	#include <sys/socket.h>
	#include <unistd.h>
#endif

#include "Utilities.hxx"

k8psh::Socket::Initializer::Initializer()
{
#ifdef _WIN32
	WSADATA data;

	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		LOG_ERROR << "Failed to initialize socket system";
#endif
}

k8psh::Socket::Initializer::~Initializer()
{
#ifdef _WIN32
	(void)WSACleanup();
#endif
}

// Creates a new socket.
static int getSocketErrorCode()
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

// Creates a read event, if it does not already exist, that can be used to poll the socket for new data.
// (On Windows the socket will automatically be placed in non-blocking mode, and the event is subject to spurious wake-ups since it is reset after a wait rather than after a read.)
k8psh::Socket::Event k8psh::Socket::createReadEvent()
{
#ifdef _WIN32
	if (!_readEvent)
	{
		_readEvent = CreateEventA(NULL, FALSE, FALSE, NULL);

		if (!_readEvent)
			LOG_ERROR << "Failed to create socket read event: " << GetLastError();
		else if (WSAEventSelect(_handle, _readEvent, FD_ACCEPT | FD_READ | FD_CLOSE) != 0)
			LOG_ERROR << "Failed to initialize socket read event: " << WSAGetLastError();
	}

	return _readEvent;
#else
	return _handle;
#endif
}

// Creates a new socket.
static k8psh::Socket::Handle createSocketHandle()
{
#ifdef _WIN32
	k8psh::Socket::Handle handle = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
#else
	k8psh::Socket::Handle handle = socket(AF_INET, SOCK_STREAM, 0);
#endif

	if (handle == k8psh::Socket::INVALID_HANDLE)
		LOG_ERROR << "Failed to create socket: " << getSocketErrorCode();

	return handle;
}

#ifdef _WIN32
	#define IF_WINSOCK(X, Y) X
#else
	#define IF_WINSOCK(X, Y) Y
#endif

// Sets the specified option on a socket
template <typename OptionT, typename T> static bool setSocketOption(k8psh::Socket::Handle handle, int level, int name, const T &value)
{
	OptionT realValue = static_cast<OptionT>(value);

#ifdef _WIN32
	return setsockopt(handle, level, name, reinterpret_cast<const char *>(&realValue), int(sizeof(OptionT))) == 0;
#else
	return setsockopt(handle, level, name, &realValue, socklen_t(sizeof(OptionT))) == 0;
#endif
}

// Sets all common socket options on a socket
static void setSocketOptions(k8psh::Socket::Handle handle)
{
	// Disable Nagle's algorithm
	(void)setSocketOption<IF_WINSOCK(BOOL, int)>(handle, IPPROTO_TCP, TCP_NODELAY, 1);
}

// Creates a new server socket listening on the specified port.
k8psh::Socket k8psh::Socket::listen(unsigned short port)
{
	k8psh::Socket socket = createSocketHandle();
	sockaddr_in ipv4 = sockaddr_in();

	ipv4.sin_family = AF_INET;
	ipv4.sin_port = htons(port);

	char *addr = reinterpret_cast<char *>(&ipv4.sin_addr);
	addr[0] = 127;
	addr[3] = 1;

	LOG_DEBUG << "Binding to port " << port << " on socket " << socket._handle;

#ifndef _WIN32
	if (!setSocketOption<IF_WINSOCK(BOOL, int)>(socket._handle, SOL_SOCKET, SO_REUSEADDR, 1))
		LOG_ERROR << "Failed to set SO_REUSEADDR socket option: " << getSocketErrorCode();
#endif

	if (::bind(socket._handle, reinterpret_cast<sockaddr *>(&ipv4), int(sizeof(ipv4))) != 0)
		LOG_ERROR << "Failed to bind to port " << port << ": " << getSocketErrorCode();

	if (::listen(socket._handle, SOMAXCONN) != 0)
		LOG_ERROR << "Failed to listen on port " << port << ": " << getSocketErrorCode();

	LOG_DEBUG << "Bound to port " << port << " on socket " << socket._handle << ", listening for new connections";
	return socket;
}

// Creates a new socket by connecting to the specified port. This call may return an invalid socket if a recoverable error is encountered.
k8psh::Socket k8psh::Socket::connect(unsigned short port, bool failOnError)
{
	k8psh::Socket socket = createSocketHandle();
	sockaddr_in ipv4 = sockaddr_in();

	ipv4.sin_family = AF_INET;
	ipv4.sin_port = htons(port);

	char *addr = reinterpret_cast<char *>(&ipv4.sin_addr);
	addr[0] = 127;
	addr[3] = 1;

	LOG_DEBUG << "Connecting to port " << port << " on socket " << socket._handle;

	if (::connect(socket._handle, reinterpret_cast<sockaddr *>(&ipv4), int(sizeof(ipv4))) != 0)
	{
		int error = getSocketErrorCode();

#ifdef _WIN32
		if (failOnError && error != WSAEINTR && error != WSAENOBUFS)
#else
		if (failOnError && error != EINTR && error != ENOBUFS)
#endif
			LOG_ERROR << "Failed to connect to port " << port << ": " << error;

		LOG_DEBUG << "Failed to connect to port " << port << " on socket " << socket._handle;
		return INVALID_HANDLE;
	}

	setSocketOptions(socket._handle);
	LOG_DEBUG << "Connected to port " << port << " on socket " << socket._handle;
	return socket;
}

// Abandons the socket, returning the abandoned handle.
k8psh::Socket::Handle k8psh::Socket::abandon()
{
	Handle handle = _handle;

	_handle = INVALID_HANDLE;
	return handle;
}

// Accepts a new socket on a server socket. This call may return an invalid socket if a recoverable error is encountered.
k8psh::Socket k8psh::Socket::accept()
{
	LOG_DEBUG << "Waiting for connection on socket " << _handle;
	Handle handle = ::accept(_handle, NULL, NULL);

	if (handle == INVALID_HANDLE)
	{
		int error = getSocketErrorCode();

#ifdef _WIN32
		if (error != WSAECONNRESET && error != WSAEINTR && error != WSAEMFILE && error != WSAENOBUFS && error != WSAEWOULDBLOCK)
#else
		if (error != EAGAIN && error != EWOULDBLOCK && error != ECONNABORTED && error != EINTR && error != EMFILE && error != ENFILE && error != ENOBUFS && error != ENOMEM)
#endif
			LOG_ERROR << "Failed to accept connection: " << error;

		return INVALID_HANDLE;
	}

	setSocketOptions(handle);
	LOG_DEBUG << "Accepted new connection (" << handle << ") on socket " << _handle;
	return Socket(handle);
}

// Closes the socket.
void k8psh::Socket::close()
{
	if (_handle != INVALID_HANDLE)
	{
		Handle handle = _handle;

		_handle = INVALID_HANDLE;
		LOG_DEBUG << "Closing connection " << handle;

#ifdef _WIN32
		if (_readEvent)
			(void)CloseHandle(_readEvent);

		(void)shutdown(handle, SD_BOTH);
		(void)closesocket(handle);
#else
		(void)shutdown(handle, SHUT_RDWR);
		(void)::close(handle);
#endif
	}
}

// Gets the bound port of the socket, or zero if the socket is not valid.
unsigned short k8psh::Socket::getPort() const
{
	sockaddr_in ipv4;
#ifdef _WIN32
	int length = int(sizeof(ipv4));
#else
	socklen_t length = socklen_t(sizeof(ipv4));
#endif

	return getsockname(_handle, reinterpret_cast<sockaddr *>(&ipv4), &length) == 0 ? ntohs(ipv4.sin_port) : 0;
}

// Checks if the socket has data to read.
bool k8psh::Socket::hasData() const
{
#ifdef _WIN32
	fd_set fdSet;
	timeval timeout = { };

	FD_ZERO(&fdSet);
	FD_SET(_handle, &fdSet);

	return select(0, &fdSet, NULL, NULL, &timeout) == 1;
#else
	struct pollfd pollSet = { };

	pollSet.fd = _handle;
	pollSet.events = POLLIN;

	return poll(&pollSet, 1, 0) == 1;
#endif
}

/** Reads data from the socket.
 *
 * @param handle the socket handle
 * @param buffer the buffer to fill with data from the socket
 * @param length the number of bytes to read from the socket
 * @param waitAll true to wait until the buffer is completely filled with data before returning
 * @return the number of bytes read from the socket
 */
static std::size_t socketRead(k8psh::Socket::Handle handle, void *buffer, std::size_t length, bool waitAll)
{
	LOG_DEBUG << "Reading up to " << length << " bytes on socket " << handle;
	std::size_t totalReceived = 0;

	while (totalReceived < length)
	{
#ifdef _WIN32
		DWORD received = 0;
		DWORD recvflags = 0;
		WSABUF wsaBuffer = { static_cast<unsigned long>(length - totalReceived), reinterpret_cast<char *>(buffer) + totalReceived };
		int result = WSARecv(handle, &wsaBuffer, 1, &received, &recvflags, NULL, NULL);

		if (result != 0)
		{
			int error = getSocketErrorCode();

			if (error != WSAEWOULDBLOCK)
				LOG_ERROR << "Failed to read data from socket: " << error;
			else if (!waitAll)
				break;

			fd_set fdSet;

			FD_ZERO(&fdSet);
			FD_SET(handle, &fdSet);

			(void)select(0, &fdSet, NULL, NULL, NULL);
			continue;
		}
		else if (received)
			totalReceived += received;
#else
		ssize_t received = recv(handle, reinterpret_cast<char *>(buffer) + totalReceived, length - totalReceived, waitAll ? MSG_WAITALL : 0);

		if (received > 0)
			totalReceived += received;
		else if (received < 0)
			LOG_ERROR << "Failed to read data from socket: " << getSocketErrorCode();
#endif
		else
			break;

		if (!waitAll)
			break;
	}

	LOG_DEBUG << "Read " << totalReceived << " bytes on socket " << handle;
	return totalReceived;
}

/** Reads data from the socket, up to the size of the vector.
 *
 * @param data a vector to fill with data from the socket
 * @param offset the offset into the vector to read data into
 * @param waitAll true to wait until the buffer is completely filled with data before returning
 * @return the number of bytes read from the socket
 */
std::size_t k8psh::Socket::read(std::vector<std::uint8_t> &data, std::size_t offset, bool waitAll)
{
	if (offset >= data.size())
		return 0;

	return socketRead(_handle, &data[offset], data.size() - offset, waitAll);
}

/** Writes data to the socket, up to the ending index.
 *
 * @param data a vector of data to write to the socket
 * @param offset the offset into the vector to write data from
 * @param end the ending index (exclusive) of the data to write
 * @return the number of bytes written to the socket
 */
std::size_t k8psh::Socket::write(const std::vector<std::uint8_t> &data, std::size_t offset, std::size_t end)
{
	LOG_DEBUG << "Writing bytes " << offset << " - " << end << " on socket " << _handle;
	std::size_t totalSent = 0;

	while (offset < end)
	{
#ifdef _WIN32
		int length = int(end - offset);
#else
		std::size_t length = end - offset;
#endif
		auto sent = send(_handle, reinterpret_cast<const char *>(data.data() + offset), length, 0);

		if (sent == -1)
#ifdef _WIN32
		{
			int error = getSocketErrorCode();

			if (error != WSAEWOULDBLOCK)
				LOG_ERROR << "Failed to write data to socket: " << error;

			fd_set fdSet;

			FD_ZERO(&fdSet);
			FD_SET(_handle, &fdSet);

			(void)select(0, NULL, &fdSet, NULL, NULL);
			continue;
		}
#else
			LOG_ERROR << "Failed to write data to socket: " << getSocketErrorCode();
#endif

		totalSent += sent;
		offset += sent;
	}

	LOG_DEBUG << "Wrote " << totalSent << " bytes on socket " << _handle;
	return totalSent;
}
