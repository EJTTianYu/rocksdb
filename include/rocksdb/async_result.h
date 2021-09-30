// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#include <liburing.h>
#include <sys/uio.h>
#include <coroutine>
#include <iostream>
#include "rocksdb/status.h"
#include "io_status.h"

namespace ROCKSDB_NAMESPACE {

struct file_page;

struct async_result {
  struct promise_type {
    ~promise_type() {
      auto hh = std::coroutine_handle<promise_type>::from_promise(*this);
      std::cout <<"handle:" << hh.address() << " done:" << hh.done() << std::endl;
    }

    async_result get_return_object() {
      auto h = std::coroutine_handle<promise_type>::from_promise(*this);
      std::cout << "Send back a return_type with handle:" << h.address() << std::endl;
      return async_result(h, this);
    }

    auto initial_suspend() { return std::suspend_never{};}

    auto final_suspend() noexcept {
      auto hh = std::coroutine_handle<promise_type>::from_promise(*this);
      std::cout << " this promise in final suspend, handler" << hh.address()
                << " prev:" << prev_ << std::endl;
      if (prev_ != nullptr) {
        auto h = std::coroutine_handle<promise_type>::from_promise(*prev_);
        std::cout << "resume prev here, prev handle" << h.address() << std::endl;
        h.resume();
      }

      return std::suspend_never{};
    }

    void unhandled_exception() { std::exit(1); }

    void return_value(Status result) { 
      result_ = result; 
      result_set_ = true;
      auto h = std::coroutine_handle<promise_type>::from_promise(*this);
      std::cout << "result_set=" << h.promise().result_set_ << ",address=" <<
          &h.promise().result_set_ << std::endl;
    }

    void return_value(IOStatus io_result) {
      io_result_ = io_result;
      result_set_ = true;
      auto h = std::coroutine_handle<promise_type>::from_promise(*this);
      std::cout << "result_set=" << h.promise().result_set_ << std::endl;
    }

    void return_value(bool posix_write_result) {
      posix_write_result_ = posix_write_result;
      result_set_ = true;
      auto h = std::coroutine_handle<promise_type>::from_promise(*this);
      std::cout << "result_set=" << h.promise().result_set_ << std::endl;
    }

    promise_type* prev_ = nullptr;
    bool result_set_ = false;
    // different return type by coroutine
    Status result_;
    IOStatus io_result_;
    bool posix_write_result_;
  };

  async_result() : async_(false) {}

  async_result(bool async, struct file_page* context) : async_(async), context_(context) {}

  async_result(std::coroutine_handle<promise_type> h, promise_type *promise) : h_{h} {
    promise_ = promise;
  }

  bool await_ready() const noexcept { 
    if (async_) {
      return false;
    } else {
      std::cout<<"h_address"<<h_.address()<<std::endl;
      std::cout<<"h_.done():"<<h_.done()<<"\n";
      std::cout<<"result_set_:"<<h_.promise().result_set_<< ",address=" <<
                               &h_.promise().result_set_ <<"\n";
      std::cout<<"promise:"<<promise_->result_set_<<",address=" <<
          &promise_->result_set_<<std::endl;
      return h_.promise().result_set_;
    }
  }

  void await_suspend(std::coroutine_handle<promise_type> h);

  void await_resume() const noexcept {}

  Status result() { return h_.promise().result_; }

  IOStatus io_result() { return h_.promise().io_result_; }

  bool posix_result() { return h_.promise().posix_write_result_; }

  // test only
  bool is_result_set() { return h_.promise().result_set_; }

  std::coroutine_handle<promise_type> h_;
  // to store co_return value
  struct promise_type* promise_;
  bool async_ = false;
  struct file_page* context_;
};

// used for liburing read or write
struct file_page {
  file_page(int pages) {
    iov = (iovec*)calloc(pages, sizeof(struct iovec));
  }

  async_result::promise_type* promise;
  struct iovec *iov;
};

}// namespace ROCKSDB_NAMESPACE



