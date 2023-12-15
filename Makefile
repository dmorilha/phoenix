a.out: c.cc
	$(CXX) -std=c++20 -lsqlite3 -o $@ $<
