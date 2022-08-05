# WebServer 学习笔记

Github 项目地址：[github - linyacool](https://github.com/linyacool/WebServer)

-------------------------------------

## 一些 base 类型

### 1. Thread 类型

```cpp
class Thread : noncopyable {
 public:
  typedef std::function<void()> ThreadFunc;
  explicit Thread(const ThreadFunc&, const std::string& name = std::string());
  ~Thread();
  void start();
  int join();
  bool started() const { return started_; }
  pid_t tid() const { return tid_; }
  const std::string& name() const { return name_; }

 private:
  void setDefaultName();
  bool started_;
  bool joined_;
  pthread_t pthreadId_;
  pid_t tid_;
  ThreadFunc func_;
  std::string name_;
  CountDownLatch latch_;
};
```



## Log 的设计

先引用作者的话：

>   与 Log 相关的类包括 AppendFile、LogFile、AsyncLogging、LogStream、Logger。
>
>   其中前4个类每一个类都含有一个 append 函数，Log 的设计也是主要围绕这个 append 函数展开的。

>   AppendFile 是最底层的文件类，封装了 Log 文件的打开、写入并在类析构的时候关闭文件，底层使用了标准 IO，该 append 函数直接向文件写。
>
>   LogFile 进一步封装了 AppendFile，并设置了一个循环次数，每过这么多次就 flush 一次。
>
>   AsyncLogging 是核心，它负责启动一个 log 线程，专门用来负责(定时到或被填满时)将缓冲区中的数据写入 LogFile 中，应用了双缓冲技术。
>
>   LogStream 主要用来格式化输出，重载了 << 运算符，同时也有自己的一块缓冲区，这里缓冲区的存在是为了缓存一行，把多个 << 的结果连成一块。
>
>   Logger 是对外接口，Logger 类内涵一个 LogStream 对象，主要是为了每次打 log 的时候在 log 之前和之后加上固定的格式化的信息，比如打 log 的行、文件名等信息。

### 用到的主要技术总结

-   类分层结构设计
-   双缓冲技术
-   `__thread` 关键字，TSL(thread local storage) 技术，用来缓存 tid，减少系统调用次数

### 1. AppendFile 

```cpp
class AppendFile : noncopyable {
 public:
  explicit AppendFile(std::string filename);
  ~AppendFile();
  // append 会向文件写
  void append(const char *logline, const size_t len);
  void flush();

 private:
  size_t write(const char *logline, size_t len);
  FILE *fp_;
  char buffer_[64 * 1024];
};
```

-   先调用 `setbuffer` 设置文件的缓冲区
-   调用 `fwrite_unlock` ，该函数不加锁写文件，是线程不安全的，但速度快
-   有一个 `flush()` 函数，调用 `fflush()`
-   `AppendFile` 全是无锁操作

### 2. LogFile 类

```cpp
class LogFile : noncopyable {
 public:
  // 每被append flushEveryN次，flush一下，会往文件写，只不过，文件也是带缓冲区的
  LogFile(const std::string& basename, int flushEveryN = 1024);
  ~LogFile();

  void append(const char* logline, int len);
  void flush();
  bool rollFile();

 private:
  void append_unlocked(const char* logline, int len);

  const std::string basename_;
  const int flushEveryN_;

  int count_;
  std::unique_ptr<MutexLock> mutex_;
  std::unique_ptr<AppendFile> file_;
};
```

-   `Logfile` 类包含一个 `AppendFile` 类，`append` 调用 `AppendFile` 的 `append`，`flush` 同理
-   每被 `append flushEveryN_` 次，`flush` 一次

简单的说，`LogFile` 干了两件事：

-   加锁调用 `AppendFile` ，保证线程安全
-   设置 `flush` 频率

### 3. AsyncLogging 类

```cpp
class AsyncLogging : noncopyable {
public:
    AsyncLogging(const std::string basename, int flushInterval = 2);
    ~AsyncLogging() {
        if (running_) stop();
    }
    void append(const char* logline, int len);
    void start();
    void stop();

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
```

`AsyncLogging` 是实现日志功能的主要结构体，开了一个 `threadFunc` 线程，作为前后端交互，向日志文件写日志

在 `threadFunc` 中，循环被阻塞在条件变量，当前端写满了一个或多个 `buffer` 或者超时后，条件满足

```cpp
if (buffers_.empty())  // unusual usage!
{
	cond_.waitForSeconds(flushInterval_);
}
```

`currentBuffer_` 移入前端队列 `buffers_`

将空闲的 `newBuffer1` 和 `newBuffer2` 移为 `currentBuffer_` 和 `nextBuffer_`

交换前端和后端的缓冲队列

```cpp
buffers_.push_back(currentBuffer_);
currentBuffer_.reset();

currentBuffer_ = std::move(newBuffer1);
buffersToWrite.swap(buffers_);
if (!nextBuffer_) {
nextBuffer_ = std::move(newBuffer2);
```

若队列缓冲区数大于25，直接丢弃，说明日志写入过快，避免数据在内存中堆积。然后开始调用 `LogFile` 类对象 `output` 写入文件

```cpp
if (buffersToWrite.size() > 25) {
	buffersToWrite.erase(buffersToWrite.begin() + 2, buffersToWrite.end());
}

for (size_t i = 0; i < buffersToWrite.size(); ++i) {
	// FIXME: use unbuffered stdio FILE ? or use ::writev ?
	output.append(buffersToWrite[i]->data(), buffersToWrite[i]->length());
}
```

用 `buffersToWrite` 的前两个缓冲区重新填充 `newBuffer1` 和 `newBuffer2`，清空 `buffersToWrite`

```
if (buffersToWrite.size() > 2) {
    // drop non-bzero-ed buffers, avoid trashing
    buffersToWrite.resize(2);
}

if (!newBuffer1) {
    assert(!buffersToWrite.empty());
    newBuffer1 = buffersToWrite.back();
    buffersToWrite.pop_back();
    newBuffer1->reset();
}

if (!newBuffer2) {
    assert(!buffersToWrite.empty());
    newBuffer2 = buffersToWrite.back();
    buffersToWrite.pop_back();
    newBuffer2->reset();
}
```

最后 `flush` 一下

```
output.flush();
```

### 4. LogStream 类

日志流输出类，包含一个缓冲区，重载了各种类型的 `<<` 运算符，存储日志输出流到缓冲区

### 5. Logger 类

```cpp
class Logger {
 public:
  Logger(const char *fileName, int line);
  ~Logger();
  LogStream &stream() { return impl_.stream_; }

  static void setLogFileName(std::string fileName) { logFileName_ = fileName; }
  static std::string getLogFileName() { return logFileName_; }

 private:
  class Impl {
   public:
    Impl(const char *fileName, int line);
    Impl() {}
    void formatTime();

    LogStream stream_;
    int line_;
    std::string basename_;
  };
  Impl impl_;
  static std::string logFileName_;
};

#define LOG Logger(__FILE__, __LINE__).stream()
```

每条日志消息格式如下：

![](Log_format.png)

-   LOG << "message" 其实是调用了 Logger 的匿名对象，在构造和析构中完成写入缓冲区等操作。

```cpp
static pthread_once_t once_control_ = PTHREAD_ONCE_INIT;
static AsyncLogging *AsyncLogger_;

std::string Logger::logFileName_ = "./WebServer.log";

void once_init()
{
    AsyncLogger_ = new AsyncLogging(Logger::getLogFileName());
    AsyncLogger_->start();
}

void output(const char* msg, int len)
{
    pthread_once(&once_control_, once_init);
    AsyncLogger_->append(msg, len);
}

Logger::~Logger()
{
    impl_.stream_ << " -- " << impl_.basename_ << ':' << impl_.line_ << '\n';
    const LogStream::Buffer& buf(stream().buffer());
    output(buf.data(), buf.length());
}
```

-   pthread_once 变量表示在多线程环境中，该线程只执行一次，不管后续被多少其他线程再次调用。这里用于启动日志主线程，output 在 Logger 析构函数中调用，调用 append 将保存在 Logstream 缓冲区中的内容写入文件。

## Epoll

```cpp
class Epoll {
 public:
  Epoll();
  ~Epoll();
  void epoll_add(SP_Channel request, int timeout);
  void epoll_mod(SP_Channel request, int timeout);
  void epoll_del(SP_Channel request);
  std::vector<std::shared_ptr<Channel>> poll();
  void add_timer(std::shared_ptr<Channel> request_data, int timeout);
  int getEpollFd() { return epollFd_; }
  void handleExpired();

 private:
  std::vector<std::shared_ptr<Channel>> getEventsRequest(int events_num);
  static const int MAXFDS = 100000;
  int epollFd_;
  //保存所有要监听的事件
  std::vector<epoll_event> events_;
  //保存fd和channel的对应关系，方便根据fd快速找到对应的channel，在epoll_add的时候保存到该数组中
  std::shared_ptr<Channel> fd2chan_[MAXFDS];
  std::shared_ptr<HttpData> fd2http_[MAXFDS];
  TimerManager timerManager_;
};
```

-   一个 Epoll 结构体表示一个 epoll 事件，对应一个 epollFd_，一般来说，程序中只有一个 Epoll 对象



## Channel

```cpp
class Channel {
 private:
  typedef std::function<void()> CallBack;
  EventLoop *loop_;
  int fd_;
  __uint32_t events_;
  __uint32_t revents_;
  __uint32_t lastEvents_;

  // 方便找到上层持有该Channel的对象
  std::weak_ptr<HttpData> holder_;

 private:
  int parse_URI();
  int parse_Headers();
  int analysisRequest();

  CallBack readHandler_;
  CallBack writeHandler_;
  CallBack errorHandler_;
  CallBack connHandler_;

 public:
  Channel(EventLoop *loop);
  Channel(EventLoop *loop, int fd);
  ~Channel();
  int getFd();
  void setFd(int fd);

  void setHolder(std::shared_ptr<HttpData> holder) { holder_ = holder; }
  std::shared_ptr<HttpData> getHolder() {
    std::shared_ptr<HttpData> ret(holder_.lock());
    return ret;
  }

  void setReadHandler(CallBack &&readHandler) { readHandler_ = readHandler; }
  void setWriteHandler(CallBack &&writeHandler) { writeHandler_ = writeHandler; }
  void setErrorHandler(CallBack &&errorHandler) { errorHandler_ = errorHandler; }
  void setConnHandler(CallBack &&connHandler) { connHandler_ = connHandler; }

  void handleEvents();void handleRead();void handleWrite();void handleError();void handleConn();

  void setRevents(__uint32_t ev) { revents_ = ev; }

  void setEvents(__uint32_t ev) { events_ = ev; }
  __uint32_t &getEvents() { return events_; }

  bool EqualAndUpdateLastEvents() {
    bool ret = (lastEvents_ == events_);
    lastEvents_ = events_;
    return ret;
  }

  __uint32_t getLastEvents() { return lastEvents_; }
};
```



## EventLoop

```cpp
class EventLoop {
 public:
  typedef std::function<void()> Functor;
  EventLoop();
  ~EventLoop();
  void loop();
  void quit();
  void runInLoop(Functor&& cb);
  void queueInLoop(Functor&& cb);
  bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }
  void assertInLoopThread() { assert(isInLoopThread()); }
  void shutdown(shared_ptr<Channel> channel) { shutDownWR(channel->getFd()); }
  void removeFromPoller(shared_ptr<Channel> channel) {poller_->epoll_del(channel);}
  void updatePoller(shared_ptr<Channel> channel, int timeout = 0) {poller_->epoll_mod(channel, timeout);}
  void addToPoller(shared_ptr<Channel> channel, int timeout = 0) {poller_->epoll_add(channel, timeout);}

 private:
  // 声明顺序 wakeupFd_ > pwakeupChannel_
  bool looping_;
  shared_ptr<Epoll> poller_;
  int wakeupFd_;
  bool quit_;
  bool eventHandling_;
  mutable MutexLock mutex_;
  std::vector<Functor> pendingFunctors_;
  bool callingPendingFunctors_;
  const pid_t threadId_;
  shared_ptr<Channel> pwakeupChannel_;

  void wakeup();
  void handleRead();
  void doPendingFunctors();
  void handleConn();
};
```

-   `EventLoop` 类和 `Poller` 类属于组合的关系，`EventLoop` 类和 `Channel` 类属于聚合的关系

## EventLoopThread

```cpp
class EventLoopThread : noncopyable {
 public:
  EventLoopThread();
  ~EventLoopThread();
  EventLoop* startLoop();

 private:
  //线程运行函数
  void threadFunc();
  //包含的 EventLoop 对象指针
  EventLoop* loop_;
  bool exiting_;
  //创建的线程
  Thread thread_;
  MutexLock mutex_;
  Condition cond_;
};
```

EventLoopThread 主要干了两件事：

-   创建了一个线程
-   在线程函数中创建一个栈上 EventLoop 对象并调用 EventLoop::loop

```cpp
EventLoop* EventLoopThread::startLoop() {
  assert(!thread_.started());
  thread_.start();
  {
    MutexLockGuard lock(mutex_);
    // 一直等到threadFun在Thread里真正跑起来
    while (loop_ == NULL) cond_.wait();
  }
  return loop_;
}

void EventLoopThread::threadFunc() {
  EventLoop loop;

  {
    MutexLockGuard lock(mutex_);
    loop_ = &loop;
    cond_.notify();
  }

  loop.loop();
  loop_ = NULL;
}
```

这里使用条件变量的原因：threadFunc() 和 return loop_ 执行的顺序是不确定的，要确保 EventLoop 对象创立了之后再返回指针

## EventLoopThreadPool

```cpp
class EventLoopThreadPool : noncopyable {
 public:
  EventLoopThreadPool(EventLoop* baseLoop, int numThreads);

  ~EventLoopThreadPool() { LOG << "~EventLoopThreadPool()"; }
  void start();

  EventLoop* getNextLoop();

 private:
  EventLoop* baseLoop_;
  bool started_;
  int numThreads_;
  int next_;
  std::vector<std::shared_ptr<EventLoopThread>> threads_;
  std::vector<EventLoop*> loops_;
};
```

-   包含 n+1 个 EventLoop，一个是传进来的 baseloop_，另外 n 个是 new 出来的 EventLoopThread 包含的
-   用 shared_ptr 保存 n 个 EventLoopThread  对象，管理生命周期
-   用裸指针保存 n 个 EventLoop 对象，避免其生存周期意外延长

## HTTP 请求报文解析

使用状态机，主要分为三个阶段：解析请求行，解析请求首部，创造响应报文

支持管线化，即一次read得到的数据可能不止一个请求

## Timer 定时器

维护了一个 TimerQueue，存放结点 TimerNode，结点中存放 Httpdata 指针

加入队列的时机有两个：

-   新连接到来时 accept，创建一个 timer_node，加入队列
-   每次触发事件，在 HandleRead 的时候，都删除结点（并不是真的删除，而是置结点里面的 httpdata 指针为空指针），之后重新加入。
-   重新加入时，根据 keep_alive 属性确认超时时间，短连接 2s，长连接 300s

从队列删除的时机有两个：

-   事件一到来立马删除该事件对应的结点
-   事件到来后，遍历优先队列

延迟删除：

-   只有在事件到来后才调用定时器处理事件：删除为空指针的结点和超时的结点
-   延迟删除好处：
    -   若 IO 线程忙，那么检查 Timer 队列的间隔也会短，释放 fd 资源快
    -   若 IO 线程空闲，也给了超时请求更长的等待时间
    -   不需要一超时就立刻遍历优先队列

## close 和 shutdown

[简书 - TCP 的四次挥手，连接关闭的2种方式](https://codeantenna.com/a/s2GIJJT2js)

### close

-   对套接字引用计数减一，一旦发现套接字引用计数到 0，就会对套接字进行两个方向的彻底释放
-   在输入方向，系统内核会将该套接字设置为不可读
-   在输出方向，系统内核尝试将发送缓冲区的数据发送给对端，并最后向对端发送一个 FIN 报文开始四次挥手
-   如果对端还继续发送报文，就会收到一个 RST 报文，告诉对端：“Hi, 我已经关闭了，别再给我发数据了。“

### shutdown

```cpp
int shutdown(int sockfd, int howto)
```

-   SHUT_RD(0)：关闭读。从数据角度来看，套接字上接收缓冲区已有的数据将被丢弃，如果再有新的数据流到达，会对数据进行 ACK，然后悄悄地丢弃。也就是说，对端还是会接收到 ACK，在这种情况下根本不知道数据已经被丢弃了。
-   SHUT_WR(1)：关闭写。这就是常被称为”半关闭“的连接。此时，不管套接字引用计数的值是多少，都会直接关闭连接的写方向。套接字上发送缓冲区已有的数据将被立即发送出去，并发送一个 FIN 报文给对端。
-   SHUT_RDWR(2)：相当于 SHUT_RD 和 SHUT_WR 操作各一次，关闭套接字的读和写两个方向。

<img src="close vs shutdown.png" style="zoom: 50%;" />

## 何时关闭连接？

### 产生各种错误

-   如解析错误等，直接 close 即可

### 文件描述符用尽

-   首先，TCP 三次握手发生在 accep() 之前，这意味着即使连接用尽 errno 被置为 EMFILE，客户端依然会认为连接成功

-   虽然可以设置最大支持并发连接数，在超过后直接关闭，但有时候不知道具体是多少，并且有时候 http 请求文件还要用额外的 fd

-   因此预留一个空 fd 处理这类事件，处理 EMFILE 时，用空 fd 接收，然后调用 close 关闭，然后重新打开一个空闲文件占用它

    ```cpp
    if (errno == EMFILE)
    {
        ::close(idleFd_);
        idleFd_ = ::accept(acceptSocket_.fd(), NULL, NULL);
        ::close(idleFd_);
        idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    ```

### read 返回 0

触发事件但 read 返回 0，不能判断 client 是 close 了还是 shutdown，这时 server 应当把消息发完，然后才可以close()；如果对方调用的是 close()，立即 close 就行了

这也是为何要忽略 SIGPIPE 的原因：SIGPIPE 产生与对方关闭连接但还向对方写时，第一次返回 RST, 第二次就会产生 SIGPIPE 信号强行使服务端程序中止，故要忽略 SIGPIPE 信号

## TCP_NODELAY

用于关闭 nagle 算法，避免连续发送数据时出现延迟

## WebServer 加锁机制

-   用条件变量的时候需要加锁，但条件变量可以用信号量替代(比如 CountDownLatch)
-   doPendingFunctors 中，存放任务的队列 functors 需要加锁，因为当前线程执行 functors 中的任务和其他线程塞任务同时进行
-   log 里写日志、双缓冲技术中将内容移入缓冲区时

## 异步调用机制

<img src="multiple_reactor.png" style="zoom:30%;" />

多 Reactor 线程模式可以分为 MainReactor 和 SubReactor

-   MainReactor 可以只有一个，但 SubReactor 一般会有多个
-   Reactor 线程池中的每一 Reactor 线程都会有自己的 Selector、线程和分发的事件循环逻辑
-   MainReactor 只负责响应 client 的连接请求，并建立连接。在建立连接后用 Round Robin 的方式分配给某个 SubReactor
-   SubReactor 都会在一个独立线程中运行，并且维护一个独立的 NIO Selector

### 主从 Reactor 结构优点

1.  **压力问题：** 客户端数量比较多的情况，单个 Reactor 负责监听和转发，那么 Reactor 压力非常的大；
2.  **单点故障问题：** 如果 Reactor 发生故障，则即使后面的 Handler 和 Worker 正常工作，但是整个应用程序无法正常对外提供服务。

**问题：当主线程把新连接分配给了某个 SubReactor，该线程此时可能正阻塞在多路选择器(epoll)的等待中，怎么得知新连接的到来呢？**

-   使用 eventfd 进行异步唤醒，线程会从 epoll_wait 中醒来，得到活跃事件，进行处理。
-   新连接到来，MainReactor 被 listenfd 唤醒，调用 handleEvents() 处理各个channel 的事件
-   handleEvents() 事件调用 Server::handNewConn() 函数，循环 accept 处理所有连接请求
-   用 round-robin 的方式，从 SubReactor 的 ThreadPool 中取出下一个 SubReactor，调用其 queueinloop 方法，把一个任务插入到 SubReactor 的队列中，调用 wakeup() 向 eventfd 里写一个字节，唤醒 SubReactor
-   SubReactor 同样要先执行 handleEvents() 处理 eventfd 的事件，先把那一个字节读掉，然后重新 updatepoller() 
-   SubReactor 然后开始 doPendingFunctors() 处理其他任务，就是前面 queueinloop 插入的任务
-   在代码里，MainReactor 和 SubReactor 都是 EventLoop

为什么用 eventfd 不用 pipe：

-   `pipe`是半双工，所有两个线程通信需要两个pipe文件描述符，而用 `eventfd ` 只需要打开一个文件描述符。
-   `pipe` 只能在两个进程/线程间使用，而 `eventfd` 是广播式的通知，可以多对多。
-   `eventfd`是一个计数器，内核维护的成本非常低，大概是自旋锁+唤醒队列的大小，读写只是一个 int 字节，而`pipe` 完全不同，一来一回数据在用户空间和内核空间有多达4次的复制，而且最糟糕的是，内核要为每个pipe分配最少4k的虚拟内存页，哪怕传送的数据长度为0

## 一些细节

### __builtin_expect

简单的说让 else 或后面的语句紧跟着条件判断（而不是 if），减少跳转带来的消耗，如下面语句，tid 很可能不为 0，故更有可能跳过 if 执行后面的 return

```cpp
inline int tid() {
  if (__builtin_expect(t_cachedTid == 0, 0)) {
    cacheTid();
  }
  return t_cachedTid;
}
```

FileUtil.cpp 为何用 fwrite_unlocked

## 总结

-   Server(入口类,其中包含了Accptor 用来处理连接事件)
-   Channel (对于文件描述符的封装)
-   Epoll (对于epoll的封装,在其中添删Channel)
-   EventLoop(while循环的封装,其中有个Epoll在那收集事件)
-   EventLoopThread(EventLoop线程类,是子线程来使用EventLoop来监控连接套接字的)
-   EventLoopThreadPool(包含指定数量的EventLoopThread来分担所有连接套接字)
-   HTTPData(连接类,在其中读取数据,处理数据,分发数据)
-   Logging(异步日志)

