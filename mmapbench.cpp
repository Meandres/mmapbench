// sudo apt install libtbb-dev
// g++ -O3 -g mmapbench.cpp -o mmapbench -ltbb -pthread

#include <atomic>
#include <boost/algorithm/string.hpp>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/fs.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "tbb/enumerable_thread_specific.h"

using namespace std;

#define check(expr) if (!(expr)) { perror(#expr); throw; }

double gettime() {
  struct timeval now_tv;
  gettimeofday (&now_tv,NULL);
  return ((double)now_tv.tv_sec) + ((double)now_tv.tv_usec)/1000000.0;
}

uint64_t readTLBShootdownCount() {
  std::ifstream irq_stats("/proc/interrupts");
  assert (!!irq_stats);

  for (std::string line; std::getline(irq_stats, line); ) {
    if (line.find("TLB") != std::string::npos) {
      std::vector<std::string> strs;
      boost::split(strs, line, boost::is_any_of("\t "));
      uint64_t count = 0;
      for (size_t i = 0; i < strs.size(); i++) {
	std::stringstream ss(strs[i]);
	uint64_t c;
	ss >> c;
	count += c;
      }
      return count;
    }
  }
  return 0;
}

uint64_t readIObytesOne() {
  std::ifstream stat("/sys/block/nvme1n1/stat");
  assert (!!stat);

  for (std::string line; std::getline(stat, line); ) {
    std::vector<std::string> strs;
    boost::split(strs, line, boost::is_any_of("\t "), boost::token_compress_on);
    std::stringstream ss(strs[3]);
    uint64_t c;
    ss >> c;
    return c*512;
  }
  return 0;
}

uint64_t readIObytes() {
  std::ifstream stat("/proc/diskstats");
  assert (!!stat);

  uint64_t sum = 0;
  for (std::string line; std::getline(stat, line); ) {
    if (line.find("nvme") != std::string::npos) {
      std::vector<std::string> strs;
      boost::split(strs, line, boost::is_any_of("\t "), boost::token_compress_on);

      std::stringstream ss(strs[6]);
      uint64_t c;
      ss >> c;
      sum += c*512;
    }
  }
  return sum;
}

std::atomic<bool> keepGoing;

int main(int argc, char** argv) {
  if (argc < 6) {
    cerr << "dev virtSize(in GiB) workload threads timetorun(in sec)" << endl;
    return 1;
  }

  int fd = open(argv[1], O_RDWR);
  check(fd != -1);

  unsigned threads = atoi(argv[4]);
  keepGoing.store(true);

  struct stat sb;
  check(stat(argv[1], &sb) != -1);
  int maxTime = atoi(argv[5]);
  int mode = atoi(argv[3]);

  uint64_t virtSize = atoi(argv[2]);
  uint64_t fileSize = virtSize * 1024 * 1024 * 1024;

  char* p = (char*)mmap(nullptr, fileSize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  assert(p != MAP_FAILED);

  tbb::enumerable_thread_specific<atomic<uint64_t>> counts;
  tbb::enumerable_thread_specific<atomic<uint64_t>> sums;

  vector<thread> t;
  for (unsigned i=0; i<threads; i++) {
    t.emplace_back([&]() {
      atomic<uint64_t>& count = counts.local();
      atomic<uint64_t>& sum = sums.local();

    if(mode == 0){
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<uint64_t> rnd(0, fileSize/4096ul);

	while (keepGoing.load()) {
    uint64_t pos = rnd(gen)*4096ul;
	  p[pos] = count;
	  count++;
	}
      } else {
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<uint64_t> rnd(0, fileSize/4096ul);

	while (keepGoing.load()) {
    uint64_t pos = rnd(gen)*4096ul;
	  sum += p[pos];
	  count++;
	}
      }
    });
  }

  atomic<uint64_t> cpuWork(0);
  t.emplace_back([&]() {
    while (true) {
      double x = cpuWork.load();
      for (uint64_t r=0; r<10000; r++) {
	x = exp(log(x));
      }
      cpuWork++;
    }
  });

  cout << "system,workload,pageSize,thread,time,throughput" << endl;
  auto lastShootdowns = readTLBShootdownCount();
  auto lastIObytes = readIObytes();
  double start = gettime();
  while (true) {
    sleep(1);
    uint64_t workCount = 0;
    for (auto& x : counts)
      workCount += x.exchange(0);
    double t = gettime() - start;
    cout << "mmap," << mode << ",4096," << threads  << "," << t << "," << workCount << endl;
    if(t >= maxTime){
      keepGoing.store(false);
      break;
    }
  }
  for(auto& t: t){
    t.join();
  }

  return 0;
}
