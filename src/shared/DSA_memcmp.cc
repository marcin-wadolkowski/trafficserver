#include <iostream>
#include <fstream> // for print_counts purpose
#include <cstring>  // for memcmp

#include "../../include/shared/DSA_memcmp.h"

using namespace IDSA;

std::map<std::string, int> DSA_memcmp::call_counts {};

// Function which is a replacement for memcmp, currently fills call_counts map 
// with the data (for testing purposes)
int DSA_memcmp::memcmp(const void *lhs, const void *rhs, size_t count, 
                        std::string fun, std::string file, int line) {
							
    // adds only when n > 1kB
    if (n >= 1024) {	
		call_counts[file+"_"+std::to_string(line)+"_"+fun+","+std::to_string(count)]++;
	}
    return ::memcmp(lhs, rhs, count);
}

// output call_counts map (for testing purposes)
void DSA_memcmp::print_counts(std::string filename) {
	std::ofstream output_file;
    output_file.open(filename);

    for (auto it = call_counts.begin(); it != call_counts.end(); ++it) {
		// each line is in format:
        // <file>_<line>_<function name>,<size of copied block>,<number of calls>
		// to be easily stored as CSV file
        output_file << it->first << "," << it->second << "\n";
    } 
	
	output_file.close();
}
