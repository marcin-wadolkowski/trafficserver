#ifndef DSA_MEMMOVE_H
#define DSA_MEMMOVE_H

#include <string> // for call_counts purpose
#include <map>    // for call_counts purpose

// Namespace for Intel DataStreaming Accelerator
namespace IDSA {
	
	// class for memmove implementation
    class DSA_memmove {
    	
        private:
		    // map which stores number of memmove calls (for testing)
            static std::map<std::string, int> call_counts;
        
        protected:
		    // Singleton
            DSA_memmove()
            {
            }
        
        public:
            // Singleton
            DSA_memmove(DSA_memmove &other) = delete;
        
		    // Singleton
            void operator=(const DSA_memmove &) = delete;
           
		    // Prints content of call_counts (for testing)
            static void print_counts(std::string filename);
        	
			// Function which is a replacement for memmove
            static void *memmove(void *dest, const void *src, size_t n, 
        	                   std::string fun = __builtin_FUNCTION(),
        					   std::string file = __builtin_FILE(),
        					   int line = __builtin_LINE());
    };

}

#endif //DSA_MEMMOVE_H
