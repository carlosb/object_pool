/*
 *  Copyright 2017 Carlos David Brito Pacheco
 *
 * Boost Software License - Version 1.0 - August 17th, 2003
 * 
 * Permission is hereby granted, free of charge, to any person or organization
 * obtaining a copy of the software and accompanying documentation covered by
 * this license (the "Software") to use, reproduce, display, distribute,
 * execute, and transmit the Software, and to prepare derivative works of the
 * Software, and to permit third-parties to whom the Software is furnished to
 * do so, all subject to the following:
 * 
 * The copyright notices in the Software and this entire statement, including
 * the above license grant, this restriction and the following disclaimer,
 * must be included in all copies of the Software, in whole or in part, and
 * all derivative works of the Software, unless such copies or derivative
 * works are solely in the form of machine-executable object code generated by
 * a source language processor.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 * FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef CARLOSB_OBJECT_POOL_HPP
#define CARLOSB_OBJECT_POOL_HPP

#include <memory>
#include <mutex>
#include <condition_variable>
#include <stack>
#include <stdexcept>
#include <type_traits>
#include <cassert>
#include <chrono>
#include <cstring>

#include <iostream>

namespace carlosb
{
    /**
     * @brief      Class for object pool.
     *
     * @tparam     T          Type which will be managed by the `object_pool`.
     * @tparam     Allocator  Allocator type. Must satisfy BasicAllocator requirement.
     * @tparam     Mutex      Mutex type. Must satisfy Lockable requirement.
     */
    template <
        class _Tp,
        class _Allocator         = std::allocator<_Tp>,
        class _Mutex             = std::mutex
    >
    class object_pool;

    /**
     * Contains types needed by the implementation.
     */
    namespace detail
    {   
        template <class...>
        using void_t = void;

        struct none_helper
        {};
    }

    typedef int detail::none_helper::*none_t;
    none_t const none = (static_cast<none_t>(0)) ;

    /**
     * Contains meta-functions to determine if an object of type `T` satisfies
     * the requirements of some concept. Currently implemented:
     * - `is_lockable<_Tp>`
     * - `is_allocator<_Tp>`
     */
    namespace type_traits
    {
        template <class _Tp, class = void>
        struct is_lockable : public std::false_type
        {};

        template <class _Tp>
        struct is_lockable<_Tp, detail::void_t<
            decltype(std::declval<_Tp>().lock()),
            decltype(std::declval<_Tp>().unlock()),
            decltype(std::declval<_Tp>().try_lock()),
            std::is_same<decltype(std::declval<_Tp>().try_lock()), bool>
        >> : std::true_type
        {};

        template <class _Tp, class = void>
        struct is_allocator : public std::false_type
        {};

        template <class _Tp>
        struct is_allocator<_Tp, detail::void_t<
            typename _Tp::value_type,
            typename _Tp::size_type,
            decltype(std::declval<_Tp>().allocate(std::declval<typename _Tp::size_type>())),
            typename std::is_same<
                decltype(std::declval<_Tp>().allocate(std::declval<typename _Tp::size_type>())),
                typename _Tp::value_type*
            >::type,
            decltype(std::declval<_Tp>().deallocate(std::declval<typename _Tp::value_type*>(), std::declval<typename _Tp::size_type>()))
        >> : std::true_type
        {};
    }

    template <
        class _Tp,
        class _Allocator,
        class _Mutex
    >
    class object_pool
    {
        static_assert(type_traits::is_allocator<_Allocator>::value,
                      "Allocator template parameter must satisfy the BasicAllocator requirement.");
        static_assert(type_traits::is_lockable<_Mutex>::value,
                      "Mutex template parameter must satisfy the Lockable requirement.");
    private:
        class impl;     ///< Contains the actual implementation of `object_pool`.
        class deleter;  ///< The `deleter` class returns lent objects to the pool.
        
    public:
        class acquired_object;

        using value_type        = _Tp;                        ///< Type managed by the pool.
        using lv_reference      = _Tp&;                       ///< l-value reference.
        using rv_reference      = _Tp&&;                      ///< r-value reference.
        using const_reference   = const _Tp&;                 ///< const l-value reference.

        using acquired_type     = acquired_object;          ///< Type of acquired objects.
        using stack_type        = std::stack<_Tp*>;           ///< Stack of pointers to the managed objects. Only contains unused objects.

        using size_type         = std::size_t;              ///< Size type used.

        using allocator_type    = _Allocator;                ///< Type of allocator.
        using mutex_type        = _Mutex;                    ///< Type of mutex.
        using scoped_lock_type  = std::lock_guard<_Mutex>;   ///< Type of lock guard.
        
        /**
         * @brief      Constructs an empty pool.
         */
        object_pool()
            : object_pool(_Allocator())
        {}

        /**
         * @brief      Constructs an empty pool with the given allocator.
         *
         * @param[in]  alloc  The allocator.
         */
        explicit
        object_pool(const _Allocator& alloc)
            : m_pool(std::make_shared<impl>(alloc))
        {}

        /**
         * @brief      Constructs a pool with `count` copies of `value`.
         *
         * @param[in]  count  Number of elements.
         * @param[in]  value  Value to be copied.
         * @param[in]  alloc  Allocator to be used.
         */
        object_pool(size_type count, const _Tp& value, const _Allocator& alloc = _Allocator())
            : m_pool(std::make_shared<impl>(count, value, alloc))
        {}

        /**
         * @brief      Constructs the container with count default-inserted instances of T. No copies are made.
         *
         * @param[in]  count  Number of default-inserted elements.
         * @param[in]  alloc  Allocator to be used.
         */
        explicit
        object_pool(size_type count, const _Allocator& alloc = _Allocator())
            : m_pool(std::make_shared<impl>(count, alloc))
        {}

        /**
         * @brief      Shares ownership of the objects.
         *
         * @param[in]  other  Other pool.
         */
        object_pool(const object_pool& other)
            : m_pool(other.m_pool)
        {}

        /**
         * @brief      Transfers ownership of objects to `*this`.
         *
         * @param[in]  other  Other value.
         */
        object_pool(object_pool&& other) noexcept
            : m_pool(std::move(other.m_pool))
        {}

        /**
         * @brief      Shares ownership of objects.
         *
         * @param[in]  other  Other pool.
         *
         * @return     Returns a reference to a pool object which manages
         * the same objects as `other`.
         */
        object_pool& operator=(const object_pool& other)
        {
            m_pool = other.m_pool;
            return *this;
        }

        /**
         * @brief      Transfers ownership of objects.
         *
         * @param[in]  other  Other pool.
         *
         * @return     Returns a reference to a pool object which manages
         * the same objects as `other`. After the call, object will no longer
         * manage any objects.
         */
        object_pool& operator=(object_pool&& other)
        {
            m_pool = std::move(other.m_pool);
            return *this;
        }

        /**
         * @brief      Acquires an object from the pool. 
         * 
         * @return     Acquired object.
         * 
         * Complexity
         * ----------
         * Constant.
         */
        acquired_type acquire()
        {
            return m_pool->acquire();
        }

        /**
         * @brief      Waits until an object from the pool has been acquired or the `time_limit`
         * has ran out.
         *
         * @return     Acquired object.
         * 
         * Complexity
         * ----------
         * Constant.
         * 
         * This method will lock until an object is available.
         * One may achieve this manually by either:
         * - pushing a new object to the pool
         * - emplacing a new object into the pool
         * - freeing other objects
         */
        acquired_type acquire_wait(std::chrono::milliseconds time_limit = std::chrono::milliseconds::zero())
        {
            return m_pool->acquire_wait(time_limit);
        }

        /**
         * @brief      Attempts to acquire an object. If no object is acquired
         * then it will construct an object from the parameters passed and
         * return it. This method will always return an object that is initialized.
         *
         * @param[in]  args  Parameter pack
         *
         * @tparam     Args       Types of the parameter
         *
         * @return     Acquired object.
         */
        template <class... Args>
        acquired_type allocate(Args&&... args)
        {
            return m_pool->allocate(std::forward<Args>(args)...);
        }

        /**
         * @brief      Pushes an object to the pool by copying it.
         *
         * @param[in]  value  Object to be copied into the pool.
         * 
         * Complexity
         * ----------
         * Amortized constant.
         */
        void push(const_reference value)
        {
            m_pool->push(value);
        }

        /**
         * @brief      Pushes an object to the pool by moving it.
         * 
         * @param[in]  value  Object to be moved into the pool.
         * 
         * Complexity
         * ----------
         * Amortized constant.
         */
        void push(rv_reference value)
        {
            m_pool->push(std::move(value));
        }

        /**
         * @brief      Pushes an object to the pool by constructing it.
         *
         * @param[in]  args       Arguments to be passed to the constructor of the object.
         * 
         * Complexity
         * ----------
         * Amortized constant.
         */
        template <class... Args>
        void emplace(Args&&... args)
        {
            m_pool->emplace(std::forward<Args>(args)...);
        }

        /**
         * @brief      Resizes the pool to contain count elements.
         * 
         * @param[in]  count  Number of elements.
         * 
         * If the current size is greater than count, nothing happens.
         * If the current size is less than count, additional default-constructed elements are appended.
         *
         * Complexity
         * ----------
         * Linear in the difference between the current number of elements and count.
         */
        void resize(size_type count)
        {
            m_pool->resize(count);
        }

        /**
         * @brief      Resizes the pool to contain count elements.
         *
         * If the current size is greater than count, the container is reduced to its first count elements.
         * If the current size is less than count, additional copies of value are appended.
         * 
         * Complexity
         * ----------
         * Linear in the difference between the current number of elements and count.
         * 
         * @param[in]  count  Number of elements.
         * @param[in]  value  Value to be copied.
         */
        void resize(size_type count, const value_type& value)
        {
            m_pool->resize(count, value);
        }

        /**
         * @brief      Reserves storage.
         * 
         * @param[in]  new_cap  New capacity of the pool
         * 
         * Increase the capacity of the vector to a value that's greater or equal to new_cap.
         * 
         * If new_cap is greater than the current capacity(), new storage is allocated, 
         * otherwise the method does nothing.
         * 
         * Complexity
         * ----------
         * At most linear in the managed_count() of the pool.
         */
        void reserve(size_type new_cap)
        {
            m_pool->reserve(new_cap);
        }

        /**
         * @brief      Returns the number of **free** elements in the pool.
         *
         * @return     The number of **free** elements in the pool.
         * 
         * Complexity
         * ----------
         * Constant.
         */
        size_type size() const
        {
            return m_pool->size();
        }

        /**
         * @brief      Returns the number of elements that can be held in currently allocated storage
         * 
         * @return     Capacity of the currently allocated storage.
         * 
         * Complexity
         * ----------
         * Constant.
         */
        size_type capacity() const
        {
            return m_pool->capacity();
        }

        /**
         * @brief      Checks if the container has no **free** elements. i.e. whether size() == 0.
         * 
         * @return     `true` if no free elements are found, `false` otherwise
         * 
         * Complexity
         * ----------
         * Constant.
         */
        bool empty() const
        {
            return m_pool->empty();
        }

        /**
         * @brief      Checks if there are free elements in the pool.
         * 
         * @return     `true` if free elements are found, `false` otherwise
         * 
         * Complexity
         * ----------
         * Constant.
         */
        explicit
        operator bool() const
        {
            return m_pool->operator bool();
        }

        /**
         * @brief      Checks if the pool is still being used, i.e. not all
         * elements have returned to the pool
         *
         * @return     `true` if the pool still has used elements, `false` otherwise
         */
        bool in_use() const
        {
            return m_pool->in_use();
        }

        void swap(object_pool& other)
        {
            m_pool->swap(*other.m_pool);
        }
    private:
        std::shared_ptr<impl>       m_pool;                ///< Pointer to implementation of pool.
    };

    // --- impl IMPLEMENTATION --------------------------------------
    template <
        class _Tp,
        class _Allocator,
        class _Mutex
    >
    class object_pool<_Tp, _Allocator, _Mutex>::impl : public std::enable_shared_from_this<impl>
    {
    public:
        explicit
        impl(const _Allocator& alloc)
            :   m_managed_count(0),
                m_capacity(4),
                m_allocator(alloc)
        {
            for (size_type i = 0; i < m_capacity; ++i)
                m_allocated_space.push(m_allocator.allocate(1));
        }

        impl(size_type count, const _Tp& value, const _Allocator& alloc = _Allocator())
            :   m_managed_count(count),
                m_capacity(count),
                m_allocator(alloc)
        {   
            static_assert(std::is_copy_constructible<_Tp>::value,
                          "T must be CopyConstructible to use this constructor.");

            for (size_type i = 0; i < m_managed_count; ++i)
            {
                _Tp* obj = m_allocator.allocate(1);
                ::new((void *) (obj)) _Tp(value);
                m_free_objects.push(obj);
            }
        }

        explicit
        impl(size_type count, const _Allocator& alloc = _Allocator())
            :   m_managed_count(count),
                m_capacity(count),
                m_allocator(alloc)
        {
            for (size_type i = 0; i < m_managed_count; ++i)
            {
                _Tp* obj = m_allocator.allocate(1);
                ::new((void *) (obj)) _Tp();
                m_free_objects.push(obj);
            }
        }

        ~impl() // called until last shared ptr to *this has gone out of scope
        {
            while (!m_free_objects.empty())
            {
                m_free_objects.top()->~_Tp();
                m_allocated_space.push(m_free_objects.top());
                m_free_objects.pop();
            }

            while (!m_allocated_space.empty())
            {
                m_allocator.deallocate(m_allocated_space.top(), 1);
                m_allocated_space.top() = nullptr;
                m_allocated_space.pop();
            }
        }

        void reserve(size_type new_cap)
        {
            scoped_lock_type pool_lock(m_pool_mutex);

            if (new_cap > m_capacity)
            {
                for (int i = m_capacity; i < new_cap; ++i)
                    m_allocated_space.push(m_allocator.allocate(1));
                m_capacity = new_cap;
            }    
        }

        acquired_type acquire()
        {   
            scoped_lock_type pool_lock(m_pool_mutex);

            if (m_free_objects.empty())
                return acquired_object(none);
            acquired_object obj(m_free_objects.top(), impl::shared_from_this());
            m_free_objects.pop();
            return std::move(obj);
        }

        acquired_type acquire_wait(std::chrono::milliseconds time_limit)
        {   
            std::unique_lock<mutex_type> pool_lock(m_pool_mutex);

            acquired_object obj;
            if (time_limit == std::chrono::milliseconds::zero())
            {
                m_objects_availabe.wait(pool_lock, [this] (void) { return !m_free_objects.empty(); });
                
                obj = acquired_object(m_free_objects.top(), impl::shared_from_this());
                m_free_objects.pop();
            }
            else
            {
                if (m_objects_availabe.wait_for(pool_lock, time_limit, [this] (void) { return m_free_objects.empty(); }))
                {
                    obj = none;
                }
                else
                {
                    obj = acquired_object(m_free_objects.top(), impl::shared_from_this());
                    m_free_objects.pop();
                }
            }

            pool_lock.unlock();
            m_objects_availabe.notify_one();

            return std::move(obj);
        }

        template <class... Args>
        acquired_type allocate(Args&&... args)
        {   
            scoped_lock_type pool_lock(m_pool_mutex);

            if (m_free_objects.empty())
            {
                _Tp* obj = m_allocator.allocate(1);
                ::new((void *) (obj)) _Tp(std::forward<Args>(args)...);
                ++m_managed_count;
                return acquired_object(obj, impl::shared_from_this());
            }
            else
            {
                acquired_object obj(m_free_objects.top(), impl::shared_from_this());
                m_free_objects.pop();
                return std::move(obj);
            }
        }


        void push(const_reference value)
        {
            scoped_lock_type pool_lock(m_pool_mutex);

            if (m_managed_count == m_capacity)
                this->reallocate(m_capacity * 2);
            _Tp* obj = m_allocated_space.top();
            m_allocated_space.pop();
            ::new((void *) (obj)) _Tp(value);
            m_free_objects.push(obj);
            ++m_managed_count;
            m_objects_availabe.notify_one();
        }

        void push(rv_reference value)
        {
            scoped_lock_type pool_lock(m_pool_mutex);

            if (m_managed_count == m_capacity)
                this->reallocate(m_capacity * 2);
            _Tp* obj = m_allocated_space.top();
            m_allocated_space.pop();
            ::new((void *) (obj)) _Tp(std::move(value));
            m_free_objects.push(obj);
            ++m_managed_count;
            m_objects_availabe.notify_one();
        }

        template <class... Args>
        void emplace(Args&&... args)
        {
            scoped_lock_type pool_lock(m_pool_mutex);

            if (m_managed_count == m_capacity)
                this->reallocate(m_capacity * 2);
            _Tp* obj = m_allocated_space.top();
            m_allocated_space.pop();
            ::new((void *) (obj)) _Tp(std::forward<Args>(args)...);
            m_free_objects.push(obj); 
            ++m_managed_count;
            m_objects_availabe.notify_one();
        }

        void resize(size_type count)
        {
            scoped_lock_type pool_lock(m_pool_mutex);

            size_type size = m_free_objects.size();
            if (count > size)
            {
                if (count > m_capacity)
                    this->reallocate(count);

                for (size_type i = size; i < count; ++i)
                {
                    _Tp* obj = m_allocated_space.top();
                    m_allocated_space.pop();

                    ::new((void *) (obj)) _Tp();
                    m_free_objects.push(obj);
                }
            }
            else
            {
                for (size_type i = count; i < size; ++i)
                {
                    m_free_objects.top()->~_Tp();
                    m_allocated_space.push(m_free_objects.top());
                    m_free_objects.pop();
                }
            }
            m_managed_count = count;
        }

        void resize(size_type count, const value_type& value)
        {
            scoped_lock_type pool_lock(m_pool_mutex);

            size_type size = m_free_objects.size();
            if (count > size)
            {
                if (count > m_capacity)
                    this->reallocate(count);

                for (size_type i = size; i < count; ++i)
                {
                    _Tp* obj = m_allocated_space.top();
                    m_allocated_space.pop();

                    ::new((void *) (obj)) _Tp(value);
                    m_free_objects.push(obj);
                }
            }
            else
            {
                for (size_type i = count; i < size; ++i)
                {
                    m_free_objects.top()->~_Tp();
                    m_allocated_space.push(m_free_objects.top());
                    m_free_objects.pop();
                }
            }
            m_managed_count = count;
        }

        void return_object(_Tp* obj)
        {
            assert(obj);
            scoped_lock_type pool_lock(m_pool_mutex);
            m_free_objects.push(obj);
            m_objects_availabe.notify_one();
        }

        allocator_type get_allocator()
        {
            scoped_lock_type pool_lock(m_pool_mutex);
            return m_allocator;
        }

        size_type size() const
        {
            scoped_lock_type pool_lock(m_pool_mutex);
            return m_free_objects.size();
        }

        size_type managed_count() const
        {
            scoped_lock_type pool_lock(m_pool_mutex);
            return m_managed_count;
        }

        size_type capacity() const
        {
            scoped_lock_type pool_lock(m_pool_mutex);
            return m_capacity;
        }

        bool in_use() const
        {
            scoped_lock_type pool_lock(m_pool_mutex);
            return (m_managed_count - m_free_objects.size()) > 0;
        }

        bool empty() const
        {
            scoped_lock_type pool_lock(m_pool_mutex);
            return m_free_objects.empty();
        }

        explicit
        operator bool() const
        {
            return size() > 0;
        }

        void swap(impl& other)
        {
            scoped_lock_type lock_this(m_pool_mutex);
            scoped_lock_type lock_other(other.m_pool_mutex);

            using std::swap;
            swap(m_managed_count, other.m_managed_count);
            swap(m_capacity, other.m_capacity);
            swap(m_free_objects, other.m_free_objects);
            swap(m_allocator, other.m_allocator);
        }

    private:
        inline void reallocate(size_type new_cap)
        {
            for (size_type i = m_capacity; i < new_cap; ++i)
                m_allocated_space.push(m_allocator.allocate(1));
            m_capacity = new_cap;
        }

        size_type                   m_managed_count;        ///< Number of objects currently in the pool.
        size_type                   m_capacity;             ///< Number of objects the pool can hold.
        allocator_type              m_allocator;            ///< Allocates space for the pool.

        stack_type                  m_allocated_space;      ///< Pointer to first object in the pool.
        stack_type                  m_free_objects;         ///< Stack of free objects.

        mutable mutex_type          m_pool_mutex;           ///< Controls access to the pool.
        std::condition_variable     m_objects_availabe;     ///< Indicates presence of free objects.
    };

    // --- deleter IMPLEMENTATION -----------------------------------
    template <
        class _Tp,
        class _Allocator,
        class _Mutex
    >
    class object_pool<_Tp, _Allocator, _Mutex>::deleter
    {
    public:
        deleter(std::weak_ptr<impl> pool_ptr)
            : m_pool_ptr(pool_ptr)
        {}

        void operator()(_Tp* ptr)
        {
            if (ptr)
            {
                if (auto pool = m_pool_ptr.lock())
                    pool->return_object(ptr);
            }
        }
    private:
        std::weak_ptr<impl> m_pool_ptr;
    };

    template <
        class _Tp,
        class _Allocator,
        class _Mutex
    >
    class object_pool<_Tp, _Allocator, _Mutex>::acquired_object
    {
    public:
        acquired_object()
            : m_is_initialized(false)
        {}

        acquired_object(none_t)
            : m_is_initialized(false)
        {}

        acquired_object(std::nullptr_t)
            : m_is_initialized(false)
        {}

        explicit
        acquired_object(_Tp* obj, std::shared_ptr<impl> lender)
            :   m_obj(obj),
                m_pool(lender),
                m_is_initialized(true)
        {
            assert(obj);
        }

        acquired_object(acquired_object&& other)
            :   m_obj(other.m_obj),
                m_pool(std::move(other.m_pool)),
                m_is_initialized(other.m_is_initialized)
        {
            other.m_obj = nullptr;
            other.m_is_initialized = false;
        }

        acquired_object(const acquired_object&) = delete;
        acquired_object& operator=(const acquired_object&) = delete;

        acquired_object& operator=(acquired_object&& other)
        {
            if (m_is_initialized)
                object_pool::deleter{m_pool}(m_obj);

            m_obj = other.m_obj;
            m_is_initialized = other.m_is_initialized;

            other.m_obj = nullptr;
            other.m_is_initialized = false;

            m_pool = std::move(other.m_pool);

            return *this;
        }

        acquired_object& operator=(none_t)
        {
            if (m_is_initialized)
                object_pool::deleter{m_pool}(m_obj);

            m_obj = nullptr;
            m_pool = nullptr;
            m_is_initialized = false;

            return *this;
        }

        ~acquired_object()
        {
            if (m_is_initialized)
                object_pool::deleter{m_pool}(m_obj);
        }

        _Tp& operator*() 
        {
            if (!m_is_initialized)
                throw std::logic_error("acquired_object::operator*(): Initialization is required for data access.");
            else
                return *m_obj;
        }

        _Tp* operator->()
        {
            if (!m_is_initialized)
                throw std::logic_error("acquired_object::operator->(): Initialization is required for data access.");
            else
                return m_obj;
        }

        bool operator==(const none_t) const
        {
            return !m_is_initialized;
        }

        bool operator!=(const none_t) const
        {
            return !(*this == none);
        }

        explicit
        operator bool() const
        {
            return m_is_initialized;
        }

    private:
        _Tp*                        m_obj;
        std::shared_ptr<impl>       m_pool;
        bool                        m_is_initialized;
    };
}
#endif