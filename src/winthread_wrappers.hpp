/*
 *              Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   winthread_wrappers.hpp
 * \author Andrey Semashev
 * \date   23.01.2016
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_WINTHREAD_WRAPPERS_HPP_INCLUDED_
#define BOOST_LOG_WINTHREAD_WRAPPERS_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <boost/detail/winapi/access_rights.hpp>
#include <boost/detail/winapi/handles.hpp>
#include <boost/detail/winapi/event.hpp>
#include <boost/detail/winapi/semaphore.hpp>
#include <boost/detail/winapi/wait.hpp>
#include <boost/detail/winapi/dll.hpp>
#include <boost/detail/winapi/time.hpp>
#include <boost/detail/winapi/GetLastError.hpp>
#include <windows.h> // for error codes
#include <cstddef>
#include <limits>
#include <string>
#include <boost/assert.hpp>
#include <boost/throw_exception.hpp>
#include <boost/checked_delete.hpp>
#include <boost/memory_order.hpp>
#include <boost/atomic/atomic.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/set.hpp>
#include <boost/intrusive/set_hook.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/list_hook.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/utility/permissions.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace ipc {

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

// TODO: Port to Boost.Atomic when it supports extended atomic ops
#if defined(BOOST_MSVC) && (_MSC_VER >= 1400) && !defined(UNDER_CE)

#if _MSC_VER == 1400
extern "C" unsigned char _interlockedbittestandset(long *a, long b);
extern "C" unsigned char _interlockedbittestandreset(long *a, long b);
#else
extern "C" unsigned char _interlockedbittestandset(volatile long *a, long b);
extern "C" unsigned char _interlockedbittestandreset(volatile long *a, long b);
#endif

#pragma intrinsic(_interlockedbittestandset)
#pragma intrinsic(_interlockedbittestandreset)

BOOST_FORCEINLINE bool bit_test_and_set(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    return _interlockedbittestandset(reinterpret_cast< long* >(&x.storage()), static_cast< long >(bit)) != 0;
}

BOOST_FORCEINLINE bool bit_test_and_reset(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    return _interlockedbittestandreset(reinterpret_cast< long* >(&x.storage()), static_cast< long >(bit)) != 0;
}

#elif (defined(BOOST_MSVC) || defined(BOOST_INTEL_WIN)) && defined(_M_IX86)

BOOST_FORCEINLINE bool bit_test_and_set(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    boost::atomic< uint32_t >::storage_type* p = &x.storage();
    bool ret;
    __asm
    {
        mov eax, bit
        mov edx, p
        lock bts [edx], eax
        setc ret
    };
    return ret;
}

BOOST_FORCEINLINE bool bit_test_and_reset(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    boost::atomic< uint32_t >::storage_type* p = &x.storage();
    bool ret;
    __asm
    {
        mov eax, bit
        mov edx, p
        lock btr [edx], eax
        setc ret
    };
    return ret;
}

#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))

#if !defined(__CUDACC__)
#define BOOST_LOG_DETAIL_ASM_CLOBBER_CC_COMMA "cc",
#else
#define BOOST_LOG_DETAIL_ASM_CLOBBER_CC_COMMA
#endif

BOOST_FORCEINLINE bool bit_test_and_set(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    bool res;
    __asm__ __volatile__
    (
        "lock; bts %[bit_number], %[storage]\n\t"
        "setc %[result]\n\t"
        : [storage] "+m" (x.storage()), [result] "=q" (res)
        : [bit_number] "Kq" (bit)
        : BOOST_LOG_DETAIL_ASM_CLOBBER_CC_COMMA "memory"
    );
    return res;
}

BOOST_FORCEINLINE bool bit_test_and_reset(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    bool res;
    __asm__ __volatile__
    (
        "lock; btr %[bit_number], %[storage]\n\t"
        "setc %[result]\n\t"
        : [storage] "+m" (x.storage()), [result] "=q" (res)
        : [bit_number] "Kq" (bit)
        : BOOST_LOG_DETAIL_ASM_CLOBBER_CC_COMMA "memory"
    );
    return res;
}

#else

BOOST_FORCEINLINE bool bit_test_and_set(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    const uint32_t mask = uint32_t(1u) << bit;
    uint32_t old_val = x.fetch_or(mask, boost::memory_order_acq_rel);
    return (old_val & mask) != 0u;
}

BOOST_FORCEINLINE bool bit_test_and_reset(boost::atomic< uint32_t >& x, uint32_t bit) BOOST_NOEXCEPT
{
    const uint32_t mask = uint32_t(1u) << bit;
    uint32_t old_val = x.fetch_and(~mask, boost::memory_order_acq_rel);
    return (old_val & mask) != 0u;
}

#endif

//! A wrapper around a kernel object handle. Automatically closes the handle on destruction.
class auto_handle
{
private:
    boost::detail::winapi::HANDLE_ m_handle;

public:
    explicit auto_handle(boost::detail::winapi::HANDLE_ h = NULL) BOOST_NOEXCEPT : m_handle(h)
    {
    }

    ~auto_handle() BOOST_NOEXCEPT
    {
        if (m_handle)
            BOOST_VERIFY(boost::detail::winapi::CloseHandle(m_handle) != 0);
    }

    void init(boost::detail::winapi::HANDLE_ h) BOOST_NOEXCEPT
    {
        BOOST_ASSERT(m_handle == NULL);
        m_handle = h;
    }

    boost::detail::winapi::HANDLE_ get() const BOOST_NOEXCEPT { return m_handle; }

    BOOST_DELETED_FUNCTION(auto_handle(auto_handle const&))
    BOOST_DELETED_FUNCTION(auto_handle& operator=(auto_handle const&))
};

//! Interprocess event object
class interprocess_event
{
private:
    auto_handle m_event;

public:
    void init(const wchar_t* name, bool manual_reset, permissions const& perms = permissions())
    {
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
        boost::detail::winapi::HANDLE_ h = boost::detail::winapi::CreateEventExW
        (
            reinterpret_cast< boost::detail::winapi::SECURITY_ATTRIBUTES_* >(perms.get_native()),
            name,
            boost::detail::winapi::CREATE_EVENT_MANUAL_RESET_ * manual_reset,
            boost::detail::winapi::SYNCHRONIZE_ | boost::detail::winapi::EVENT_MODIFY_STATE_
        );
#else
        boost::detail::winapi::HANDLE_ h = boost::detail::winapi::CreateEventW
        (
            reinterpret_cast< boost::detail::winapi::SECURITY_ATTRIBUTES_* >(perms.get_native()),
            manual_reset,
            false,
            name
        );
#endif
        if (h == NULL)
        {
            boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
            if (BOOST_LIKELY(err == ERROR_ALREADY_EXISTS))
            {
                h = boost::detail::winapi::OpenEventW(boost::detail::winapi::SYNCHRONIZE_ | boost::detail::winapi::EVENT_MODIFY_STATE_, false, name);
                if (BOOST_UNLIKELY(h == NULL))
                {
                    err = boost::detail::winapi::GetLastError();
                    BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to open an interprocess event object", (err));
                }
            }
            else
            {
                BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to create an interprocess event object", (err));
            }
        }

        m_event.init(h);
    }

    boost::detail::winapi::HANDLE_ get_handle() const BOOST_NOEXCEPT { return m_event.get(); }

    void set()
    {
        if (BOOST_UNLIKELY(!boost::detail::winapi::SetEvent(m_event.get())))
        {
            err = boost::detail::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to set an interprocess event object", (err));
        }
    }

    void set_noexcept() BOOST_NOEXCEPT
    {
        BOOST_VERIFY(!!boost::detail::winapi::SetEvent(m_event.get()));
    }

    void reset()
    {
        if (BOOST_UNLIKELY(!boost::detail::winapi::ResetEvent(m_event.get())))
        {
            err = boost::detail::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to reset an interprocess event object", (err));
        }
    }

    void wait()
    {
        const boost::detail::winapi::DWORD_ retval = boost::detail::winapi::WaitForSingleObject(m_event.get(), boost::detail::winapi::infinite);
        if (BOOST_UNLIKELY(retval != boost::detail::winapi::wait_object_0))
        {
            const boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to block on an interprocess event object", (err));
        }
    }

    bool wait(boost::detail::winapi::HANDLE_ abort_handle)
    {
        boost::detail::winapi::HANDLE_ handles[2u] = { m_event.get(), abort_handle };
        const boost::detail::winapi::DWORD_ retval = boost::detail::winapi::WaitForMultipleObjects(2u, handles, false, boost::detail::winapi::infinite);
        if (retval == (boost::detail::winapi::wait_object_0 + 1u))
        {
            // Wait was interrupted
            return false;
        }
        else if (BOOST_UNLIKELY(retval != boost::detail::winapi::wait_object_0))
        {
            const boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to block on an interprocess event object", (err));
        }

        return true;
    }
};

//! Interprocess semaphore object
class interprocess_semaphore
{
private:
    typedef boost::detail::winapi::DWORD_ NTSTATUS_;
    struct semaphore_basic_information
    {
        boost::detail::winapi::ULONG_ current_count; // current semaphore count
        boost::detail::winapi::ULONG_ maximum_count; // max semaphore count
    };
    typedef NTSTATUS_ (__stdcall *nt_query_semaphore_t)(boost::detail::winapi::HANDLE_ h, unsigned int info_class, semaphore_basic_information* pinfo, boost::detail::winapi::ULONG_ info_size, boost::detail::winapi::ULONG_* ret_len);
    typedef bool (*is_semaphore_zero_count_t)(boost::detail::winapi::HANDLE_ h);

private:
    auto_handle m_sem;

    static boost::atomic< is_semaphore_zero_count_t > is_semaphore_zero_count;
    static nt_query_semaphore_t nt_query_semaphore;

public:
    void init(const wchar_t* name, permissions const& perms = permissions())
    {
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
        boost::detail::winapi::HANDLE_ h = boost::detail::winapi::CreateSemaphoreExW
        (
            reinterpret_cast< boost::detail::winapi::SECURITY_ATTRIBUTES_* >(perms.get_native()),
            0, // initial count
            (std::numeric_limits< boost::detail::winapi::LONG_ >::max)(), // max count
            name,
            0u, // flags
            boost::detail::winapi::SYNCHRONIZE_ | boost::detail::winapi::SEMAPHORE_MODIFY_STATE_
        );
#else
        boost::detail::winapi::HANDLE_ h = boost::detail::winapi::CreateSemaphoreW
        (
            reinterpret_cast< boost::detail::winapi::SECURITY_ATTRIBUTES_* >(perms.get_native()),
            0, // initial count
            (std::numeric_limits< boost::detail::winapi::LONG_ >::max)(), // max count
            name
        );
#endif
        if (h == NULL)
        {
            boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
            if (BOOST_LIKELY(err == ERROR_ALREADY_EXISTS))
            {
                h = boost::detail::winapi::OpenSemaphoreW(boost::detail::winapi::SYNCHRONIZE_ | boost::detail::winapi::SEMAPHORE_MODIFY_STATE_, false, name);
                if (BOOST_UNLIKELY(h == NULL))
                {
                    err = boost::detail::winapi::GetLastError();
                    BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to open an interprocess semaphore object", (err));
                }
            }
            else
            {
                BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to create an interprocess semaphore object", (err));
            }
        }

        m_sem.init(h);
    }

    boost::detail::winapi::HANDLE_ get_handle() const BOOST_NOEXCEPT { return m_sem.get(); }

    void post(uint32_t count)
    {
        BOOST_ASSERT(count <= (std::numeric_limits< boost::detail::winapi::LONG_ >::max)());

        if (BOOST_UNLIKELY(!boost::detail::winapi::ReleaseSemaphore(m_sem.get(), static_cast< boost::detail::winapi::LONG_ >(count), NULL)))
        {
            err = boost::detail::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to post on an interprocess semaphore object", (err));
        }
    }

    bool is_zero_count() const
    {
        return is_semaphore_zero_count.load(boost::memory_order_acquire)(m_sem.get());
    }

    void wait()
    {
        const boost::detail::winapi::DWORD_ retval = boost::detail::winapi::WaitForSingleObject(m_sem.get(), boost::detail::winapi::infinite);
        if (BOOST_UNLIKELY(retval != boost::detail::winapi::wait_object_0))
        {
            const boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to block on an interprocess semaphore object", (err));
        }
    }

    bool wait(boost::detail::winapi::HANDLE_ abort_handle)
    {
        boost::detail::winapi::HANDLE_ handles[2u] = { m_sem.get(), abort_handle };
        const boost::detail::winapi::DWORD_ retval = boost::detail::winapi::WaitForMultipleObjects(2u, handles, false, boost::detail::winapi::infinite);
        if (retval == (boost::detail::winapi::wait_object_0 + 1u))
        {
            // Wait was interrupted
            return false;
        }
        else if (BOOST_UNLIKELY(retval != boost::detail::winapi::wait_object_0))
        {
            const boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to block on an interprocess semaphore object", (err));
        }

        return true;
    }

private:
    static bool is_semaphore_zero_count_init(boost::detail::winapi::HANDLE_ h)
    {
        is_semaphore_zero_count_t impl = &interprocess_semaphore::is_semaphore_zero_count_emulated;

        // Check if ntdll.dll provides NtQuerySemaphore, see: http://undocumented.ntinternals.net/index.html?page=UserMode%2FUndocumented%20Functions%2FNT%20Objects%2FSemaphore%2FNtQuerySemaphore.html
        boost::detail::winapi::HMODULE_ ntdll = boost::detail::winapi::GetModuleHandleW(L"ntdll.dll");
        if (ntdll)
        {
            nt_query_semaphore_t ntqs = (nt_query_semaphore_t)boost::detail::winapi::get_proc_address(ntdll, "NtQuerySemaphore");
            if (ntqs)
            {
                nt_query_semaphore = ntqs;
                impl = &interprocess_semaphore::is_semaphore_zero_count_nt_query_semaphore;
            }
        }

        is_semaphore_zero_count.store(impl, boost::memory_order_release);

        return impl(h);
    }

    static bool is_semaphore_zero_count_nt_query_semaphore(boost::detail::winapi::HANDLE_ h)
    {
        semaphore_basic_information info = {};
        NTSTATUS_ err = nt_query_semaphore
        (
            h,
            0u, // SemaphoreBasicInformation
            &info,
            sizeof(info),
            NULL
        );
        if (BOOST_UNLIKELY(err != 0u))
        {
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to test an interprocess semaphore object for zero count", (ERROR_INVALID_HANDLE));
        }

        return info.current_count == 0u;
    }

    static bool is_semaphore_zero_count_emulated(boost::detail::winapi::HANDLE_ h)
    {
        const boost::detail::winapi::DWORD_ retval = boost::detail::winapi::WaitForSingleObject(h, 0u);
        if (retval == boost::detail::winapi::wait_timeout)
        {
            return true;
        }
        else if (BOOST_UNLIKELY(retval != boost::detail::winapi::wait_object_0))
        {
            const boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to test an interprocess semaphore object for zero count", (err));
        }

        // Restore the decremented counter
        BOOST_VERIFY(!!boost::detail::winapi::ReleaseSemaphore(h, 1, NULL))

        return false;
    }
};

boost::atomic< interprocess_semaphore::is_semaphore_zero_count_t > interprocess_semaphore::is_semaphore_zero_count(&interprocess_semaphore::is_semaphore_zero_count_init);
interprocess_semaphore::nt_query_semaphore_t interprocess_semaphore::nt_query_semaphore = NULL;

//! Interprocess mutex. Implementation adopted from Boost.Sync.
class interprocess_mutex
{
public:
    //! Shared state that should be visible to all processes using the mutex
    struct shared_state
    {
        boost::atomic< uint32_t > m_lock_state;

        shared_state() BOOST_NOEXCEPT : m_lock_state(0u)
        {
        }
    };

    struct auto_unlock
    {
        explicit auto_unlock(interprocess_mutex& mutex) BOOST_NOEXCEPT : m_mutex(mutex) {}
        ~auto_unlock() { m_mutex.unlock(); }

        BOOST_DELETED_FUNCTION(auto_unlock(auto_unlock const&))
        BOOST_DELETED_FUNCTION(auto_unlock& operator=(auto_unlock const&))

    private:
        interprocess_mutex& m_mutex;
    };

    struct optional_unlock
    {
        optional_unlock() BOOST_NOEXCEPT : m_mutex(NULL) {}
        explicit optional_unlock(interprocess_mutex& mutex) BOOST_NOEXCEPT : m_mutex(&mutex) {}
        ~optional_unlock() { if (m_mutex) m_mutex->unlock(); }

        interprocess_mutex* disengage() BOOST_NOEXCEPT
        {
            interprocess_mutex* p = m_mutex;
            m_mutex = NULL;
            return p;
        }

        void engage(interprocess_mutex& mutex) BOOST_NOEXCEPT
        {
            BOOST_ASSERT(!m_mutex);
            m_mutex = &mutex;
        }

        BOOST_DELETED_FUNCTION(optional_unlock(optional_unlock const&))
        BOOST_DELETED_FUNCTION(optional_unlock& operator=(optional_unlock const&))

    private:
        interprocess_mutex* m_mutex;
    };

private:
    interprocess_event m_event;
    shared_state* m_shared_state;

    static BOOST_CONSTEXPR_OR_CONST uint32_t lock_flag_bit = 31u;
    static BOOST_CONSTEXPR_OR_CONST uint32_t event_set_flag_bit = 30u;
    static BOOST_CONSTEXPR_OR_CONST uint32_t lock_flag_value = 1u << lock_flag_bit;
    static BOOST_CONSTEXPR_OR_CONST uint32_t event_set_flag_value = 1u << event_set_flag_bit;
    static BOOST_CONSTEXPR_OR_CONST uint32_t waiter_count_mask = event_set_flag_value - 1u;

public:
    interprocess_mutex() BOOST_NOEXCEPT : m_shared_state(NULL)
    {
    }

    void init(const wchar_t* name, shared_state* shared, permissions const& perms = permissions())
    {
        m_event.init(name, false, perms);
        m_shared_state = shared;
    }

    bool try_lock()
    {
        return !bit_test_and_set(m_shared_state->m_lock_state, lock_flag_bit);
    }

    void lock()
    {
        if (BOOST_UNLIKELY(!try_lock()))
            lock_slow();
    }

    bool lock(boost::detail::winapi::HANDLE_ abort_handle)
    {
        if (BOOST_LIKELY(try_lock()))
            return true;
        return lock_slow(abort_handle);
    }

    void unlock() BOOST_NOEXCEPT
    {
        const uint32_t old_count = m_shared_state->m_lock_state.fetch_add(lock_flag_value, boost::memory_order_release);
        if ((old_count & event_set_flag_value) == 0u && (old_count > lock_flag_value))
        {
            if (!bit_test_and_set(m_shared_state->m_lock_state, event_set_flag_bit))
            {
                m_event.set_noexcept();
            }
        }
    }

    BOOST_DELETED_FUNCTION(interprocess_mutex(interprocess_mutex const&))
    BOOST_DELETED_FUNCTION(interprocess_mutex& operator=(interprocess_mutex const&))

private:
    void lock_slow()
    {
        uint32_t old_state = m_shared_state->m_lock_state.load(boost::memory_order_relaxed);
        mark_waiting_and_try_lock(old_state);

        if ((old_state & lock_flag_value) != 0u) try
        {
            do
            {
                m_event.wait()
                clear_waiting_and_try_lock(old_state);
            }
            while ((old_state & lock_flag_value) != 0u);
        }
        catch (...)
        {
            m_shared_state->m_lock_state.fetch_sub(1u, boost::memory_order_acq_rel);
            throw;
        }
    }

    bool lock_slow(boost::detail::winapi::HANDLE_ abort_handle)
    {
        uint32_t old_state = m_shared_state->m_lock_state.load(boost::memory_order_relaxed);
        mark_waiting_and_try_lock(old_state);

        if ((old_state & lock_flag_value) != 0u) try
        {
            do
            {
                if (!m_event.wait(abort_handle))
                {
                    // Wait was interrupted
                    m_shared_state->m_lock_state.fetch_sub(1u, boost::memory_order_acq_rel);
                    return false;
                }

                clear_waiting_and_try_lock(old_state);
            }
            while ((old_state & lock_flag_value) != 0u);
        }
        catch (...)
        {
            m_shared_state->m_lock_state.fetch_sub(1u, boost::memory_order_acq_rel);
            throw;
        }

        return true;
    }

    void mark_waiting_and_try_lock(uint32_t& old_state)
    {
        uint32_t was_locked, new_state;
        do
        {
            was_locked = (old_state & lock_flag_value);
            if (was_locked)
            {
                // Avoid integer overflows
                if (BOOST_UNLIKELY((old_state & waiter_count_mask) == waiter_count_mask))
                    BOOST_LOG_THROW_DESCR(limitation_error, "Too many waiters on an interprocess mutex");

                new_state = old_state + 1u;
            }
            else
            {
                new_state = old_state | lock_flag_value;
            }
        }
        while (!m_shared_state->m_lock_state.compare_exchange_weak(old_state, new_state, boost::memory_order_acq_rel, boost::memory_order_relaxed));
    }

    void clear_waiting_and_try_lock(uint32_t& old_state)
    {
        old_state &= ~lock_flag_value;
        old_state |= event_set_flag_value;
        uint32_t new_state;
        do
        {
            new_state = ((old_state & lock_flag_value) ? old_state : ((old_state - 1u) | lock_flag_value)) & ~event_set_flag_value;
        }
        while (!m_shared_state->m_lock_state.compare_exchange_strong(old_state, new_state, boost::memory_order_acq_rel, boost::memory_order_relaxed));
    }
};

//! Interprocess condition variable (semaphore-based)
class interprocess_condition_variable
{
private:
    typedef boost::intrusive::list_base_hook<
        boost::intrusive::tag< struct for_sem_order_by_usage >,
        boost::intrusive::link_mode< boost::intrusive::safe_link >
    > semaphore_info_list_hook_t;

    typedef boost::intrusive::set_base_hook<
        boost::intrusive::tag< struct for_sem_lookup_by_id >,
        boost::intrusive::link_mode< boost::intrusive::safe_link >,
        boost::intrusive::optimize_size< true >
    > semaphore_info_set_hook_t;

    //! Information about a semaphore object
    struct semaphore_info :
        public semaphore_info_list_hook_t,
        public semaphore_info_set_hook_t
    {
        struct order_by_id
        {
            typedef bool result_type;

            result_type operator() (semaphore_info const& left, semaphore_info const& right) const BOOST_NOEXCEPT
            {
                return left.m_id < right.m_id;
            }
            result_type operator() (semaphore_info const& left, uint32_t right) const BOOST_NOEXCEPT
            {
                return left.m_id < right;
            }
            result_type operator() (uint32_t left, semaphore_info const& right) const BOOST_NOEXCEPT
            {
                return left < right.m_id;
            }
        };

        //! The semaphore
        interprocess_semaphore m_semaphore;
        //! Timestamp of the moment when the semaphore was seen with a zero counter last time. Corresponds fo the \c GetTickCount/GetTickCount64 timeline, in milliseconds since epoch.
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
        boost::detail::winapi::ULONGLONG_ m_last_seen_zero;
#else
        boost::detail::winapi::DWORD_ m_last_seen_zero;
#endif
        //! The semaphore id
        const uint32_t m_id;

        explicit semaphore_info(uint32_t id) BOOST_NOEXCEPT : m_last_seen_zero(0u), m_id(id)
        {
        }

        BOOST_DELETED_FUNCTION(semaphore_info(semaphore_info const&))
        BOOST_DELETED_FUNCTION(semaphore_info& operator=(semaphore_info const&))
    };

    typedef boost::intrusive::list<
        semaphore_info,
        boost::intrusive::base_hook< semaphore_info_list_hook_t >,
        boost::intrusive::constant_time_size< false >
    > semaphore_info_list;

    typedef boost::intrusive::set<
        semaphore_info,
        boost::intrusive::base_hook< semaphore_info_set_hook_t >,
        boost::intrusive::compare< semaphore_info::order_by_id >,
        boost::intrusive::constant_time_size< false >
    > semaphore_info_set;

public:
    struct shared_state
    {
        //! Number of waiters blocked on the semaphore if >0, 0 if no threads are blocked, <0 when the blocked threads were signalled
        int32_t m_waiters;
        //! Id of the semaphore which is used to block threads on
        uint32_t m_semaphore_id;

        shared_state() BOOST_NOEXCEPT :
            m_waiters(0),
            m_semaphore_id(0u)
        {
        }
    };

private:
    semaphore_info_list m_semaphore_info_list;
    semaphore_info_set m_semaphore_info_set;
    semaphore_info* m_current_semaphore;
    std::wstring m_semaphore_name;
    shared_state* m_shared_state;
    uint32_t m_next_semaphore_id;

public:
    interprocess_condition_variable() BOOST_NOEXCEPT : m_current_semaphore(NULL), m_shared_state(NULL), m_next_semaphore_id(0u)
    {
    }

    ~interprocess_condition_variable()
    {
        m_semaphore_info_set.clear();
        m_semaphore_info_list.clear_and_dispose(boost::checked_deleter< semaphore_info >());
    }

    void init(const wchar_t* name, shared_state* shared, permissions const& perms = permissions())
    {
        m_event.init(name, true, perms);
        m_shared_state = shared;
    }

    void notify_all()
    {
        const int32_t waiters = m_shared_state->m_waiters;
        if (waiters > 0)
        {
            const uint32_t id = m_shared_state->m_semaphore_id;
            if (m_current_semaphore->m_id != id)
                m_current_semaphore = get_semaphore(id);

            m_current_semaphore->m_semaphore.post(waiters);
            m_shared_state->m_waiters = -waiters;
        }
    }

    bool wait(interprocess_mutex::optional_unlock& lock, boost::detail::winapi::HANDLE_ abort_handle)
    {
        int32_t waiters = m_shared_state->m_waiters;
        if (waiters < 0)
        {
            // We need to select a new semaphore to block on
            m_current_semaphore = get_unused_semaphore();
            m_shared_state->m_semaphore_id = m_current_semaphore->m_id;
            waiters = 0;
        }
        else
        {
            // Avoid integer overflow
            if (BOOST_UNLIKELY(waiters >= ((std::numeric_limits< int32_t >::max)() - 1)))
                BOOST_LOG_THROW_DESCR(limitation_error, "Too many waiters on an interprocess condition variable");
        }

        m_shared_state->m_waiters = waiters + 1;
        semaphore_info* const sem_info = m_current_semaphore;

        interprocess_mutex* const mutex = lock.disengage();
        mutex->unlock();

        result = sem_info->m_semaphore.wait(abort_handle);

        // Have to unconditionally lock the mutex here
        mutex->lock();
        lock.engage(*mutex);

        if (!result && m_shared_state->m_waiters > 0)
            --m_shared_state->m_waiters;

        return result;
    }

    BOOST_DELETED_FUNCTION(interprocess_condition_variable(interprocess_condition_variable const&))
    BOOST_DELETED_FUNCTION(interprocess_condition_variable& operator=(interprocess_condition_variable const&))

private:
    //! Finds or opens a semaphore with the specified id
    semaphore_info* get_semaphore(uint32_t id)
    {
        semaphore_info_set::iterator it = m_semaphore_info_set.find(id, semaphore_info::order_by_id());
        if (it == m_semaphore_info_set.end())
        {
            // We need to open the semaphore
        }

        return &*it;
    }

    //! Finds or creates a semaphore with zero counter
    semaphore_info* get_unused_semaphore()
    {
    }
};

/*
//! Interprocess condition variable (event-based)
class interprocess_condition_variable
{
public:
    struct shared_state
    {
        boost::atomic< uint32_t > m_waiters;
        uint32_t m_notify_generation;
        uint32_t m_wait_generation;

        shared_state() BOOST_NOEXCEPT :
            m_waiters(0u),
            m_notify_generation(0u),
            m_wait_generation(0u)
        {
        }
    };

private:
    interprocess_event m_event;
    shared_state* m_shared_state;

public:
    interprocess_condition_variable() BOOST_NOEXCEPT : m_shared_state(NULL)
    {
    }

    void init(const wchar_t* name, shared_state* shared, permissions const& perms = permissions())
    {
        m_event.init(name, true, perms);
        m_shared_state = shared;
    }

    void notify_all()
    {
        if (m_shared_state->m_notify_generation == m_shared_state->m_wait_generation && m_shared_state->m_waiters.load(boost::memory_order_relaxed) > 0u)
        {
            ++m_shared_state->m_notify_generation;
            m_event.set();
        }
    }

    bool wait(interprocess_mutex::optional_unlock& lock, boost::detail::winapi::HANDLE_ abort_handle)
    {
        if (m_shared_state->m_notify_generation != m_shared_state->m_wait_generation)
        {
            if (BOOST_UNLIKELY(m_shared_state->m_waiters.load(boost::memory_order_relaxed) > 0u))
            {
                // Some threads are being notified by the event. Do a short wait on the abort handle and then return.
                // The caller will act as if this was a spurious wakeup, which are allowed anyway.
                return simulate_wait(lock, abort_handle);
            }

            m_shared_state->m_wait_generation = m_shared_state->m_notify_generation;
        }

        // Block on the condition variable properly
        const uint32_t generation = m_shared_state->m_wait_generation;

        // It is essential to increment the counter under the lock so that the checks for zero above are valid.
        // The mutex lock/unlock ensures memory ordering of the counter modifications, even though they have relaxed ordering.
        m_shared_state->m_waiters.fetch_add(1u, boost::memory_order_relaxed);

        bool result;
        try
        {
            interprocess_mutex* const mutex = lock.disengage();
            mutex->unlock();

            result = m_event.wait(abort_handle);

            // Have to unconditionally lock the mutex here
            mutex->lock();
            lock.engage(*mutex);
        }
        catch (...)
        {
            m_shared_state->m_waiters.fetch_sub(1u, boost::memory_order_relaxed);
            throw;
        }

        const uint32_t old_waiters = m_shared_state->m_waiters.fetch_sub(1u, boost::memory_order_relaxed);

        if (generation == m_shared_state->m_wait_generation && m_shared_state->m_notify_generation == m_shared_state->m_wait_generation && old_waiters == 1u)
            m_event.reset();

        return result;
    }

    BOOST_DELETED_FUNCTION(interprocess_condition_variable(interprocess_condition_variable const&))
    BOOST_DELETED_FUNCTION(interprocess_condition_variable& operator=(interprocess_condition_variable const&))

private:
    //! Waits for a short period of time on the abort handle
    static bool simulate_wait(interprocess_mutex::optional_unlock& lock, boost::detail::winapi::HANDLE_ abort_handle)
    {
        interprocess_mutex* const mutex = lock.disengage();
        mutex->unlock();

        const boost::detail::winapi::DWORD_ retval = boost::detail::winapi::WaitForSingleObject(abort_handle, 20u);

        // Have to unconditionally lock the mutex here
        mutex->lock();
        lock.engage(*mutex);

        if (retval == boost::detail::winapi::wait_timeout)
        {
            // Simulate spurious wakeup
            return true;
        }
        else if (BOOST_UNLIKELY(retval != boost::detail::winapi::wait_object_0))
        {
            const boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to block on an interprocess event object", (err));
        }

        // Wait aborted
        return false;
    }
};
*/

} // namespace

} // namespace aux

} // namespace ipc

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_WINTHREAD_WRAPPERS_HPP_INCLUDED_