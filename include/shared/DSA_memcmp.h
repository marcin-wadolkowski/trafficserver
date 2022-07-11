#ifndef DSA_MEMCMP_H
#define DSA_MEMCMP_H

#include <string> // for call_counts purpose
#include <map>    // for call_counts purpose

// Namespace for Intel DataStreaming Accelerator
namespace IDSA {
	
	// class for memcmp implementation
    class DSA_memcmp {
    	
        private:
		    // map which stores number of memcmp calls (for testing)
            static std::map<std::string, int> call_counts;
        
        protected:
		    // Singleton
            DSA_memcmp()
            {
            }
        
        public:
            // Singleton
            DSA_memcmp(DSA_memcmp &other) = delete;
        
		    // Singleton
            void operator=(const DSA_memcmp &) = delete;
           
		    // Prints content of call_counts (for testing)
            static void print_counts(std::string filename);
        	
			// Function which is a replacement for memcmp
            static int memcmp(const void *lhs, const void *rhs, size_t count, 
        	                   std::string fun = __builtin_FUNCTION(),
        					   std::string file = __builtin_FILE(),
        					   int line = __builtin_LINE());
    };

}

#endif //DSA_MEMCMP_H
