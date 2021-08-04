// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#define TEST_FAIL(X) do { ::std::ostringstream ss; ss << X; ::std::string message = ss.str(); ::std::cerr << message << " (" << __FILE__ << ":" << __LINE__ << ")" << ::std::endl; throw ::std::runtime_error(message); } while (false)
#define TEST_THAT(X) do { bool failed_ = true; try { failed_ = bool(!(X)); } catch (const ::std::exception &e) { TEST_FAIL("*** TEST FAILED *** " #X " with unexpected exception \"" << e.what() << "\""); } if (failed_) TEST_FAIL("*** TEST FAILED *** " #X); } while (false)
#define TEST_THROWS(X) do { bool threw_ = false; try { (void)(X); } catch (...) { threw_ = true; } if (!threw_) TEST_FAIL("*** TEST FAILED TO THROW *** " #X); } while (false)
#define TEST_DOESNT_THROW(X) do { try { (void)(X); } catch (const ::std::exception &e) { TEST_FAIL("*** TEST FAILED *** " #X " with unexpected exception \"" << e.what() << "\""); } } while (false)
