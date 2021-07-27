// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#include "Socket.cxx"

#include <iostream>

#include "Test.hxx"

#include "Utilities.cxx"

int main()
{
	k8psh::Socket::Initializer socketInit;
	std::cout << "Creating sockets" << std::endl;

	k8psh::Socket listener = k8psh::Socket::listen(k8psh::Socket::RANDOM_PORT);
	k8psh::Socket client;
	k8psh::Socket server;

	std::cout << "Listening on port " << listener.getPort() << std::endl;

	while (!client.isValid())
		client = k8psh::Socket::connect(listener.getPort());

	while (!server.isValid())
		server = listener.accept();

	std::vector<std::uint8_t> data = { 1, 2, 3, 'H', 'e', 'l', 'l', 'o', 5, 6, 7 };
	std::vector<std::uint8_t> received;

	// Test writing and reading
	std::cout << "Sending data" << std::endl;
	server.write(data);

	received.resize(4096);
	TEST_THAT(client.read(received) == data.size());

	for (std::size_t i = 0; i < data.size(); i++)
		TEST_THAT(received[i] == data[i]);

	// Test writing and reading with offsets
	std::cout << "Sending offset data" << std::endl;
	client.write(data, 3);
	TEST_THAT(server.read(5) == "Hello");

	static constexpr std::size_t DATA_OFFSET = 3 + 5;
	static constexpr std::size_t READ_OFFSET = 1;
	TEST_THAT(server.read(received, READ_OFFSET) == data.size() - DATA_OFFSET);

	for (std::size_t i = 0; i < data.size() - DATA_OFFSET; i++)
		TEST_THAT(received[i + READ_OFFSET] == data[i + DATA_OFFSET]);

	// Test closing
	std::cout << "Closing sockets" << std::endl;
	server.close();
	TEST_THAT(client.read(received) == 0);

	client.close();
	listener.close();

	std::cout << "Finished testing sockets" << std::endl;
}
