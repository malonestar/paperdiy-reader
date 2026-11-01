# Host tests: make && ./run.sh   (needs g++ with ASan/UBSan)
MAIN := ../../main
CXXFLAGS := -std=c++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Istub -I$(MAIN) -I$(MAIN)/third_party
CFLAGS := -g -O1 -fsanitize=address,undefined -I$(MAIN)/third_party

all: test_layout test_portal test_zip test_epub

test_layout: test_layout.cpp stubs.cpp $(MAIN)/reader/layout.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

test_portal: test_portal.cpp $(MAIN)/portal/fs_path.cpp $(MAIN)/net/dns_reply.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

miniz_lite.o: $(MAIN)/third_party/miniz_lite.c
	$(CC) $(CFLAGS) -c $< -o $@

test_zip: test_zip.cpp miniz_lite.o
	$(CXX) $(CXXFLAGS) $^ -o $@

clean:
	rm -f test_layout test_portal test_zip *.o

.PHONY: all clean

test_epub: test_epub.cpp stubs.cpp $(MAIN)/reader/book_epub.cpp $(MAIN)/reader/layout.cpp miniz_lite.o
	$(CXX) $(CXXFLAGS) $^ -o $@
