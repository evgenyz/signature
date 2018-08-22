#ifndef __HASHER_H__
#define __HASHER_H__

#include <iostream>

class Hasher {
  public:
    Hasher(unsigned int threads);
    Hasher();
    void processFile(const std::string& inf, const std::string& outf, size_t block_size);
  private:
    unsigned int threads_count;
};

#endif  //__HASHER_H__