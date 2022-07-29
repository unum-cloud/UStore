#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <cstring> // `std::memcpy`

#include "ukv/ukv.h"

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

namespace {

struct remote_db_t {
    int socket;
    int io_context;
};

} // namespace
