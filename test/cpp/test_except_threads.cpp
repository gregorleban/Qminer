/* Copyright (C) Event Registry d.o.o. - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */

// Regression for TBufferStackWalker::GetStackTrace (ut.cpp): on Windows every EAssert* builds
// a stack trace through ONE global StackWalker whose output buffer is shared and whose DbgHelp
// symbol calls are single-threaded per process. Two threads throwing at the same time (the
// backend's omp parallel for over a kafka batch of bad articles, 2026-09-09) corrupted the heap
// (0xc0000374) and killed the process. GetStackTrace now serializes the walk under a mutex.
//
// The test throws from many threads at once for a while; it passes when every exception came
// back with a message and the process is still alive to count them. (On non-Windows builds no
// stack trace is collected and the test only exercises plain concurrent throwing.)
#include <base.h>
#include "gtest/gtest.h"
#include <thread>
#include <atomic>
#include <vector>

TEST(ExceptThreads, ConcurrentEAssertDoesNotCorruptTheHeap)
{
    const int Threads = 16;
    const int PerThread = 300;
    std::atomic<int> Caught(0);
    std::atomic<int> BadMsg(0);
    std::vector<std::thread> ThreadV;
    for (int ThreadN = 0; ThreadN < Threads; ThreadN++) {
        ThreadV.emplace_back(std::thread([&, ThreadN]() {
            for (int N = 0; N < PerThread; N++) {
                try {
                    EAssertR(false, TStr::Fmt("thread %d iteration %d", ThreadN, N));
                }
                catch (PExcept E) {
                    Caught++;
                    if (E->GetMsgStr() != TStr::Fmt("thread %d iteration %d", ThreadN, N)) { BadMsg++; }
                }
            }
        }));
    }
    for (std::thread& Thread : ThreadV) { Thread.join(); }
    EXPECT_EQ(Threads * PerThread, Caught.load());
    EXPECT_EQ(0, BadMsg.load());
}

// the trace itself must still be produced (the mutex must not have turned it off)
TEST(ExceptThreads, StackTraceIsStillCollected)
{
    try {
        EAssertR(false, "boom");
        FAIL() << "EAssertR did not throw";
    }
    catch (PExcept E) {
        EXPECT_EQ(TStr("boom"), E->GetMsgStr());
#ifdef GLib_WIN
        EXPECT_TRUE(E->GetLocStr().SearchStr("Stack trace:") >= 0) << E->GetLocStr().CStr();
#endif
    }
}
