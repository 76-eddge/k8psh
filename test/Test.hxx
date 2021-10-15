// This software is released as part of the k8psh project.
// Portions of this software are public domain or licensed under the MIT license. (See the license file for details.)

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#define TEST_FAIL(X) do { ::std::ostringstream TEST_FAIL_ss_; TEST_FAIL_ss_ << X; ::std::string TEST_FAIL_message_ = TEST_FAIL_ss_.str(); ::std::cerr << TEST_FAIL_message_ << " (" << __FILE__ << ":" << __LINE__ << ")" << ::std::endl; throw ::std::runtime_error(TEST_FAIL_message_); } while (false)
#define TEST_THAT(X) do { bool TEST_THAT_failed_ = true; try { TEST_THAT_failed_ = bool(!(X)); } catch (const ::std::exception &TEST_DOESNT_THROW_e_) { TEST_FAIL("*** TEST FAILED *** " #X " with unexpected exception \"" << TEST_DOESNT_THROW_e_.what() << "\""); } if (TEST_THAT_failed_) TEST_FAIL("*** TEST FAILED *** " #X); } while (false)
#define TEST_THROWS(X) do { bool TEST_THROWS_threw_ = false; try { (void)(X); } catch (...) { TEST_THROWS_threw_ = true; } if (!TEST_THROWS_threw_) TEST_FAIL("*** TEST FAILED TO THROW *** " #X); } while (false)
#define TEST_DOESNT_THROW(X) do { try { (void)(X); } catch (const ::std::exception &TEST_DOESNT_THROW_e_) { TEST_FAIL("*** TEST FAILED *** " #X " with unexpected exception \"" << TEST_DOESNT_THROW_e_.what() << "\""); } } while (false)
