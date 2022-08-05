// @Author Lin Ya
// @Email xxbbb@vip.qq.com
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "CountDownLatch.h"
#include "LogStream.h"
#include "MutexLock.h"
#include "Thread.h"
#include "noncopyable.h"


class AsyncLogging : noncopyable {
 public:
  AsyncLogging(const std::string basename, int flushInterval = 2);
  ~AsyncLogging() {
    if (running_) stop();
  }
  void append(const char* logline, int len);

  void start() {
    running_ = true;
    thread_.start();
    latch_.wait();
  }

  void stop() {
    running_ = false;
    cond_.notify();
    thread_.join();
  }

 private:
  typedef FixedBuffer<kLargeBuffer> Buffer;
  typedef std::shared_ptr<Buffer> BufferPtr;
  typedef std::vector<BufferPtr> BufferptrVector;

  // main thread
  void threadFunc();

  // information
  bool running_;
  std::string basename_;
  Thread thread_;

  // buffer
  BufferPtr currentBuffer_;
  BufferPtr nextBuffer_;
  BufferptrVector buffers_;

  // synchronization
  const int flushInterval_;
  MutexLock mutex_;
  Condition cond_;
  CountDownLatch latch_;
};