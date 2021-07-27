// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#include <iostream>
#include <exception>

#define TEST_FAIL(X) do { ::std::cerr << X << " (" << __FILE__ << ":" << __LINE__ << ")" << ::std::endl; throw "failure"; } while (false)
#define TEST_THAT(X) do { bool failed = true; try { failed = bool(!(X)); } catch (const ::std::exception &e) { TEST_FAIL("Failed: " #X " with unexpected exception \"" << e.what() << "\""); } if (failed) TEST_FAIL("Failed: " #X); } while (false)
#define TEST_THROWS(X) do { try { (void)(X); TEST_FAIL("Failed to throw: " #X); } catch(...) { } } while (false)
#define TEST_DOESNT_THROW(X) do { try { (void)(X); } catch (const ::std::exception &e) { TEST_FAIL("Failed: " #X " with unexpected exception \"" << e.what() << "\""); } } while (false)
