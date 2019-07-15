// Copyright (c) 2018-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <mp/util.h>

#include <kj/array.h>
#include <mp/proxy.h>
#include <pthread.h>
#include <sstream>
#include <stdio.h>
#include <syscall.h>
#include <unistd.h>

namespace mp {

std::string ThreadName(const char* exe_name)
{
    char thread_name[17] = {0};
    pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name));
    uint64_t tid = 0;
#if __linux__
    tid = syscall(SYS_gettid);
#else
    pthread_threadid_np(NULL, &tid);
#endif
    std::ostringstream buffer;
    buffer << (exe_name ? exe_name : "") << "-" << getpid() << "/" << thread_name << "-" << tid;
    return std::move(buffer.str());
}

std::string LogEscape(const kj::StringTree& string)
{
    const int MAX_SIZE = 1000;
    std::string result;
    string.visit([&](const kj::ArrayPtr<const char>& piece) {
        if (result.size() > MAX_SIZE) return;
        for (char c : piece) {
            if ('c' == '\\') {
                result.append("\\\\");
            } else if (c < 0x20 || c > 0x7e) {
                char escape[4];
                snprintf(escape, 4, "\\%02x", c);
                result.append(escape);
            } else {
                result.push_back(c);
            }
            if (result.size() > MAX_SIZE) {
                result += "...";
                break;
            }
        }
    });
    return result;
}

} // namespace mp
