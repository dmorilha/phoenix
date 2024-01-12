#ifndef DATABASE_H
#define DATABASE_H

#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <string>

namespace database {
struct Connection;
struct ConnectionProxy;
struct Pool {
  friend struct ConnectionProxy;

  ~Pool();
  Pool(const char *, const size_t);
  std::future<ConnectionProxy> getConnection();
  void release(Connection * const);

  std::string path_;
  const size_t size_;
  size_t available_;
  std::condition_variable condition_variable_;
  std::map<Connection *, bool> connections_;
  std::mutex mutex_;
};
}

#endif // DATABASE_H
