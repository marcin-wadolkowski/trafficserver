#ifndef DSA_MEMCPY_H
#define DSA_MEMCPY_H

#include <string> // for call_counts purpose
#include <map>    // for call_counts purpose

// Namespace for Intel DataStreaming Accelerator
namespace DSA {
	
	// class for memcpy implementation
    class DSA_memcpy {
    	
        private:
		    // map which stores number of memcpy calls (for testing)
            static std::map<std::string, int> call_counts;
        
        protected:
		    // Singleton
            DSA_memcpy()
            {
            }
        
        public:
            // Singleton
            DSA_memcpy(DSA_memcpy &other) = delete;
        
		    // Singleton
            void operator=(const DSA_memcpy &) = delete;
           
		    // Prints content of call_counts (for testing)
            static void print_counts(std::string filename);
        	
			// Function which is a replacement for memcpy
            static void *memcpy(void *dest, const void *src, size_t n, 
        	                   std::string fun = __builtin_FUNCTION(),
        					   std::string file = __builtin_FILE(),
        					   int line = __builtin_LINE());
    };

}

#endif //DSA_MEMCPY_H
