/*
 *
 *    Copyright 2023 Jay Logue
 *    All rights reserved.
 *
 *    SPDX-License-Identifier: Apache-2.0
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *          Multi-threaded UDP Echo Client Tool
 */

#ifdef _WIN32
#define WINVER _WIN32_WINNT_WIN7
#define _WIN32_WINNT _WIN32_WINNT_WIN7
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif 

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits>
#include <climits>
#include <atomic>
#include <time.h>
#include <sched.h>
#include <getopt.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#endif

#include "TimeSpecUtils.hpp"

using namespace timespec_utils;

class UDPEchoClient;

typedef enum : uint32_t
{
    kTestState_Init,
    kTestState_Sending,
    kTestState_WindDown,
    kTestState_Done,
    kTestState_Error
} TestState;

constexpr size_t min_packet_size = (sizeof(uint64_t) + sizeof(uint32_t));

#ifdef _WIN32

typedef int socklen_t;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define SOCKET_BUF_FACTOR 1

#else

#define SOCKET_BUF_FACTOR 2

#endif


/*
 * Global variables
 */

// Test Configuration
static uint16_t sLocalPortBase = 50000;
static const char *sRemoteHostName = NULL;
static struct sockaddr_storage sRemoteAddr;
static int sRemoteAddrLen;
static int sSockFamily;
static uint16_t sRemotePort = 7;
static time_t sWindDownTime = 3; // seconds
static time_t sStatsInterval = 1; // seconds
static uint32_t sClientCount = 1;
static uint32_t sPacketSize = 20;
static struct timespec sTxInterval = { 1, 0 };
static uint32_t sTxCountPerInterval = 1;
static uint32_t sTxCountLimit = 0;
static time_t sTxTimeLimit = 10; // seconds
static int sTxBufSize = 0;
static int sRxBufSize = 0;
static bool sDisableAffinity = false;

// Internal State
static UDPEchoClient * sClients;
static std::atomic<TestState> sTestState(kTestState_Init);
static std::atomic_uint32_t sReadyThreadCount;
static std::atomic_uint32_t sPostedStatsCount;
static std::atomic_uint32_t sRemainingTxCount;
static struct timespec sTestStartTime;
static struct timespec sTxDoneTime;
static struct timespec sTestDoneTime;
static uint32_t sTotalTx = 0;
static uint32_t sTotalRx = 0;

#ifdef __WIN32__
static HANDLE sMainThreadEvent;
static uint64_t sPerfCounterFreq = 0;
#else
static pthread_t sMainThread;
#endif


/*
 * Utility Functions
 */

static inline void logActivity(const struct timespec &timeStamp, const char *msgF, ...)
{
    va_list args;
    struct timespec timeFromStart = timeStamp - sTestStartTime;

    va_start(args, msgF);
    fprintf(stdout, "%02d:%02d:%02d.%03d ",
            (int)(timeFromStart.tv_sec / (60 * 60)),
            (int)((timeFromStart.tv_sec / 60) % 60),
            (int)((timeFromStart.tv_sec) % 60),
            (int)(timeFromStart.tv_nsec / 1000000));
    vfprintf(stdout, msgF, args);
    fputs("\n", stdout);
    va_end(args);
}

static inline void logWarning(const char *msgF, ...)
{
    va_list args;

    va_start(args, msgF);
    fputs("WARNING: ", stderr);
    vfprintf(stderr, msgF, args);
    fputs("\n", stderr);
    va_end(args);
}

static inline void logError(const char *msgF, ...)
{
    va_list args;

    va_start(args, msgF);
    fputs("ERROR: ", stderr);
    vfprintf(stderr, msgF, args);
    fputs("\n", stderr);
    va_end(args);
}

static inline void logOSError(int err, const char *msgF, ...)
{
    va_list args;

    va_start(args, msgF);
    fputs("ERROR: ", stderr);
    vfprintf(stderr, msgF, args);
    fputs("\n", stderr);
    fputs(strerror(err), stderr);
    fputs("\n", stderr);
    va_end(args);
}

static inline void logOSError(const char *msgF, ...)
{
    va_list args;

    va_start(args, msgF);
    fputs("ERROR: ", stderr);
    vfprintf(stderr, msgF, args);
    fputs("\n", stderr);
    fputs(strerror(errno), stderr);
    fputs("\n", stderr);
    va_end(args);
}

static inline void logSocketError(const char *msgF, ...)
{
    va_list args;

    va_start(args, msgF);
    fputs("ERROR: ", stderr);
    vfprintf(stderr, msgF, args);
    fputs("\n", stderr);
#ifdef _WIN32
    char * errStr = NULL;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 
                  NULL, WSAGetLastError(),
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPSTR)&errStr, 0, NULL);
    fputs(errStr, stderr);
    LocalFree(errStr);
#else
    fputs(strerror(errno), stderr);
#endif
    fputs("\n", stderr);
    va_end(args);
}

#ifdef _WIN32

static inline void logWinError(const char *msgF, ...)
{
    char * errStr = NULL;
    va_list args;

    va_start(args, msgF);
    fputs("ERROR: ", stderr);
    vfprintf(stderr, msgF, args);
    fputs("\n", stderr);
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 
                  NULL, GetLastError(),
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPSTR)&errStr, 0, NULL);
    fputs(errStr, stderr);
    LocalFree(errStr);
    fputs("\n", stderr);
    va_end(args);
}

#endif

static inline void getNow(struct timespec &now)
{
#ifdef __WIN32__

    LARGE_INTEGER l;

    if (sPerfCounterFreq == 0) {
        QueryPerformanceFrequency(&l);
        sPerfCounterFreq = l.QuadPart;
    }

    QueryPerformanceCounter(&l);

    now.tv_sec = l.QuadPart / sPerfCounterFreq;
    now.tv_nsec = ((l.QuadPart % sPerfCounterFreq) * 1000000000L) / sPerfCounterFreq;
    if (now.tv_nsec > 1000000000L) {
        now.tv_sec++;
        now.tv_nsec -= 1000000000L;
    }

#else

    clock_gettime(CLOCK_MONOTONIC, &now);

#endif
}

#ifdef _WIN32

static inline void sleepMS(uint32_t durMS)
{
    ::SleepEx(durMS, FALSE);
}

#else

static inline void sleepMS(uint32_t durMS)
{
    struct timespec dur = { 0, durMS * 1000000 };
    clock_nanosleep(CLOCK_MONOTONIC, 0, &dur, NULL);
}

#endif

static inline void wakeMainThread(void)
{
#ifdef __WIN32__
    ::SetEvent(sMainThreadEvent);
#else
    if (!pthread_equal(pthread_self(), sMainThread))
        pthread_kill(sMainThread, SIGUSR1);
#endif
}

static inline bool testRunning(void)
{
    return sTestState == kTestState_Sending || sTestState == kTestState_WindDown;
}

static inline void txDone(struct timespec doneTime)
{
    // Record the time at which sending completed
    sTxDoneTime = doneTime;

    // Enter the Wind Down state
    TestState expectedState = kTestState_Sending;
    sTestState.compare_exchange_strong(expectedState, kTestState_WindDown);

    wakeMainThread();
}

static inline void testFailed(void)
{
    sTestState = kTestState_Error;
    wakeMainThread();
}


/** Packet transmission statistics class
 */
class TxStats final
{
public:
    inline TxStats()
    {
        mTxCount = 0;
        mTxFailedCount = 0;
        mPostTime.tv_sec = 0;
        mPostTime.tv_nsec = 0;
    }

    inline ~TxStats()
    {
    }

    inline uint32_t txCount() const
    {
        return mTxCount;
    }

    inline uint32_t txFailedCount() const
    {
        return mTxFailedCount;
    }

    inline void recordTx()
    {
        ++mTxCount;
    }

    inline void recordTxFailed()
    {
        ++mTxFailedCount;
    }

    inline struct timespec postTime(void) const
    {
        return mPostTime;
    }

    inline void setPostTime(struct timespec postTime)
    {
        mPostTime = postTime;
    }

    inline bool isPosted(void) const
    {
        return !is_zero(mPostTime);
    }

    inline const TxStats & operator+=(const TxStats &other)
    {
        mTxCount += other.mTxCount;
        mTxFailedCount += other.mTxFailedCount;
        if (!isPosted() || (other.isPosted() && mPostTime < other.mPostTime)) {
            mPostTime = other.mPostTime;
        }
        return *this;
    }

    inline void clear(void)
    {
        mTxCount = 0;
        mPostTime.tv_sec = 0;
        mPostTime.tv_nsec = 0;
    }

private:
    uint32_t mTxCount;
    uint32_t mTxFailedCount;
    struct timespec mPostTime;
};

/** Packet reception statistics class
 */
class RxStats final
{
public:
    RxStats()
    {
        mRxCount = 0;
        mRTTTable = NULL;
        mRTTTableSize = 0;
        mRTTTableSorted = false;
        mPostTime.tv_sec = 0;
        mPostTime.tv_nsec = 0;
        // TODO: pre-size mRTTTable
    }

    ~RxStats()
    {
        if (mRTTTable != NULL) {
            free(mRTTTable);
        }
    }

    const RxStats & operator+=(const RxStats &other)
    {
        ensureRTTTableSpace(other.mRxCount);
        memcpy(mRTTTable + mRxCount, other.mRTTTable, other.mRxCount * sizeof(uint64_t));
        mRxCount += other.mRxCount;
        mRTTTableSorted = false;
        if (!isPosted() || (other.isPosted() && mPostTime < other.mPostTime)) {
            mPostTime = other.mPostTime;
        }
        return *this;
    }

    inline void clear(void)
    {
        mRxCount = 0;
        mPostTime.tv_sec = 0;
        mPostTime.tv_nsec = 0;
    }

    void recordRx(uint64_t rttNS)
    {
        ensureRTTTableSpace(1);
        mRTTTable[mRxCount] = rttNS;
        mRxCount++;
        mRTTTableSorted = false;
    }

    inline struct timespec postTime(void) const
    {
        return mPostTime;
    }

    inline void setPostTime(struct timespec postTime)
    {
        mPostTime = postTime;
    }

    inline bool isPosted(void) const
    {
        return !is_zero(mPostTime);
    }

    inline uint32_t rxCount() const
    {
        return mRxCount;
    }

    double averageRTT() const
    {
        if (mRxCount == 0) {
            return 0.0;
        }
        uint64_t total = 0;
        for (size_t i = 0; i < mRxCount; i++) {
            total += mRTTTable[i];
        }
        return ((double)total) / mRxCount;
    }

    uint64_t minRTT() const
    {
        if (mRxCount == 0) {
            return 0;
        }
        uint64_t val = UINT64_MAX;
        for (size_t i = 0; i < mRxCount; i++) {
            if (val > mRTTTable[i]) {
                val = mRTTTable[i];
            }
        }
        return val;
    }

    uint64_t maxRTT() const
    {
        if (mRxCount == 0) {
            return 0;
        }
        uint64_t val = 0;
        for (size_t i = 0; i < mRxCount; i++) {
            if (val < mRTTTable[i]) {
                val = mRTTTable[i];
            }
        }
        return val;
    }

    uint64_t rttPercentile(uint8_t percentile)
    {
        if (mRxCount == 0) {
            return 0;
        }
        sortRTTTable();
        uint32_t valIndex = ((percentile * mRxCount) + 99) / 100;
        if (valIndex >= mRxCount) {
            valIndex = mRxCount - 1;
        }
        return mRTTTable[valIndex];
    }

private:
    uint32_t mRxCount;
    uint64_t * mRTTTable;
    size_t mRTTTableSize;
    bool mRTTTableSorted;
    struct timespec mPostTime;

    void ensureRTTTableSpace(size_t requestedSize)
    {
        const size_t kMinTableSize = 1024;

        if (mRTTTable == NULL || (mRxCount + requestedSize) >= mRTTTableSize) {

            size_t newTableSize = (mRTTTableSize < kMinTableSize) ? kMinTableSize : mRTTTableSize;

            while (newTableSize < (mRxCount + requestedSize))
                newTableSize *= 2;

            uint64_t *newTable = (uint64_t *)realloc(mRTTTable, newTableSize * sizeof(uint64_t));
            if (newTable == NULL) {
                logError("Stats::resize() failed (current size %u)", mRTTTableSize);
                exit(EXIT_FAILURE);
            }
            mRTTTable = newTable;
            mRTTTableSize = newTableSize;
        }
    }

    void sortRTTTable(void)
    {
        if (!mRTTTableSorted) {
            qsort(mRTTTable, mRxCount, sizeof(uint64_t), orderRTT);
            mRTTTableSorted = true;
        }
    }

    static int orderRTT(const void *a, const void *b)
    {
        const uint64_t *recA = (const uint64_t *)a;
        const uint64_t *recB = (const uint64_t *)b;

        if (recA < recB)
            return -1;
        if (recA > recB)
            return 1;
        return 0;
    }
};


/** UDP Echo Client class
 */
class UDPEchoClient final
{
public:
    UDPEchoClient()
    {
        mClientNum = 0;
#ifdef _WIN32
        mSocket = INVALID_SOCKET;
        mSocketEvent = NULL;
        mTxTimer = NULL;
        mTxThread = NULL;
        mRxThread = NULL;
#else
        mSocket = -1;
        mThreadsStarted = 0;
#endif
        mActiveTxStats = NULL;
        mPostedTxStats = NULL;
        mUnusedTxStats = NULL;
        mActiveRxStats = NULL;
        mPostedRxStats = NULL;
        mUnusedRxStats = NULL;
    }

    bool init(uint32_t clientNum)
    {
        struct sockaddr_storage remoteAddr = sRemoteAddr;
        uint16_t localPort = sLocalPortBase + clientNum;
        int res;

        mClientNum = clientNum;

        // Create the socket.
        mSocket = ::socket(sSockFamily, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
        if (mSocket == INVALID_SOCKET) {
#else
        if (mSocket < 0) {
#endif
            logSocketError("socket() failed");
            return false;
        }

#ifdef _WIN32

        // Create an event that will be signaled when a UDP packet arrives 
        // This also puts the socket in non-blocking mode
        mSocketEvent = ::WSACreateEvent();
        if (mSocketEvent == NULL) {
            logSocketError("WSACreateEvent() failed");
            shutdown();
            return false;
        }
        if (::WSAEventSelect(mSocket, mSocketEvent, FD_READ) == SOCKET_ERROR) {
            logSocketError("WSAEventSelect() failed");
            shutdown();
            return false;
        }

        // Create a timer for timing packet sends
        mTxTimer = ::CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (mTxTimer == NULL && GetLastError() == ERROR_INVALID_PARAMETER) {
            // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is not supported
            mTxTimer = ::CreateWaitableTimerEx(NULL, NULL, 0, TIMER_ALL_ACCESS);
        }
        if (mTxTimer == NULL) {
            logWinError("CreateWaitableTimerEx() failed");
            shutdown();
            return false;
        }

#else

        // Put the socket in non-blocking mode
        fcntl(mSocket, F_SETFL, fcntl(mSocket, F_GETFL, 0) | O_NONBLOCK);

#endif

        // Set the send buffer size.
        if (sTxBufSize != 0) {
            int val = sTxBufSize / SOCKET_BUF_FACTOR;
            res = setsockopt(mSocket, SOL_SOCKET, SO_SNDBUF, (const char *)&val, (socklen_t)sizeof(sTxBufSize));
            if (res != 0) {
                logSocketError("setsockopt(SO_SNDBUF) failed");
                shutdown();
                return false;
            }
            socklen_t optLen = (socklen_t)sizeof(val);
            res = getsockopt(mSocket, SOL_SOCKET, SO_SNDBUF, (char *)&val, &optLen);
            if (res != 0) {
                logSocketError("getsockopt(SO_SNDBUF) failed");
                shutdown();
                return false;
            }
            if (val != sTxBufSize) {
                logWarning("Send buffer size set to %d", val);
            }
        }

        // Set the receive buffer size.
        if (sRxBufSize != 0) {
            int val = sRxBufSize / SOCKET_BUF_FACTOR;
            res = setsockopt(mSocket, SOL_SOCKET, SO_RCVBUF, (const char *)&val, (socklen_t)sizeof(sRxBufSize));
            if (res != 0) {
                logSocketError("setsockopt(SO_RCVBUF) failed");
                shutdown();
                return false;
            }
            socklen_t optLen = (socklen_t)sizeof(val);
            res = getsockopt(mSocket, SOL_SOCKET, SO_RCVBUF, (char *)&val, (socklen_t *)&optLen);
            if (res != 0) {
                logSocketError("getsockopt(SO_RCVBUF) failed");
                shutdown();
                return false;
            }
            if (val != sRxBufSize) {
                logWarning("Receive buffer size set to %d", val);
            }
        }

        if (sSockFamily == AF_INET) {
            struct sockaddr_in bindAddr = { };
            bindAddr.sin_family = AF_INET;
            bindAddr.sin_addr.s_addr = INADDR_ANY;
            bindAddr.sin_port = htons(localPort);
            res = ::bind(mSocket, (struct sockaddr *)&bindAddr, sizeof(bindAddr));
        }
        else {
            struct sockaddr_in6 bindAddr = { };
            bindAddr.sin6_family = AF_INET6;
            bindAddr.sin6_addr = IN6ADDR_ANY_INIT;
            bindAddr.sin6_port = htons(localPort);
            res = ::bind(mSocket, (struct sockaddr *)&bindAddr, sizeof(bindAddr));
        }
        if (res != 0) {
            logSocketError("bind() failed");
            shutdown();
            return false;
        }

        // If running in send-to-self mode, arrange for the sending thread to send packets 
        // directly to the receiving thread.
        if (sRemotePort == 0) {
            if (sSockFamily == AF_INET) {
                ((struct sockaddr_in *)&remoteAddr)->sin_port = htons(localPort);
            }
            else {
                ((struct sockaddr_in6 *)&remoteAddr)->sin6_port = htons(localPort);
            }
        }

        // Connect the socket to the remote address
        res = ::connect(mSocket, (struct sockaddr *)&remoteAddr, sRemoteAddrLen);
        if (res != 0) {
            logSocketError("connect() failed");
            shutdown();
            return false;
        }

        mActiveTxStats = new TxStats();
        mPostedTxStats = new TxStats();
        mUnusedTxStats = new TxStats();

        mActiveRxStats = new RxStats();
        mPostedRxStats = new RxStats();
        mUnusedRxStats = new RxStats();

        return true;
    }

    void shutdown(void)
    {
#ifdef _WIN32
        if (mSocket != INVALID_SOCKET) {
            ::CloseHandle((HANDLE)mSocket);
            mSocket = INVALID_SOCKET;
        }
        if (mSocketEvent != NULL) {
            ::WSACloseEvent(mSocketEvent);
            mSocketEvent = NULL;
        }
        if (mTxTimer != NULL) {
            ::CloseHandle(mTxTimer);
            mTxTimer = NULL;
        }
#else
        if (mSocket >= 0) {
            ::close(mSocket);
            mSocket = -1;
        }
#endif

        delete mActiveTxStats; mActiveTxStats = NULL;
        delete mPostedTxStats; mPostedTxStats = NULL;
        delete mUnusedTxStats; mUnusedTxStats = NULL;

        delete mActiveRxStats; mActiveRxStats = NULL;
        delete mPostedRxStats; mPostedRxStats = NULL;
        delete mUnusedRxStats; mUnusedRxStats = NULL;
    }

    bool startThreads(void)
    {
#ifdef _WIN32

        mTxThread = (HANDLE)_beginthreadex(NULL, 0, 
            [](void * arg) __stdcall -> unsigned {
                ((UDPEchoClient *)arg)->txThreadMain();
                return 0;
            }, this, 0, NULL);
        if (mTxThread == NULL) {
            logOSError("_beginthreadex() failed");
            return false;
        }

        mRxThread = (HANDLE)_beginthreadex(NULL, 0, 
            [](void * arg) __stdcall -> unsigned {
                ((UDPEchoClient *)arg)->rxThreadMain();
                return 0;
            }, this, 0, NULL);
        if (mRxThread == NULL) {
            logOSError("_beginthreadex() failed");
            return false;
        }

#else

        int res;

        res = pthread_create(&mTxThread, NULL,
            [](void * arg) -> void * {
                ((UDPEchoClient *)arg)->txThreadMain();
                return NULL;
            }, this);
        if (res != 0) {
            logOSError(res, "pthread_create() failed");
            return false;
        }
        mThreadsStarted++;

        res = pthread_create(&mRxThread, NULL,
            [](void * arg) -> void * {
                ((UDPEchoClient *)arg)->rxThreadMain();
                return NULL;
            }, this);
        if (res != 0) {
            logOSError(res, "pthread_create() failed");
            return false;
        }
        mThreadsStarted++;

#endif // _WIN32

        return true;
    }

    void joinThreads(void)
    {
#ifdef _WIN32
        if (mTxThread != NULL) {
            ::WaitForSingleObject(mTxThread, INFINITE);
            mTxThread = NULL;
        }
        if (mRxThread != NULL) {
            ::WaitForSingleObject(mRxThread, INFINITE);
            mRxThread = NULL;
        }
#else
        if (mThreadsStarted == 2) {
            pthread_join(mRxThread, NULL);
            mThreadsStarted--;
        }

        if (mThreadsStarted == 1) {
            pthread_join(mTxThread, NULL);
            mThreadsStarted--;
        }
#endif
    }

    void getPostedStats(TxStats &totalTxStats, RxStats &totalRxStats)
    {
        TxStats * txStats = mUnusedTxStats;
        RxStats * rxStats = mUnusedRxStats;

        // Swap out the posted stats objects for an unused set of objects
        txStats = mPostedTxStats.exchange(txStats);
        rxStats = mPostedRxStats.exchange(rxStats);

        // Record that two of the posts stats objects have been consumed
        sPostedStatsCount -= 2;

        // sum the posted stats
        totalTxStats += *txStats;
        totalRxStats += *rxStats;

        // Clear the previously posted stats objects and make them available to be used again
        txStats->clear();
        rxStats->clear();
        mUnusedTxStats = txStats;
        mUnusedRxStats = rxStats;
    }

private:
    uint32_t mClientNum;
#ifdef _WIN32
    SOCKET mSocket;
    WSAEVENT mSocketEvent;
    HANDLE mTxTimer;
    HANDLE mTxThread;
    HANDLE mRxThread;
#else
    int mSocket;
    pthread_t mTxThread;
    pthread_t mRxThread;
    int mThreadsStarted;
#endif
    RxStats * mActiveRxStats;
    std::atomic<RxStats * > mPostedRxStats;
    RxStats * mUnusedRxStats;
    TxStats * mActiveTxStats;
    std::atomic<TxStats * > mPostedTxStats;
    TxStats * mUnusedTxStats;

    void txThreadMain(void)
    {
        struct timespec nextTxTime, nextStatsTime, now;
        uint32_t packetId = 0, txCountLimit;
        uint64_t packetTxTime;
        uint8_t *packetData = new uint8_t[sPacketSize];

#ifndef _WIN32
        char threadName[30];
        snprintf(threadName, sizeof(threadName), "UDP-TX-%02u", mClientNum);
        pthread_setname_np(pthread_self(), threadName);
#endif

        // Determine the total number of packets to be sent by this client.
        if (sTxCountLimit != 0) {
            txCountLimit = sTxCountLimit / sClientCount;
            if (mClientNum < (sTxCountLimit % sClientCount)) {
                txCountLimit++;
            }
        }
        else {
            txCountLimit = std::numeric_limits<uint32_t>::max();
        }

        // Fill unused space in the packet with the client number.
        memset(packetData, (int)mClientNum, sPacketSize);

#ifndef _WIN32
        // If not disabled, set processor affinity
        int numCPU = sysconf(_SC_NPROCESSORS_ONLN);
        if (!sDisableAffinity && numCPU > 1) {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(mClientNum % numCPU, &mask);
            int res = sched_setaffinity(0, sizeof(mask), &mask);
            if (res != 0) {
                logOSError("sched_setaffinity() failed");
                testFailed();
            }
        }
#endif

        // Wait for the test to start
        waitTestStart();

        // Determine the time at which the first stats will be posted
        nextStatsTime = (sStatsInterval != 0) ? sTestStartTime + sStatsInterval : timespec_max;

        // Delay sending the initial set of packets by a fraction of the send interval that
        // is proportional to the client number.  This has the effect of distributing the activity
        // of sending over the send interval such that multiple client are less likely to be
        // sending at the same time.
        nextTxTime = sTestStartTime + proportion(sTxInterval, mClientNum, sClientCount);

        // Loop until the main thread signals the test is complete...
        while (testRunning()) {

            getNow(now);

            if (nextStatsTime <= now) {
                postTxStats(nextStatsTime);
                nextStatsTime = nextStatsTime + sStatsInterval;
            }

            // If the time has arrived to send another set of packets...
            if (nextTxTime <= now) {

                // Send the requested number of packets...
                for (uint32_t i = 0; i < sTxCountPerInterval; i++) {

                    // Stop if past the sending end time
                    if (sTxDoneTime <= now) {
                        break;
                    }

                    // Stop sending if the send count limit has been reached
                    if (packetId == txCountLimit) {
                        break;
                    }

                    // Form a new packet
                    // Fill the packet with the send time (in ns since the start of the test)
                    // and the packet id.
                    packetTxTime = to_ns(now - sTestStartTime);
                    memcpy(packetData, &packetTxTime, sizeof(packetTxTime));
                    memcpy(packetData + sizeof(packetTxTime), &packetId, sizeof(packetId));

                    // Send the packet.
                    // If the send fails because there's no room in the socket's send buffer,
                    // record a send failure.
                    if (::send(mSocket, (const char *)packetData, sPacketSize, 0) < 0) {
#ifdef _WIN32
                        int res = WSAGetLastError();
                        if (res == WSAEWOULDBLOCK) {
                            mActiveTxStats->recordTxFailed();
                            break;
                        }
                        if (res != WSAECONNRESET) {
                            logSocketError("send() failed");
                            testFailed();
                            goto breakall;
                        }
#else
                        if (errno == EWOULDBLOCK) {
                            mActiveTxStats->recordTxFailed();
                            break;
                        }
                        if (errno != ECONNREFUSED) {
                            logSocketError("send() failed");
                            testFailed();
                            goto breakall;
                        }
#endif
                    }

                    // Record the packet as successfully sent.
                    mActiveTxStats->recordTx();

                    packetId++;

                    // If this thread has just sent the last packet, alert the main thread that sending is
                    // done.
                    if (packetId == txCountLimit) {
                        if ((sRemainingTxCount -= packetId) == 0) {
                            getNow(now);
                            txDone(now);
                        }
                    }
                }

                // Compute the time at which the next set of packets should be sent.
                nextTxTime = nextTxTime + sTxInterval;
            }

            struct timespec nextWakeTime = nextTxTime;
            if (nextStatsTime < nextWakeTime) {
                nextWakeTime = nextStatsTime;
            }

            // Wait until it is time to send another set of packets.
#ifdef _WIN32
            getNow(now);
            struct timespec dur = nextWakeTime - now;
            LARGE_INTEGER durL;
            durL.QuadPart = ((dur.tv_nsec + UINT64_C(99)) / UINT64_C(100)) + (dur.tv_sec * UINT64_C(10000000));
            if (!::SetWaitableTimer(mTxTimer, &durL, 0, NULL, NULL, 0)) {
                logWinError("SetWaitableTimer() failed");
                testFailed();
                break;
            }
            DWORD res = ::WaitForSingleObject(mTxTimer, INFINITE);
            if (res != WAIT_OBJECT_0) {
                logWinError("WaitForSingleObject() failed");
                testFailed();
                break;
            }
#else
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextWakeTime, NULL);
#endif
        }
breakall:

        if (sTestState == kTestState_Done && sStatsInterval == 0) {
            postTxStats(sTestDoneTime);
        }

        delete [] packetData;
    }

    void rxThreadMain(void)
    {
        struct timespec nextStatsTime, now, sleepTime;
        uint64_t packetTxTime, packetRTT;
        uint8_t *packetData = new uint8_t[sPacketSize];
        const struct timespec statePollInterval = { 0, 1000000 };
        bool rxReady = false;

#ifndef _WIN32        
        char threadName[30];
        snprintf(threadName, sizeof(threadName), "UDP-RX-%02u", mClientNum);
        pthread_setname_np(pthread_self(), threadName);
#endif

#ifndef _WIN32
        // If not disabled, set processor affinity
        int numCPU = sysconf(_SC_NPROCESSORS_ONLN);
        if (!sDisableAffinity && numCPU > 1) {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(mClientNum % numCPU, &mask);
            int res = sched_setaffinity(0, sizeof(mask), &mask);
            if (res != 0) {
                logOSError("sched_setaffinity() failed");
                testFailed();
            }
        }
#endif

        // Wait for the test to start
        waitTestStart();

        // Determine the time at which the first stats will be posted
        nextStatsTime = (sStatsInterval != 0) ? sTestStartTime + sStatsInterval : timespec_max;

        // Loop until the main thread signals the test is complete...
        while (testRunning()) {

            getNow(now);

            if (nextStatsTime <= now) {
                postRxStats(nextStatsTime);
                nextStatsTime = nextStatsTime + sStatsInterval;
            }

            // If packets are available to be received...
            if (rxReady) {

                // Continue receiving packets until all queued packets have been received or
                // the main thread signals the test is complete.
                while (testRunning()) {

#ifdef _WIN32
                    // Reset the socket event immediately before attempting to read
                    ::WSAResetEvent(mSocketEvent);
#endif

                    // Attempt to receive a packet.
                    ssize_t recvRes = ::recv(mSocket, (char *)packetData, sPacketSize, 0);
                    if (recvRes < 0) {
#ifdef _WIN32
                        int res = WSAGetLastError();
                        if (res == WSAECONNRESET) {
                            continue;
                        }
                        if (res == WSAEWOULDBLOCK) {
                            break;
                        }
#else
                        if (errno == ECONNREFUSED || errno == EAGAIN) {
                            break;
                        }
#endif
                        logSocketError("recv() failed");
                        testFailed();
                        goto breakall;
                    }

                    // Mark the receive time for the packet.
                    getNow(now);

                    // If the packet data length is correct...
                    if (recvRes == sPacketSize) {

                        // Extract the send time from the packet.
                        memcpy(&packetTxTime, packetData, sizeof(packetTxTime));

                        // Compute the round trip time.
                        packetRTT = to_ns(now - sTestStartTime) - packetTxTime;

                        // Record the packet receive.
                        mActiveRxStats->recordRx(packetRTT);
                    }
                }
            }

            sleepTime = nextStatsTime - now;
            if (statePollInterval < sleepTime) {
                sleepTime = statePollInterval;
            }

            // Wait until an incoming packet is ready to be received.
#ifdef _WIN32
            DWORD res = ::WaitForSingleObject(mSocketEvent, (DWORD)to_ms(sleepTime));
            if (res == WAIT_FAILED) {
                logWinError("WaitForSingleObject() failed");
                testFailed();
                break;
            }
            rxReady = (res == WAIT_OBJECT_0); // true if a packet is ready to be received
#else
            fd_set readFDs;
            FD_ZERO(&readFDs);
            FD_SET(mSocket, &readFDs);
            int res = pselect(mSocket + 10, &readFDs, NULL, NULL, &sleepTime, NULL);
            if (res < 0) {
                logOSError("select() failed");
                testFailed();
                break;
            }
            rxReady = (res != 0);
#endif
        }
breakall:

        if (sTestState == kTestState_Done && sStatsInterval == 0) {
            postRxStats(sTestDoneTime);
        }

        delete [] packetData;
    }

    void waitTestStart(void)
    {
        // Inform the main thread that this thread is ready
        sReadyThreadCount++;

        // Wait for the test to start
        while (sTestState == kTestState_Init) {
            sleepMS(1);
        }
    }

    void postTxStats(struct timespec postTime)
    {
        static std::atomic_bool warningLogged(false);

        mActiveTxStats->setPostTime(postTime);

        // Post the current send stats, retrieving a new stats object in return.
        mActiveTxStats = mPostedTxStats.exchange(mActiveTxStats);

        // If the main thread hasn't processed the previously posted stats, print an error
        // and discard the stats.
        if (mActiveTxStats->isPosted()) {
            if (!warningLogged.exchange(true)) {
                logError("WARNING: Posted send stats not processed in time; stats will be inaccurate");
            }
            mActiveTxStats->clear();
        }

        // Track the number of stats that have posted and wake the main thread when all are ready
        // to be consumed.
        else {
            if (++sPostedStatsCount >= (sClientCount * 2)) {
                wakeMainThread();
            }
        }
    }

    void postRxStats(struct timespec postTime)
    {
        static std::atomic_bool warningLogged(false);

        mActiveRxStats->setPostTime(postTime);

        // Post the current receive stats, retrieving a new stats object in return.
        mActiveRxStats = mPostedRxStats.exchange(mActiveRxStats);

        // If the main thread hasn't processed the previously posted stats, print an error
        // and discard the stats data.
        if (mActiveRxStats->isPosted()) {
            if (!warningLogged.exchange(true)) {
                logError("WARNING: Posted receive stats not processed in time; stats will be inaccurate");
            }
            mActiveRxStats->clear();
        }

        else {
            // Track the number of stats that have posted and wake the main thread when all are ready
            // to be consumed.
            if (++sPostedStatsCount >= (sClientCount * 2)) {
                wakeMainThread();                
            }
        }
    }
};

static uint32_t parseUInt32Option(const char *name, const char *arg, int base, uint32_t minVal, uint32_t maxVal)
{
    uint32_t val;
    char *parseEnd;

    if (strchr(arg, '-') == NULL) {
        errno = 0;
        val = strtoul(arg, &parseEnd, base);

        if (parseEnd > arg && *parseEnd == 0 && (val != ULONG_MAX || errno == 0)) {
            if (val < minVal || val > maxVal) {
                logError("Invalid value specified for %s: Value out of range (%s)", name, arg);
                exit(EXIT_FAILURE);
            }
            return val;
        }
    }

    if (*arg == 0) {
        arg = "(empty)";
    }
    logError("Invalid value specified for %s: %s", name, arg);

    exit(EXIT_FAILURE);
}

static void getOptName(int opt, int argc, char * argv[], struct option * longOpts, char * buf, size_t bufSize)
{
    if (opt == '?') {
        if (optopt == 0) {
            snprintf(buf, bufSize, "%s", argv[optind - 1]);
        }
        else {
            snprintf(buf, bufSize, "-%c", optopt);
        }
        return;
    }
    if (opt == ':') {
        opt = optopt;
    }
    for (; longOpts && longOpts->name != NULL; longOpts++) {
        if (longOpts->flag == NULL && longOpts->val == opt) {
            snprintf(buf, bufSize, "--%s", longOpts->name);
            return;
        }
    }
    snprintf(buf, bufSize, "-%c", opt);
}

static void DisplayHelp(void)
{
    printf(
"Usage: UDPEchoClient [<option>...] <remote-host> [<remote-port>]\n"
"\n"
"UDPEchoClient -- Generate and time packet transmission through a UDP echo server.\n"
"\n"
"  -t, --threads=int           Number of send/receive thread pairs. Defaults to 1.\n"
"  -c, --send-count=int        Number of packets that each sending thread sends at\n"
"                              each send interval. Defaults to 1.\n"
"  -i, --send-interval=int     Interval between sending, in microseconds.  Defaults\n"
"                              to 1000000 (1 second).\n"
"  -T, --send-time=int         Total amount of time to spend sending packets, in\n"
"                              seconds.  Defaults to 10.\n"
"  -L, --send-limit=int        Maximum number of packets to send across all threads.\n"
"                              Defaults to unlimited.\n"
"  -s, --packet-size=int       Size of the UDP packet payload in bytes. Defaults to 20.\n"
"  -w, --wind-down=int         Amount of time to continuing waiting for packets\n"
"                              after sending completes, in seconds. Defaults to 3.\n"
"  -l, --local-port=int        Base port number for local sockets. Each send/receive\n"
"                              thread pair increments this number by 1. Defaults to\n"
"                              50000.\n"
"  -r, --stats-interval=int    Interval at which send/receive statistics are reported.\n"
"                              Defaults to 1.  Specify 0 to disable periodic reporting.\n"
"  -n, --no-affinity           Disable use of processor affinity.\n"
"  -R, --receive-buf-size=int  Size of the UDP socket receive buffer, in bytes.\n"
"                              Defaults to system default.\n"
"  -S, --send-buf-size=int     Size of the UDP socket send buffer, in bytes.  Defaults\n"
"                              to system default.\n"
"  -h, --help                  Display the help message.\n"
"\n"
"Reported Statistics:\n"
"\n"
"  tx           Number of packets sucessfully sent by all threads during the\n"
"               reporting interval.\n"
"\n"
"  tx-fail      Number of attempts to send a packet that failed due to lack\n"
"               of buffer space in the kernel, during the reporting interval.\n"
"\n"
"  rx           Number of packets received by all threads during the reporting\n"
"               interval.\n"
"\n"
"  rx-pending   Cumulative number of packets that were sent, but for which a\n"
"               response packet has yet to be received.\n"
"\n"
"  min-rtt      Minimum measured round-trip time of all packets sent and\n"
"               received during the reporting interval, in miliseconds.\n"
"\n"
"  max-rtt      Maximum measured round-trip time of all packets sent and\n"
"               received during the reporting interval, in miliseconds.\n"
"\n"
"  avg-rtt      Average measured round-trip time of all packets sent and\n"
"               received during the reporting interval, in miliseconds.\n"
"\n"
"  p90-rtt      The 90 percentile round-trip time of the fastest packets sent\n"
"               and received during the reporting interval, in milliseconds.\n"
"               90%% of the round-trip times measured during the reporting\n"
"               interval were at or below this time.\n"
"\n"
"  p99-rtt      The 99 percentile round-trip time of the fastest packets sent\n"
"               and received during the reporting interval, in milliseconds.\n"
"               99%% of the round-trip times measured during the reporting\n"
"               interval were at or below this time.\n"
"\n"
"  total-tx     Number of packets sucessfully sent during the run.\n"
"\n"
"  total-rx     Number of packets received during the run.\n"
"\n"
"  total-lost   Number and percentage of packets sucessfully sent for which a\n"
"               response was not received.\n"
"\n"
    );
}

static void parseArgs(int argc, char *argv[])
{
    constexpr int IS_LONG_OPT = 0x0100;

    static struct option longOpts[] = {
        { "threads",            required_argument,  0, IS_LONG_OPT | 't' },
        { "local-port",         required_argument,  0, IS_LONG_OPT | 'l' },
        { "wind-down",          required_argument,  0, IS_LONG_OPT | 'w' },
        { "send-time",          required_argument,  0, IS_LONG_OPT | 'T' },
        { "send-limit",         required_argument,  0, IS_LONG_OPT | 'L' },
        { "send-interval",      required_argument,  0, IS_LONG_OPT | 'i' },
        { "send-count",         required_argument,  0, IS_LONG_OPT | 'c' },
        { "packet-size",        required_argument,  0, IS_LONG_OPT | 's' },
        { "send-buf-size",      required_argument,  0, IS_LONG_OPT | 'S' },
        { "receive-buf-size",   required_argument,  0, IS_LONG_OPT | 'R' },
        { "stats-interval",     required_argument,  0, IS_LONG_OPT | 'r' },
        { "no-affinity",        no_argument,        0, IS_LONG_OPT | 'n' },
        { "help",               no_argument,        0, IS_LONG_OPT | 'h' },
        {0, 0, 0, 0}
    };
    static const char shortOpts[] = ":t:l:w:T:L:i:c:d:S:R:r:hn";

    optind = 1;
    opterr = 0;
    while (true) {
        int opt = getopt_long(argc, argv, shortOpts, longOpts, NULL);
        if (opt < 0) {
            break;
        }

        char optName[64];
        getOptName(opt, argc, argv, longOpts, optName, sizeof(optName));

        if (opt == '?') {
            logError("Unknown option '%s'", optName);
            exit(EXIT_FAILURE);
        }

        if (opt == ':') {
            logError("Missing argument for %s option", optName);
            exit(EXIT_FAILURE);
        }

        switch (opt & ~IS_LONG_OPT) {
        case 't':
            sClientCount = parseUInt32Option(optName, optarg, 10, 1, 1000);
            break;
        case 'l':
            sLocalPortBase = (uint16_t)parseUInt32Option(optName, optarg, 10, 1, UINT16_MAX);
            break;
        case 'w':
            sWindDownTime = parseUInt32Option(optName, optarg, 10, 0, UINT32_MAX);
            break;
        case 'L':
            sTxCountLimit = parseUInt32Option(optName, optarg, 10, 0, UINT32_MAX);
            break;
        case 'T':
            sTxTimeLimit = parseUInt32Option(optName, optarg, 10, 0, UINT32_MAX);
            break;
        case 'i': {
            uint32_t sendIntervalUS = parseUInt32Option(optName, optarg, 10, 0, UINT32_MAX);
            sTxInterval.tv_sec = sendIntervalUS / 1000000;
            sTxInterval.tv_nsec = (sendIntervalUS % 1000000) * 1000;
            break;
        }
        case 'c':
            sTxCountPerInterval = parseUInt32Option(optName, optarg, 10, 0, UINT32_MAX);
            break;
        case 's':
            sPacketSize = parseUInt32Option(optName, optarg, 10, min_packet_size, UINT16_MAX);
            break;
        case 'S':
            sTxBufSize = parseUInt32Option(optName, optarg, 10, 0, UINT32_MAX);
            break;
        case 'R':
            sRxBufSize = parseUInt32Option(optName, optarg, 10, 0, UINT32_MAX);
            break;
        case 'r':
            sStatsInterval = parseUInt32Option(optName, optarg, 10, 0, UINT32_MAX);
            break;
        case 'n':
            sDisableAffinity = true;
            break;
        case 'h':
            DisplayHelp();
            exit(EXIT_SUCCESS);
        }
    }

    char **remainingArgs = argv + optind;
    int remainingArgCount = argc - optind;
    if (remainingArgCount == 0) {
        logError("Please specify the remote address");
        exit(EXIT_FAILURE);
    }
    if (remainingArgCount > 2) {
        logError("Unknown argument: %s", remainingArgs[2]);
        exit(EXIT_FAILURE);
    }
    if (strcmp(remainingArgs[0], "-") == 0) {
        sRemoteHostName = "127.0.0.1";
        sRemotePort = 0; // send-to-self mode
    }
    else {
        sRemoteHostName = remainingArgs[0];
        if (remainingArgCount > 1) {
            if (strcmp(remainingArgs[1], "-") == 0) {
                sRemotePort = 0; // send-to-self mode
            }
            else {
                sRemotePort = parseUInt32Option("remote port", remainingArgs[1], 10, 1, UINT16_MAX);
            }
        }
    }
}

static bool getRemoteAddress(void)
{
    int res;
    struct addrinfo hints;
    struct addrinfo *addrInfoList;
    struct addrinfo *remoteAddrInfo = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    res = getaddrinfo(sRemoteHostName, NULL, &hints, &addrInfoList);
    if (res != 0) {
        logError("%s\nUnable to resolve %s\n", gai_strerror(res), sRemoteHostName);
        return false;
    }

    for (struct addrinfo *p = addrInfoList; p != NULL; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            remoteAddrInfo = p;
            break;
        }
        if (p->ai_family == AF_INET6) {
            if (remoteAddrInfo == NULL) {
                remoteAddrInfo = p;
            }
        }
    }

    if (remoteAddrInfo == NULL) {
        logError("Unable to resolve %s\nNo matching address", sRemoteHostName);
        return false;
    }

    memcpy(&sRemoteAddr, remoteAddrInfo->ai_addr, remoteAddrInfo->ai_addrlen);
    sRemoteAddrLen = remoteAddrInfo->ai_addrlen;
    sSockFamily = remoteAddrInfo->ai_family;

    // Setup the remote port
    if (sSockFamily == AF_INET) {
        ((struct sockaddr_in *)&sRemoteAddr)->sin_port = htons(sRemotePort);
    }
    else {
        ((struct sockaddr_in6 *)&sRemoteAddr)->sin6_port = htons(sRemotePort);
    }

    return true;
}

static void logStats(void)
{
    TxStats txStats;
    RxStats rxStats;

    // Sum up the currently posted stats from each of the clients
    for (uint32_t i = 0; i < sClientCount; i++) {
        sClients[i].getPostedStats(txStats, rxStats);
    }

    // Keep a running total of the packets send and received
    sTotalTx += txStats.txCount();
    sTotalRx += rxStats.rxCount();

    // Determine how many send packets are currently waiting to be received
    uint32_t rxPending = (sTotalTx > sTotalRx) ? sTotalTx - sTotalRx : 0;

    logActivity(txStats.postTime(),
                "tx:%u tx-fail:%u rx:%u rx-pending:%u min-rtt:%.03f max-rtt:%.03f avg-rtt:%.03f p90-rtt:%.03f p99-rtt:%.03f",
                txStats.txCount(),
                txStats.txFailedCount(),
                rxStats.rxCount(),
                rxPending,
                ((double)rxStats.minRTT()) / 1000000,
                ((double)rxStats.maxRTT()) / 1000000,
                ((double)rxStats.averageRTT()) / 1000000,
                ((double)rxStats.rttPercentile(90)) / 1000000,
                ((double)rxStats.rttPercentile(99)) / 1000000);
}

int main(int argc, char *argv[])
{
    struct timespec now, nextWakeTime, sleepTime;
    int sendDoneReported = 0;

    // Parse the supplied command-line arguments.
    parseArgs(argc, argv);

    if (sLocalPortBase > (std::numeric_limits<uint16_t>::max() - sClientCount)) {
        logError("Invalid values specified for threads and local-port options\nSpecified number of threads would cause port number to exceed maximum");
        exit(EXIT_FAILURE);
    }

#ifdef _WIN32
    WSADATA wsaData;
    int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (res != 0) {
        logSocketError("WSAStartup() failed");
        exit(EXIT_FAILURE);
    }
#endif

    // Parse the supplied remote address (if any) and determine whether to use IPv4 or IPv6.
    if (!getRemoteAddress()) {
        return EXIT_FAILURE;
    }

    sRemainingTxCount = sTxCountLimit;

    // Initialize the set of client objects
    sClients = new UDPEchoClient[sClientCount];
    for (uint32_t i = 0; i < sClientCount; i++) {
        if (!sClients[i].init(i)) {
            return EXIT_FAILURE;
        }
    }

#ifdef _WIN32

    // Create an auto-reset event that the client threads can use to wake the main thread
    sMainThreadEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
    if (sMainThreadEvent == NULL) {
        logError("CreateEvent() failed");
        exit(EXIT_FAILURE);
    }

#else // _WIN32

    // Setup SIGUSR1 as a means for the client threads to wake the main thread
    {
        sigset_t usr1SigSet;
        struct sigaction usr1SigAction = { };

        // Configure a no-op signal handler
        usr1SigAction.sa_handler = [](int signum) { /* no-op */};
        sigemptyset(&usr1SigAction.sa_mask);
        sigaction(SIGUSR1, &usr1SigAction, NULL);

        // Block reception of SIGUSR1 until pselect is called below
        sigemptyset(&usr1SigSet);
        sigaddset(&usr1SigSet, SIGUSR1);
        sigprocmask(SIG_BLOCK, &usr1SigSet, NULL);

        sMainThread = pthread_self();
    }

#endif // _WIN32

    // Launch the client threads.
    for (uint32_t i = 0; i < sClientCount; i++) {
        if (!sClients[i].startThreads()) {
            return EXIT_FAILURE;
        }
    }

    // Wait for all threads to be ready
    while (sReadyThreadCount < (sClientCount * 2)) {
        if (sTestState == kTestState_Error) {
            return EXIT_FAILURE;
        }
        sleepMS(1);
    }

    // Arrange for the test to start 100ms in the future
    getNow(now);
    sTestStartTime = now + timespec { 0, 100000000 };
    sTxDoneTime = (sTxTimeLimit != 0) ? sTestStartTime + sTxTimeLimit : timespec_max;

    // Inform the client threads that the test is starting
    sTestState = kTestState_Sending;

    // Report that the test has started
    {
        const void * addr = (sSockFamily == AF_INET) 
            ? (const void *)&((struct sockaddr_in *)&sRemoteAddr)->sin_addr 
            : (const void *)&((struct sockaddr_in6 *)&sRemoteAddr)->sin6_addr;
        char addrStrBuf[INET6_ADDRSTRLEN];
        char portStrBuf[20];

        inet_ntop(sSockFamily, addr, addrStrBuf, sizeof(addrStrBuf));
        if (sRemotePort != 0) {
            snprintf(portStrBuf, sizeof(portStrBuf), "port %" PRIu16, sRemotePort);
        }
        else {
            strncpy(portStrBuf, "send-to-self", sizeof(portStrBuf));
        }
        logActivity(sTestStartTime, "TEST STARTING (target: %s, %s)", addrStrBuf, portStrBuf);
    }

    // Loop until the test is complete...
    while (testRunning()) {

        // If all clients have posted stats, print a summary now.
        if (sPostedStatsCount == (sClientCount * 2)) {
            logStats();
        }

        nextWakeTime = timespec_max;

        // If sending is not complete, and a send time limit has been specified, stop sending
        // when the specified time has been reached.  Otherwise arrange to wake when sending
        // should stop.
        if (sTestState == kTestState_Sending) {
            if (sTxDoneTime <= now) {
                txDone(sTxDoneTime);
            }
            else {
                nextWakeTime = sTxDoneTime;
            }
        }

        // If sending is complete...
        if (sTestState == kTestState_WindDown) {

            // Report when sending completed
            if (!sendDoneReported) {
                logActivity(sTxDoneTime, "SENDING DONE");
                sendDoneReported = 1;
            }

            // If the Wind Down time has passed, mark the test as done and stop the loop
            struct timespec windDownEndTime = sTxDoneTime + sWindDownTime;
            if (windDownEndTime <= now) {
                sTestDoneTime = windDownEndTime;
                sTestState  = kTestState_Done;
                break;
            }

            nextWakeTime = windDownEndTime;
        }

        sleepTime = nextWakeTime - now;

        // Wait for the test state to progress or a signal from the client threads
#ifdef _WIN32
        ::WaitForSingleObject(sMainThreadEvent, to_ms(sleepTime));
#else // _WIN32
        sigset_t emptySigSet;
        sigemptyset(&emptySigSet);
        pselect(0, NULL, NULL, NULL, &sleepTime, &emptySigSet);
#endif // _WIN32

        getNow(now);
    }

    // Wait for all client threads to terminate
    for (uint32_t i = 0; i < sClientCount; i++) {
        sClients[i].joinThreads();
    }

    // If something failed, simply exit
    if (sTestState == kTestState_Error) {
        return EXIT_FAILURE;
    }

    // Report final stats
    if (sStatsInterval == 0) {
        logStats();
    }

    // Log that the test is complete
    uint32_t totalLost = sTotalTx - sTotalRx;
    double totalLostPercent = (sTotalTx != 0) ? ((totalLost * 100.0) / sTotalTx) : 0;
    logActivity(sTestDoneTime, 
                "TEST COMPLETE total-tx:%u total-rx:%u total-lost:%u (%.03f%%)",
                sTotalTx,
                sTotalRx,
                totalLost,
                totalLostPercent);

    // Shutdown all the clients
    for (uint32_t i = 0; i < sClientCount; i++) {
        sClients[i].shutdown();
    }

    // Return success if no packets were lost
    return (sTotalTx == sTotalRx) ? EXIT_SUCCESS : EXIT_FAILURE;
}
