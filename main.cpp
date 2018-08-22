#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include <iostream>
using namespace std;

#include "hasher.hpp"

int main(int argc, const char* argv[]) {
    size_t block_size;
    string input_file;
    string output_file;

    boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::info);

    po::options_description desc("Usage: signature [-b <block-size>] <input-file> [<output-file>]\nOptions");
    desc.add_options()
        ("help", "this message")
        ("block-size,b", po::value<size_t>(&block_size)->default_value(1048576), "set size of the block (optional, default: 1048576), in bytes")
        ("input-file", po::value<string>(&input_file), "name of a file to process")
        ("output-file", po::value<string>(&output_file), "name of a file to write resulting signature (optional, default: '<input-file>.sig')")
    ;
    po::positional_options_description po_desc;
    po_desc.add("input-file", 1).add("output-file", 2);

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(po_desc).run(), vm);
    }
    catch (po::invalid_option_value ex) {
        cout << "Invalid command line options: " << ex.what() << endl;
        return 2;
    }
    catch (po::unknown_option ex) {
        cout << "Unable to parse command line options: " << ex.what() << endl;
        return 3;
    }
    po::notify(vm);

    if (vm.count("help")) {
        cout << desc << "\n";
        return 1;
    }

    if (!vm.count("input-file")) {
        cout << "There is no input file defined. Aborting" << endl << endl << desc << endl;
        return 10;
    }

    if (!vm.count("output-file")) {
        output_file = input_file;
        output_file.append(".sig");
    }

    BOOST_LOG_TRIVIAL(debug) << "Files: input=" << input_file << ", output=" << output_file << endl;

    try {
        Hasher hs;
        hs.processFile(input_file, output_file, block_size);
    }
    catch (std::runtime_error ex) {
        cout << "Aborting. " << ex.what() << endl;
    }

    return 0;
}