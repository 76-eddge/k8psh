// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#include "Utilities.cxx"

#include <iostream>

#include "Test.hxx"

int main()
{
	// Working Directory / Hostname
	TEST_DOESNT_THROW(std::cout << "Executable: " << k8psh::Utilities::getExecutablePath() << std::endl);
	TEST_THAT(!k8psh::Utilities::getExecutablePath().empty());
	TEST_THAT(!k8psh::Utilities::readFile(k8psh::Utilities::getExecutablePath()).empty());
	TEST_DOESNT_THROW(std::cout << "Working Directory: " << k8psh::Utilities::getWorkingDirectory() << std::endl);
	TEST_THAT(!k8psh::Utilities::getWorkingDirectory().empty());
	TEST_THAT(k8psh::Utilities::getWorkingDirectory() == k8psh::Utilities::getAbsolutePath("."));
	TEST_THAT(k8psh::Utilities::getWorkingDirectory() == k8psh::Utilities::getAbsolutePath(""));
	TEST_DOESNT_THROW(std::cout << "Hostname: " << k8psh::Utilities::getHostname() << std::endl);
	TEST_THAT(!k8psh::Utilities::getHostname().empty());

	// Environment variables
	TEST_DOESNT_THROW(std::cout << "Path: " << k8psh::Utilities::getEnvironmentVariable("PATH") << std::endl);
	TEST_THAT(k8psh::Utilities::getEnvironmentVariable("PATH"));
	TEST_THAT(k8psh::Utilities::setEnvironmentVariable("PATH", "/bin"));
	TEST_THAT(k8psh::Utilities::getEnvironmentVariable("PATH") == "/bin");
	TEST_THAT(k8psh::Utilities::setEnvironmentVariable("PATH", k8psh::OptionalString()));
	TEST_THAT(!k8psh::Utilities::getEnvironmentVariable("PATH"));
	TEST_THAT(k8psh::Utilities::getEnvironmentVariable("PATH").empty());

	// Base Name
	TEST_THAT(k8psh::Utilities::getBasename("/usr/lib") == "lib");
	TEST_THAT(k8psh::Utilities::getBasename("/usr/") == "usr");
	TEST_THAT(k8psh::Utilities::getBasename("/") == "/");
	TEST_THAT(k8psh::Utilities::getBasename("///") == "/");
	TEST_THAT(k8psh::Utilities::getBasename("//usr//lib//") == "lib");
	TEST_THAT(k8psh::Utilities::getBasename("") == ".");
	TEST_THAT(k8psh::Utilities::getBasename("./bin/") == "bin");
	TEST_THAT(k8psh::Utilities::getBasename("./bin/k8psh") == "k8psh");
	TEST_THAT(k8psh::Utilities::getBasename("./k8psh") == "k8psh");

	// Relativize
	TEST_THAT(k8psh::Utilities::relativize("/blah//blah2//", "/blah/blah2/blah3") == "blah3");
	TEST_THAT(k8psh::Utilities::relativize("/blah/./blah2/.", "/./blah/blah2/blah3") == "blah3");
	TEST_THROWS(k8psh::Utilities::relativize("/blah//blah2", "/blah/blah2_blah3"));
	TEST_THROWS(k8psh::Utilities::relativize("/blah//blah2_blah3", "/blah/blah2"));

#ifdef _WIN32
	TEST_THAT(k8psh::Utilities::relativize("C:/Blah//blah2", "c:/blah/Blah2/blah3") == "blah3");
#endif
}
