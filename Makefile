run: a.out
	./$< &
	sleep 1;
	curl 'http://127.0.0.1:8080/';
	rm -vf ./database.sql;

a.out: c.cc
	$(CXX) -std=c++20 -lsqlite3 -o $@ $<
