#ifndef DATABASE_H
#define DATABASE_H

#include <future>
#include <map>
#include <string>

namespace database {
struct Connection;
struct ConnectionProxy;
struct Pool {
  ~Pool();
  Pool(const char *, const size_t);
  std::future<ConnectionProxy> getConnection();
  void release(Connection * const);
  std::map<Connection *, bool> connections_;
  std::string path_;
  const size_t size_;
};
}

#endif // DATABASE_H
