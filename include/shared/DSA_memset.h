#ifndef DSA_MEMSET_H
#define DSA_MEMSET_H

#include <string> // for call_counts purpose
#include <map>    // for call_counts purpose

// Namespace for Intel DataStreaming Accelerator
namespace IDSA {
	
	// class for memset implementation
    class DSA_memset {
    	
        private:
		    // map which stores number of memset calls (for testing)
            static std::map<std::string, int> call_counts;
        
        protected:
		    // Singleton
            DSA_memset()
            {
            }
        
        public:
            // Singleton
            DSA_memset(DSA_memset &other) = delete;
        
		    // Singleton
            void operator=(const DSA_memset &) = delete;
           
		    // Prints content of call_counts (for testing)
            static void print_counts(std::string filename);
        	
			// Function which is a replacement for memset
            static void *memset(void *ptr, int value, size_t num, 
        	                   std::string fun = __builtin_FUNCTION(),
        					   std::string file = __builtin_FILE(),
        					   int line = __builtin_LINE());
    };

}

#endif //DSA_MEMSET_H
