#pragma once
#include <uv.h> // libuv
#include <functional>
#include <memory>
#include <list>
#include <string.h>
#include <string>
#include <atomic>
#include <tuple>
#include <vector>
#include <assert.h>

#if __has_include(<experimental/coroutine>)
#include <experimental/coroutine>
#else
#include <experimental\resumable>
#endif

namespace awaituv
{
// awaitable_common holds some simple stuff that all awaitables here are based on.
struct awaitable_common
{
  //std::experimental::coroutine_handle<> callback_ = nullptr;
  std::function<void(void)> callback_ = nullptr;
  bool ready_ = false;

  void reset()
  {
    callback_ = nullptr;
    ready_ = false;
  }

  void set_value()
  {
    // Set all members first as calling coroutine may reset stuff here.
    ready_ = true;
    auto coro = callback_;
    callback_ = nullptr;
    //if (coro != nullptr)
    //  coro.resume();
    if (coro != nullptr)
      coro();
  }

  // functions that make this awaitable
  bool await_ready() const
  {
    return ready_;
  }

  // set_callback is used by multi_awaitable
  void set_callback(std::function<void(void)> cb)
  {
    // Test to make sure nothing else is waiting on this future.
    assert(((cb == nullptr) || (callback_ == nullptr)) && "This awaitable is already being awaited.");
    callback_ = cb;
  }

  void await_suspend(std::experimental::coroutine_handle<> resume_cb)
  {
    // Test to make sure nothing else is waiting on this future.
    assert(((resume_cb == nullptr) || (callback_ == nullptr)) && "This awaitable is already being awaited.");
    callback_ = resume_cb;
  }
};

/*
  awaitable_state is used to return an awaitable type from a function
  that is not itself a coroutine (i.e. it doesn't use co_await/co_return/co_yield)
  The awaitable_state object is allocated outside of the function and passed in
  and then returned by reference from the function. This state provides a 
  location for the eventual value to be stored. Returning it from the function 
  allows the function to be directly co_await'ed.
*/
template <typename T>
struct awaitable_state : public awaitable_common
{
  T value_;

  awaitable_state() = default;
  // movable, but not copyable
  awaitable_state(const awaitable_state&) = delete;
  awaitable_state& operator=(const awaitable_state&) = delete;
  awaitable_state(awaitable_state&& f) = default;
  awaitable_state& operator=(awaitable_state&&) = default;

  void reset()
  {
    awaitable_common::reset();
    value_ = T{};
  }

  using awaitable_common::set_value;
  void set_value(const T& t)
  {
    value_ = t;
    set_value();
  }

  void set_value(T&& t)
  {
    std::swap(value_, t);
    set_value();
  }

  // functions that make this awaitable
  auto await_resume() const
  {
    return value_;
  }

  T&& await_resume()
  {
    return std::move(value_);
  }
};

// specialization of awaitable_state<void>
template <>
struct awaitable_state<void> : public awaitable_common
{
  awaitable_state() = default;
  awaitable_state(awaitable_state&&) = delete;
  awaitable_state(const awaitable_state&) = delete;

  // functions that make this awaitable
  void await_resume() const
  {
  }
};

template <typename T>
struct awaitable_t : public awaitable_common
{
  struct promise_type
  {
    T value_ = {};
    bool ready_ = false;
    awaitable_t* awaitable_ = nullptr; // points to one and only awaitable_t

    awaitable_t<T> get_return_object()
    {
      assert(awaitable_ == nullptr);
      return awaitable_t<T>(*this);
    }

    template <typename X>
    void return_value(const X& val, typename std::is_assignable<T, X>::type* = nullptr)
    {
      if (awaitable_ == nullptr)
      {
        value_ = val;
        ready_ = true;
      }
      else
        awaitable_->return_value_helper(val);
    }

    void return_value(T&& val)
    {
      if (awaitable_ == nullptr)
      {
        std::swap(value_, val);
        ready_ = true;
      }
      else
        awaitable_->return_value_helper(std::move(val));
    }

    std::experimental::suspend_never initial_suspend() const { return {}; }
    std::experimental::suspend_never final_suspend() const { return {}; }
  };

  const T& await_resume() const
  {
    return value_;
  }

  T&& await_resume()
  {
    return std::move(value_);
  }

  // forwarded from promise
  template <typename X>
  void return_value_helper(const X& val, typename std::is_assignable<T, X>::type* = nullptr)
  {
    value_ = val;
    set_value();
  }

  void return_value_helper(T&& val)
  {
    std::swap(value_, val);
    set_value();
  }

  awaitable_t(promise_type& promise) : promise_(promise)
  {
    promise_.awaitable_ = this;
    ready_ = promise_.ready_;
    value_ = std::move(promise_.value_);
  }
  // move ctor, but nothing else
  awaitable_t& operator=(const awaitable_t&) = delete;
  awaitable_t(const awaitable_t&) = delete;
  awaitable_t& operator=(awaitable_t&& f) = delete;
  awaitable_t(awaitable_t&& f) : promise_(f.promise_)
  {
    assert(f.callback_ == nullptr); // not awaited yet
    ready_ = f.ready_;
    value_ = std::move(f.value_);
    promise_.awaitable_ = this;
  }

  promise_type& promise_;
  T value_ = {};
};

/*
  The awaitable_t class is used to return an awaitable from a function
  that is itself a coroutine. The awaitable_t/promise_type both provide
  potential storage for the eventual value. 
  This allows two things:
  1) The async operation can start(and finish) without waiting for co_await to be called.
  2) Avoids requiring the awaitable to ever be co_await'ed. Cleanup still happens correctly.
  The awaitable is movable and whenever it is moved, it updates the promise to
  point to itself. There can be only one awaitable for a promise. Because libuv is single
  threaded, this is dramatically simpler as we don't need to worry about races.
*/
template <>
struct awaitable_t<void> : public awaitable_common
{
  struct promise_type
  {
    bool ready_ = false;
    awaitable_t* awaitable_ = nullptr; // points to one and only awaitable_t

    awaitable_t get_return_object()
    {
      assert(awaitable_ == nullptr);
      return awaitable_t(*this);
    }

    void return_void()
    {
      if (awaitable_ == nullptr)
        ready_ = true;
      else
        awaitable_->return_void();
    }

    std::experimental::suspend_never initial_suspend() const { return {}; }
    std::experimental::suspend_never final_suspend() const { return {}; }
  };

  void await_resume() const
  {
  }

  // forwarded from promise
  void return_void()
  {
    set_value();
  }

  awaitable_t(promise_type& promise) : promise_(promise)
  {
    promise_.awaitable_ = this;
    ready_ = promise_.ready_;
  }
  // move ctor, but nothing else
  awaitable_t& operator=(const awaitable_t&) = delete;
  awaitable_t& operator=(awaitable_t&&) = delete;
  awaitable_t(const awaitable_t&) = delete;
  awaitable_t(awaitable_t&& f) : promise_(f.promise_)
  {
    assert(f.callback_ == nullptr); // not awaited yet
    ready_ = f.ready_;
    promise_.awaitable_ = this;
  }

  promise_type& promise_;
};


// future_of_all is pretty trivial as we can just await on each argument
template <typename T>
awaitable_t<void> future_of_all(T& f)
{
  co_await f;
}

template <typename T, typename... Rest>
awaitable_t<void> future_of_all(T& f, Rest&... args)
{
  co_await f;
  co_await future_of_all(args...);
}

// future_of_all_range will return a vector of results when all futures complete
template <typename Iterator>
auto future_of_all_range(Iterator begin, Iterator end) -> awaitable_t<std::vector<typename std::remove_reference<decltype(begin->await_resume())>::type>>
{
  std::vector<typename std::remove_reference<decltype(begin->await_resume())>::type> vec;
  while (begin != end)
  {
    vec.emplace_back(co_await *begin);
    ++begin;
  }
  co_return std::move(vec);
}

#if 0
// Define some helper templates to iterate through each element
// of the tuple
template <typename tuple_t, size_t N>
struct callback_helper_t
{
  static void set(tuple_t& tuple, std::function<void(void)> cb)
  {
    std::get<N>(tuple).set_callback(cb);
    callback_helper_t<tuple_t, N-1>::set(tuple, cb);
  }
};
// Specialization for last item
template <typename tuple_t>
struct callback_helper_t<tuple_t, 0>
{
  static void set(tuple_t& tuple, std::function<void(void)> cb)
  {
    std::get<0>(tuple).set_callback(cb);
  }
};

template <typename tuple_t>
void set_callback_helper(tuple_t& tuple, std::function<void(void)> cb)
{
  callback_helper_t<tuple_t, std::tuple_size<tuple_t>::value - 1>::set(tuple, cb);
}

// allows waiting for just one future to complete
template <typename... Rest>
struct multi_awaitable_state : public awaitable_state<void>
{
  // Store references to all the futures passed in.
  std::tuple<Rest&...> _futures;
  multi_awaitable_state(Rest&... args) : _futures(args...)
  {
  }

  ~multi_awaitable_state()
  {
  }

  void await_suspend(std::experimental::coroutine_handle<> resume_cb)
  {
    // Test to make sure nothing else is waiting on this future.
    assert(((resume_cb == nullptr) || (callback_ == nullptr)) && "This awaitable is already being awaited.");
    callback_ = resume_cb;

    // Make any completion of a future call any_completed
    std::function<void(void)> func = std::bind(&multi_awaitable_state::any_completed, this);
    set_callback_helper(_futures, func);
  }

  // any_completed will be called by any future completing
  void any_completed()
  {
    set_callback_helper(_futures, nullptr);
    set_value();
  }
};

template <typename T>
T& future_of_any(T& multistate)
{
  return multistate;
}

// future_of_any is pretty complicated
// We have to create a new promise with a custom awaitable state object
template <typename T, typename... Rest>
awaitable_t<void> future_of_any(T& f, Rest&... args)
{
  multi_awaitable_state<T, Rest...> state(f, args...);
  co_return co_await future_of_any(state);
}

// iterator_awaitable_state will track the index of which future completed
template <typename Iterator>
struct iterator_awaitable_state : public awaitable_state<Iterator>
{
  Iterator begin_;
  Iterator end_;
  iterator_awaitable_state(Iterator begin, Iterator end) : begin_(begin), end_(end)
  {
  }

  // any_completed will be called by any future completing
  void any_completed(Iterator completed)
  {
    // stop any other callbacks from coming in
    for (Iterator c = begin_; c != end_; ++c)
      c->state_->set_callback(nullptr);
    set_value(completed);
  }

  void set_callback(std::function<void(void)> cb)
  {
    for (Iterator c = begin_; c != end_; ++c)
    {
      std::function<void(void)> func = std::bind(&iterator_awaitable_state::any_completed, this, c);
      c->state_->set_callback(func);
    }
    awaitable_state<Iterator>::set_callback(cb);
  }
};

// returns the index of the iterator that succeeded
template <typename Iterator>
future_t<Iterator, iterator_awaitable_state<Iterator>> future_of_any_range(Iterator begin, Iterator end)
{
  promise_t<Iterator, iterator_awaitable_state<Iterator>> promise(begin, end);
  return promise.get_future();
}
#endif

// Simple RAII for uv_loop_t type
class loop_t : public ::uv_loop_t
{
  int status_ = -1;
public:
  loop_t& operator=(const loop_t&) = delete; // no copy
  loop_t()
  {
    status_ = uv_loop_init(this);
    if (status_ != 0)
      throw std::exception();
  }
  ~loop_t()
  {
    if (status_ == 0)
      uv_loop_close(this);
  }
  int run()
  {
    return uv_run(this, UV_RUN_DEFAULT);
  }
  int run(uv_run_mode mode)
  {
    return uv_run(this, mode);
  }
};

// Simple RAII for uv_fs_t type
struct fs_t : public ::uv_fs_t
{
  ~fs_t()
  {
    ::uv_fs_req_cleanup(this);
  }
  fs_t() = default;
  // movable, but not copyable
  fs_t(const fs_t&) = delete;
  fs_t& operator=(const fs_t&) = delete;
  fs_t(fs_t&& f) = default;
  fs_t& operator=(fs_t&&) = default;
};

struct getaddrinfo_t : public uv_getaddrinfo_t
{
  getaddrinfo_t()
  {
    this->addrinfo = nullptr;
  }
  ~getaddrinfo_t()
  {
    ::uv_freeaddrinfo(addrinfo);
  }
  // movable, but not copyable
  getaddrinfo_t(const getaddrinfo_t&) = delete;
  getaddrinfo_t& operator=(const getaddrinfo_t&) = delete;
  getaddrinfo_t(getaddrinfo_t&& f) = default;
  getaddrinfo_t& operator=(getaddrinfo_t&&) = default;

};

// Fixed size buffer
template <size_t size>
struct static_buf_t : ::uv_buf_t
{
  char buffer[size];
  static_buf_t()
  {
    *(uv_buf_t*)this = uv_buf_init(buffer, sizeof(buffer));
  }
};

// Buffer based on null-terminated string
struct string_buf_t : ::uv_buf_t
{
  string_buf_t(const char* p)
  {
    *(uv_buf_t*)this = uv_buf_init(const_cast<char*>(p), strlen(p));
  }
  string_buf_t(const char* p, size_t len)
  {
    *(uv_buf_t*)this = uv_buf_init(const_cast<char*>(p), len);
  }
};

// is_uv_handle_t checks for three data members: data, loop, and type.
// These members mean this type is convertible to a uv_handle_t. This
// can be used to make it easier to call functions that take a handle.
template <typename T, typename = int, typename = int, typename = int>
struct is_uv_handle_t : std::false_type
{
};

template <typename T>
struct is_uv_handle_t<T, decltype((void)T::data, 0), decltype((void)T::loop, 0), decltype((void)T::type, 0)> : std::true_type
{
};

template <typename T>
auto unref(T* handle, typename std::enable_if<is_uv_handle_t<T>::value>::type* dummy = nullptr)
{
  uv_unref(reinterpret_cast<uv_handle_t*>(handle));
}

template <typename T>
auto ref(T* handle, typename std::enable_if<is_uv_handle_t<T>::value>::type* dummy = nullptr)
{
  uv_ref(reinterpret_cast<uv_handle_t*>(handle));
}

// return reference to passed in awaitable so that fs_open is directly awaitable
inline auto& fs_open(awaitable_state<uv_file>& awaitable, uv_loop_t* loop, uv_fs_t* req, const char* path, int flags, int mode)
{
  req->data = &awaitable;

  auto ret = uv_fs_open(loop, req, path, flags, mode,
    [](uv_fs_t* req) -> void
  {
    static_cast<awaitable_state<uv_file>*>(req->data)->set_value(req->result);
  });

  if (ret != 0)
    awaitable.set_value(ret);
  return awaitable;
}

inline awaitable_t<uv_file> fs_open(uv_loop_t* loop, const char* path, int flags, int mode)
{
  fs_t req;
  awaitable_state<uv_file> state;
  co_return co_await fs_open(state, loop, &req, path, flags, mode);
}

inline auto& fs_close(awaitable_state<int>& awaitable, uv_loop_t* loop, uv_fs_t* req, uv_file file)
{
  req->data = &awaitable;

  auto ret = uv_fs_close(loop, req, file,
    [](uv_fs_t* req) -> void
  {
    static_cast<awaitable_state<int>*>(req->data)->set_value(req->result);
  });

  if (ret != 0)
    awaitable.set_value(ret);
  return awaitable;
}

inline awaitable_t<int> fs_close(uv_loop_t* loop, uv_file file)
{
  fs_t req;
  awaitable_state<int> state;
  co_return co_await fs_close(state, loop, &req, file);
}

inline auto& fs_write(awaitable_state<int>& awaitable, uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)
{
  req->data = &awaitable;

  auto ret = uv_fs_write(loop, req, file, bufs, nbufs, offset,
    [](uv_fs_t* req) -> void
  {
    static_cast<awaitable_state<int>*>(req->data)->set_value(req->result);
  });

  if (ret != 0)
    awaitable.set_value(ret);
  return awaitable;
}

inline awaitable_t<int> fs_write(uv_loop_t* loop, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)
{
  fs_t req;
  awaitable_state<int> state;
  co_return co_await fs_write(state, loop, &req, file, bufs, nbufs, offset);
}

inline auto& fs_read(awaitable_state<int>& awaitable, uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)
{
  req->data = &awaitable;

  auto ret = uv_fs_read(loop, req, file, bufs, nbufs, offset,
    [](uv_fs_t* req) -> void
  {
    static_cast<awaitable_state<int>*>(req->data)->set_value(req->result);
  });

  if (ret != 0)
    awaitable.set_value(ret);
  return awaitable;
}

inline awaitable_t<int> fs_read(uv_loop_t* loop, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)
{
  fs_t req;
  awaitable_state<int> state;
  co_return co_await fs_read(state, loop, &req, file, bufs, nbufs, offset);
}

// generic stream functions
inline auto& write(awaitable_state<int>& awaitable, ::uv_write_t* req, uv_stream_t* handle, const uv_buf_t bufs[], unsigned int nbufs)
{
  req->data = &awaitable;

  auto ret = uv_write(req, handle, bufs, nbufs,
    [](uv_write_t* req, int status) -> void
  {
    static_cast<awaitable_state<int>*>(req->data)->set_value(status);
  });

  if (ret != 0)
    awaitable.set_value(ret);
  return awaitable;
}

inline awaitable_t<int> write(uv_stream_t* handle, const uv_buf_t bufs[], unsigned int nbufs)
{
  uv_write_t req;
  awaitable_state<int> state;
  co_return co_await write(state, &req, handle, bufs, nbufs);
}

template <typename T>
auto& close(awaitable_state<void>& awaitable, T* handle, typename std::enable_if<is_uv_handle_t<T>::value>::type* dummy = nullptr)
{
  handle->data = &awaitable;

  // uv_close returns void so no need to test return value
  uv_close(reinterpret_cast<uv_handle_t*>(handle),
    [](uv_handle_t* req) -> void
  {
    static_cast<awaitable_state<void>*>(req->data)->set_value();
  });
  return awaitable;
}

template <typename T>
inline awaitable_t<void> close(T* handle, typename std::enable_if<is_uv_handle_t<T>::value>::type* dummy = nullptr)
{
  awaitable_state<void> state;
  co_await close(state, handle);
}

struct timer_state_t : public awaitable_state<int>
{
  timer_state_t& next()
  {
    reset();
    return *this;
  }
};

inline auto& timer_start(timer_state_t& awaitable, uv_timer_t* timer, uint64_t timeout, uint64_t repeat)
{
  timer->data = &awaitable;

  auto ret = ::uv_timer_start(timer,
    [](uv_timer_t* req) -> void
  {
    static_cast<timer_state_t*>(req->data)->set_value(0);
  }, timeout, repeat);

  if (ret != 0)
    awaitable.set_value(ret);
  return awaitable;
}

inline auto& timer_start(awaitable_state<int>& awaitable, uv_timer_t* timer, uint64_t timeout)
{
  timer->data = &awaitable;

  auto ret = ::uv_timer_start(timer,
    [](uv_timer_t* req) -> void
  {
    static_cast<timer_state_t*>(req->data)->set_value(0);
  }, timeout, 0);

  if (ret != 0)
    awaitable.set_value(ret);
  return awaitable;
}

inline awaitable_t<int> timer_start(uint64_t timeout)
{
  awaitable_state<int> state;
  uv_timer_t timer;
  co_return co_await timer_start(state, &timer, timeout);
}

inline auto& tcp_connect(awaitable_state<int>& awaitable, uv_connect_t* req, uv_tcp_t* socket, const struct sockaddr* dest)
{
  req->data = &awaitable;

  auto ret = ::uv_tcp_connect(req, socket, dest,
    [](uv_connect_t* req, int status) -> void
  {
    static_cast<awaitable_state<int>*>(req->data)->set_value(status);
  });

  if (ret != 0)
    awaitable.set_value(ret);
  return awaitable;
}

inline awaitable_t<int> tcp_connect(uv_connect_t* req, uv_tcp_t* socket, const struct sockaddr* dest)
{
  awaitable_state<int> state;
  co_return co_await tcp_connect(state, req, socket, dest);
}

inline auto& getaddrinfo(awaitable_state<int>& awaitable, uv_loop_t* loop, uv_getaddrinfo_t* req, const char* node, const char* service, const struct addrinfo* hints)
{
  req->data = &awaitable;

  auto ret = ::uv_getaddrinfo(loop, req,
    [](uv_getaddrinfo_t* req, int status, struct addrinfo* res) -> void
  {
    assert(res == req->addrinfo);
    static_cast<awaitable_state<int>*>(req->data)->set_value(status);
  }, node, service, hints);

  if (ret != 0) {
    assert(req->addrinfo == nullptr);
    awaitable.set_value(ret);
  }
  return awaitable;
}

inline awaitable_t<int> getaddrinfo(uv_loop_t* loop, uv_getaddrinfo_t* req, const char* node, const char* service, const struct addrinfo* hints)
{
  awaitable_state<int> state;
  co_return co_await getaddrinfo(state, loop, req, node, service, hints);
}

struct buffer_info
{
  uv_buf_t buf_ = uv_buf_init(nullptr, 0);
  ssize_t nread_{ 0 };

  buffer_info() = default;
  buffer_info(const uv_buf_t* buf, ssize_t nread)
  {
    memcpy(&buf_, buf, sizeof(uv_buf_t));
    nread_ = nread;
  }
  ~buffer_info()
  {
    if (buf_.base != nullptr)
      delete[] buf_.base;
  }
  // movable, but not copyable
  buffer_info(const buffer_info&) = delete;
  buffer_info& operator=(const buffer_info&) = delete;
  buffer_info(buffer_info&& f)
  {
    buf_ = f.buf_;
    memset(&f.buf_, 0, sizeof(uv_buf_t));
    nread_ = f.nread_;
  }
  buffer_info& operator=(buffer_info&& f)
  {
    std::swap(buf_, f.buf_);
    std::swap(nread_, f.nread_);
    return *this;
  }
};

// For reads, we need to define a new type to hold the completed read callbacks as we may not have
// a future for them yet.  This is somewhat equivalent to other libuv functions that take a uv_write_t
// or a uv_fs_t.
// This is a little convoluted as uv_read_start is not a one-shot read, but continues to provide
// data to its callback.  So, we need to handle two cases.  One is where the future is created before
// the data is passed to the callback and the second is where the future is not created first.
class read_request_t
{
  // We have data to provide.  If there is already a promise that has a future, then
  // use that.  Otherwise, we need to create a new promise for this new data.
  void add_buffer(ssize_t nread, const uv_buf_t* buf)
  {
    buffer_info info{ buf, nread };
    if (waiting != nullptr)
    {
      auto p = waiting;
      waiting = nullptr;
      p->set_value(std::move(info));
    }
    else
    {
      buffers_.emplace_back(std::move(info));
    }
  }

  std::list<buffer_info> buffers_;
  awaitable_state<buffer_info>* waiting = nullptr;

public:
  read_request_t() = default;
  // no copy/move
  read_request_t(const read_request_t&) = delete;
  read_request_t(read_request_t&&) = delete;
  read_request_t& operator=(const read_request_t&) = delete;
  read_request_t& operator=(read_request_t&&) = default;

  void clear()
  {
    buffers_.clear();
  }

  // We may already have a promise with data available so check for that first.
  awaitable_state<buffer_info>& read_next(awaitable_state<buffer_info>& awaitable)
  {
    if (!buffers_.empty())
    {
      awaitable.set_value(std::move(buffers_.front()));
      buffers_.pop_front();
    }
    else
    {
      assert(waiting == nullptr);
      waiting = &awaitable;
    }

    return awaitable;
  }

  awaitable_t<buffer_info> read_next()
  {
    awaitable_state<buffer_info> buffer;
    co_return co_await read_next(buffer);
  }

  // note: read_start does not return a future. All futures are acquired through read_request_t::read_next
  inline int read_start(uv_stream_t* handle)
  {
    uv_read_stop(handle);
    clear();

    handle->data = this;

    int res = uv_read_start(handle,
      [](uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
    {
      *buf = uv_buf_init(new char[suggested_size], suggested_size);
    },
      [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
    {
      auto reader = reinterpret_cast<read_request_t*>(stream->data);
      reader->add_buffer(nread, buf);
    }
    );

    return res;
  }
};

inline awaitable_t<std::string> stream_to_string(uv_stream_t* handle)
{
  read_request_t reader;
  std::string str;
  if (reader.read_start(handle) == 0)
  {
    while (1)
    {
      awaitable_state<buffer_info> buffer;
      auto state = co_await reader.read_next(buffer);
      if (state.nread_ <= 0)
        break;
      str.append(state.buf_.base, state.nread_);
    }
  }
  co_return str;
}
} // namespace awaituv
