source = $(wildcard *.cc)
objects = $(patsubst %.cc,%.o, ${source})

run: a.out
	./$< &
	sleep 1;
	curl 'http://127.0.0.1:8080/';
	rm -vf ./database.sql;

a.out : $(objects)
	$(CXX) -lsqlite3 -o $@ $^

%.o : %.cc
	$(CXX) -std=c++20 -o $@ -c $<

clean:
	rm -rfv $(objects)
