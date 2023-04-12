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

/*
 * Very simple C++ functions for working with timespec structures
 *
 * NOTE:
 *    - Negative time values are not supported
 *    - Input timespec values must be in normalized form, i.e. where
 *      tv_nsec < 1000000000UL.
 * 
 */

#ifndef __TimeSpecUtils_hpp__
#define __TimeSpecUtils_hpp__

#include <stdint.h>
#include <time.h>
#include <limits>

namespace timespec_utils {

constexpr struct timespec timespec_zero     = { 0, 0 };
constexpr struct timespec timespec_max      = { std::numeric_limits<time_t>::max(), 999999999UL };

static inline void normalize(struct timespec &v)
{
    v.tv_sec += v.tv_nsec / 1000000000UL;
    v.tv_nsec = v.tv_nsec % 1000000000UL;
}

static inline struct timespec operator+(const struct timespec &a, const struct timespec &b)
{
    struct timespec res = a;
    res.tv_sec += b.tv_sec;
    res.tv_nsec += b.tv_nsec;
    normalize(res);
    return res;
}

static inline struct timespec operator+(const struct timespec &a, time_t b)
{
    struct timespec res = a;
    res.tv_sec += b;
    return res;
}

static inline struct timespec operator-(const struct timespec &a, const struct timespec &b)
{
    struct timespec res;
    if (a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec <= b.tv_nsec)) {
        res = timespec_zero;
    }
    else {
        res.tv_sec = a.tv_sec - b.tv_sec;
        if (a.tv_nsec < b.tv_nsec) {
            res.tv_sec--;
            res.tv_nsec = a.tv_nsec + 1000000000UL - b.tv_nsec;
        }
        else {
            res.tv_nsec = a.tv_nsec - b.tv_nsec;
        }
    }
    return res;
}

static inline struct timespec operator-(const struct timespec &a, time_t b)
{
    struct timespec res;
    if (a.tv_sec < b) {
        res = timespec_zero;
    }
    else {
        res.tv_sec -= b;
    }
    return res;
}

static inline bool operator==(const struct timespec &a, const struct timespec &b)
{
    return (a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec);
}

static inline bool operator<(const struct timespec &a, const struct timespec &b)
{
    return (a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec));
}

static inline bool operator<=(const struct timespec &a, const struct timespec &b)
{
    return (a < b || a == b);
}

static inline bool operator>(const struct timespec &a, const struct timespec &b)
{
    return (a.tv_sec > b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec > b.tv_nsec));
}

static inline bool operator>=(const struct timespec &a, const struct timespec &b)
{
    return (a > b || a == b);
}

static inline bool is_zero(const struct timespec &v)
{
    return v.tv_sec == 0 && v.tv_nsec == 0;
}

static inline uint64_t to_ns(const struct timespec &v)
{
    return v.tv_sec * UINT64_C(1000000000) + v.tv_nsec;
}

static inline uint64_t to_us(const struct timespec &v)
{
    return v.tv_sec * UINT64_C(1000000) + (v.tv_nsec / 1000);
}

static inline uint64_t to_ms(const struct timespec &v)
{
    return v.tv_sec * UINT64_C(1000) + (v.tv_nsec / 1000000);
}

static inline struct timespec proportion(const struct timespec &v, uint32_t num, uint32_t denom)
{
    struct timespec res;

    res.tv_nsec = v.tv_nsec * num;
    res.tv_sec = v.tv_sec * num;
    
    normalize(res);

    res.tv_nsec /= denom;
    res.tv_nsec += ((res.tv_sec % denom) * 1000000000UL) / denom;
    res.tv_sec = res.tv_sec / denom;
    
    normalize(res);

    return res;
}

} // namespace timespec_utils

#endif // __TimeSpecUtils_hpp__
