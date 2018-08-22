#include "hasher.hpp"

#include "boost/thread.hpp"
#include "boost/lockfree/queue.hpp"
#include <boost/crc.hpp>
#include <boost/cstdint.hpp>
#include <boost/log/trivial.hpp>
using namespace boost;

#include <queue>
#include <iostream>
#include <fstream>

typedef struct block {
    unsigned int index;
    size_t data_len;
    char* buf;
    size_t buf_size;
} hash_block;

typedef struct sum {
    unsigned int index;
    std::string hex;
} hash_sum;

class Sums {
  public:
    void putSum(hash_sum& sum) {
        BOOST_LOG_TRIVIAL(trace) << "Sums: Going to put sum for " << sum.index;
        unique_lock<mutex> lock(this->hash_mtx);
        if (this->hashes.empty() || this->hashes.back().index < sum.index) {
            this->hashes.push_back(sum);
            BOOST_LOG_TRIVIAL(trace) << "Sums: Pushback " << sum.index;
        } else {
            std::deque<hash_sum>::iterator hs = this->hashes.begin();
            while (hs != this->hashes.end()) {
                if (hs->index > sum.index) {
                    this->hashes.insert(hs, sum);
                    BOOST_LOG_TRIVIAL(trace) << "Sums: Insert " << sum.index;
                    break;
                }
                hs++;
            }
        }
        lock.unlock();
        this->hash_cond.notify_one();
        BOOST_LOG_TRIVIAL(trace) << "Sums: Sum in for " << sum.index;
    }
    bool getSum(unsigned int index, hash_sum& sum) {
        unique_lock<mutex> lock(this->hash_mtx);
        while (this->hashes.empty() || this->hashes.front().index != index) {
            if (!this->workers) {
                BOOST_LOG_TRIVIAL(trace) << "Sums: No workers (done)";
                return false;
            }
            BOOST_LOG_TRIVIAL(trace) << "Sums: Waiting for sum, workers=" << this->workers;
            this->hash_cond.wait(lock);
        }
        sum = this->hashes.front();
        this->hashes.pop_front();
        BOOST_LOG_TRIVIAL(trace) << "Sums: Got sum";
        return true;
    }
    void addWorker(void) {
        {
            lock_guard<mutex> lock(this->hash_mtx);
            this->workers++;
        }
    }
    void removeWorker(void) {
        {
            lock_guard<mutex> lock(this->hash_mtx);
            this->workers--;
        }
        this->hash_cond.notify_one();
    }
    void waitForWorkers(void) {
        unique_lock<mutex> lock(this->hash_mtx);
        if (!this->workers) {
            this->hash_cond.wait(lock);
        }
    }
  private:
    std::deque<hash_sum> hashes;
    condition_variable hash_cond;
    mutex hash_mtx;
    std::atomic<int> workers{0};
};

class Buffer {
  public:
    Buffer(unsigned int workers, size_t block_size) : empty(workers*2), full(workers*2) {
        BOOST_LOG_TRIVIAL(debug) << "W: " << workers << " BS: " << block_size;
        for (unsigned int i = 0; i < workers*2; i++) {
            hash_block b;
            b.buf = new char[block_size];
            b.buf_size = block_size;
            b.data_len = 0;
            b.index = 0;
            this->empty.push(b);
        }
        this->complete = false;
    }
    ~Buffer(void) {
        this->empty.consume_all([](hash_block& bl) { delete[] bl.buf; });
        this->full.consume_all([](hash_block& bl) { delete[] bl.buf; });
    }
    bool getEmptyBlock(hash_block& block) {
        unique_lock<mutex> lock(this->empty_mtx);
        while (!this->empty.pop(block)) {
            BOOST_LOG_TRIVIAL(trace) << "Buffer: Waiting for empty block";
            this->empty_cond.wait(lock);
        }
        BOOST_LOG_TRIVIAL(trace) << "Buffer: Got empty block";
        return true;
    }
    bool getFullBlock(hash_block& block) {
        unique_lock<mutex> lock(this->full_mtx);
        while (!this->full.pop(block)) {
            if (this->complete) {
                BOOST_LOG_TRIVIAL(trace) << "Buffer: Complete (no full block)";
                return false;
            }
            BOOST_LOG_TRIVIAL(trace) << "Buffer: Waiting for full block";
            this->full_cond.wait(lock);
        }
        BOOST_LOG_TRIVIAL(trace) << "Buffer: Got full block";
        return true;
    }
    void returnEmptiedBlock(hash_block& block) {
        {
            lock_guard<mutex> lock(this->empty_mtx);
            this->empty.push(block);
        }
        BOOST_LOG_TRIVIAL(trace) << "Buffer: Returned empty block";
        this->empty_cond.notify_one();
    }
    void returnFilledBlock(hash_block& block) {
        {
            lock_guard<mutex> lock(this->full_mtx);
            this->full.push(block);
        }
        BOOST_LOG_TRIVIAL(trace) << "Buffer: Returned full block";
        this->full_cond.notify_one();
    }
    void setInactive(void) {
        {
            lock_guard<mutex> lock(this->full_mtx);
            this->complete = true;
        }
        this->full_cond.notify_all();
    }
  private:
    lockfree::queue<hash_block> empty;
    lockfree::queue<hash_block> full;
    condition_variable full_cond;
    mutex full_mtx;
    condition_variable empty_cond;
    mutex empty_mtx;
    std::atomic<bool> complete;
};

class CRCHashWorker {
  public:
    CRCHashWorker(Sums& s, Buffer& b) : sums(s), buffer(b) {  }
    void operator() () {
        hash_block block;
        BOOST_LOG_TRIVIAL(debug) << "CRCHashWorker: Start";
        this->sums.addWorker();
        while (this->buffer.getFullBlock(block)) {
            crc_32_type crcr;
            crcr.process_bytes(block.buf, block.data_len);
            std::stringstream sstream;
            sstream << std::setfill('0') << std::setw(8) << std::hex << crcr.checksum();
            hash_sum sum;
            sum.hex = sstream.str();
            sum.index = block.index;
            this->sums.putSum(sum);
            this->buffer.returnEmptiedBlock(block);
        }
        this->sums.removeWorker();
        BOOST_LOG_TRIVIAL(debug) << "CRCHashWorker: Done";
    }
  private:
    Sums& sums;
    Buffer& buffer;
};

class FileReadWorker {
  public:
    FileReadWorker(std::ifstream& inp, Buffer& b) : input(inp), buffer(b) {  }
    void operator() () {
        hash_block block;
        BOOST_LOG_TRIVIAL(debug) << "FileReadWorker: Start";
        unsigned int idx = 0;
        while (this->buffer.getEmptyBlock(block)) {
            block.data_len = this->input.read(block.buf, block.buf_size).gcount();
            block.index = idx;
            BOOST_LOG_TRIVIAL(trace) << "FileReadWorker: Got " << block.data_len << " bytes from input, block: " << idx;
            this->buffer.returnFilledBlock(block);
            if (block.buf_size != block.data_len) {
                BOOST_LOG_TRIVIAL(debug) << "FileReadWorker: Last block";
                break;
            }
            idx++;
        }
        this->buffer.setInactive();
        BOOST_LOG_TRIVIAL(debug) << "FileReadWorker: Done";
    }
  private:
    std::ifstream& input;
    Buffer& buffer;
};

Hasher::Hasher() {
    this->threads_count = thread::hardware_concurrency();
    if (!this->threads_count) {
        this->threads_count = thread::physical_concurrency();
    }
    BOOST_LOG_TRIVIAL(debug) << "Hasher.threads_count: " << this->threads_count;
}

void Hasher::processFile(const std::string& inf, const std::string& outf, size_t block_size) {
    Buffer b(this->threads_count, block_size);
    Sums s;
    std::ifstream input;
    std::ofstream output;

    output.open(outf, std::ios::binary);
    if (output.fail()) {
        BOOST_LOG_TRIVIAL(debug) << "Hasher: Unable to open output";
        throw std::ios_base::failure("Unable to open output file");
    }

    input.open(inf, std::ios::binary);
    if (input.fail()) {
        BOOST_LOG_TRIVIAL(debug) << "Hasher: Unable to open input";
        throw std::ios_base::failure("Unable to open input file");
    }

    thread_group worker_threads;
    for (unsigned int i = 0; i < this->threads_count; i++) {
        worker_threads.create_thread(CRCHashWorker{s, b});
    }
    thread reader_thread(FileReadWorker{input, b});

    unsigned int idx = 0;
    hash_sum sum;
    s.waitForWorkers();
    output << "#CRC32: " << block_size << std::endl;
    while (s.getSum(idx, sum)) {
        BOOST_LOG_TRIVIAL(debug) << "Sum: " << sum.hex << " for " << sum.index;
        output << sum.hex << std::endl;
        idx++;
    }

    reader_thread.join();
    worker_threads.join_all();
}
