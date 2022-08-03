#if TS_USE_DSA

#include "IDSA.h"

#include <vector>
#include <linux/idxd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cstring>
#include <numa.h>
#include <numaif.h>

namespace IDSA {
    
    // used in common method, performs task on work queue
    static inline void movdir64b(volatile void *portal, void *desc)
    {
        asm volatile("sfence\t\n"
                ".byte 0x66, 0x0f, 0x38, 0xf8, 0x02\t\n"  :
                : "a" (portal), "d" (desc));
    }
    
    // used in common method, for calculating timeout
    static inline int umwait(unsigned long timeout, unsigned int state)
    {
        uint8_t r;
        uint32_t timeout_low = (uint32_t)timeout;
        uint32_t timeout_high = (uint32_t)(timeout >> 32);
    
        asm volatile(".byte 0xf2, 0x48, 0x0f, 0xae, 0xf1\t\n"
            "setc %0\t\n"
            : "=r"(r)
            : "c"(state), "a"(timeout_low), "d"(timeout_high));
        return r;
    }
    
    // used in common method, for calculating timeout
    static inline unsigned long rdtsc(void)
    {
        uint32_t a, d;
    
        asm volatile("rdtsc" : "=a"(a), "=d"(d));
        return ((uint64_t)d << 32) | (uint64_t)a;
    }
    
    // used in common method, for calculating timeout
    static inline void umonitor(void *addr)
    {
        asm volatile(".byte 0xf3, 0x48, 0x0f, 0xae, 0xf0" : : "a"(addr));
    }
    
    // used in common method, for clearing status to allow next iterations after block on fault
    static inline void resolve_page_fault(uint64_t addr, uint8_t status)
    {
        uint8_t *addr_u8 = (uint8_t *)addr;
        *addr_u8 =  ~(*addr_u8);
    
        if (!(status & 0x80))
            *addr_u8 = ~(*addr_u8);
    }
    
    const unsigned long Device::dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR; // descriptor flags 
    const unsigned long Device::msec_timeout = 100; // timeout of operation on DSA work queue in ms
    
    Device::Device(struct accfg_device *dev) {
        initialize_error = false;
        this->task_counter = 0;
        d = dev;

        struct accfg_wq *wq;

        wq_idx = 0;
        
        char path[PATH_MAX];
        accfg_wq_foreach(d, wq) {
            // get file path which is connected to work queue in device d
            if (accfg_wq_get_user_dev_path(wq, path, PATH_MAX) < 0) {
                initialize_error = true;
                break;
            }
            fds[wq_idx] = open(path, O_RDWR);
            wq_regs[wq_idx] = mmap(NULL, 0x1000, PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, fds[wq_idx], 0);
            if (wq_regs[wq_idx] == MAP_FAILED) {
                initialize_error = true;
                break;
            }
            wq_idx++;
        }

        if (initialize_error) {
            numa_node = 0;
        } else {
            numa_node = accfg_device_get_numa_node(d);
        }
        task_counter = 0;
    }

    Device::~Device() {
        for (int i = 0; i < wq_idx; i++) {
            munmap(wq_regs[i], 0x1000);
            close(fds[i]);
        }
    }
    
    bool Device::is_initialize_error() {
        return initialize_error;
    }

    int Device::get_numa_node() {
        return numa_node;
    }
    
    void *Device::memfill(void *dest, const void *src, std::size_t size ) {
        // src is ignored in memfill
        return common(dest, src, size, DSA_OPCODE_MEMFILL );
    }
    
    void *Device::memcpy(void *dest, const void *src, std::size_t size ) {
        return common(dest, src, size, DSA_OPCODE_MEMMOVE );
    }

    // common function for memfill and memcpy on DSA
    void *Device::common( void *dest, const void *src, std::size_t size, int opcode ) {

        uint64_t src_addr = (uint64_t)src;
        uint64_t dst_addr = (uint64_t)dest;
        
        // there must be aligned_alloc, if we are using malloc it won't work
        struct dsa_completion_record  *comp = (struct dsa_completion_record *)aligned_alloc(accfg_device_get_compl_size(d), sizeof(struct dsa_completion_record));
    
        if (comp == nullptr) {
            return nullptr;
        }
        
        struct dsa_hw_desc *hw = (dsa_hw_desc *)malloc(sizeof(struct dsa_hw_desc));
        if (hw == nullptr) {
            return nullptr;
        }
        
        do {
        
            std::memset(comp, 0, sizeof(struct dsa_completion_record));
            std::memset(hw, 0, sizeof(struct dsa_hw_desc));
            
            hw->flags = dflags;
            hw->opcode = opcode;
            if (opcode == DSA_OPCODE_MEMMOVE)
                hw->src_addr = src_addr;
            else
                hw->src_addr = 0; // value for filling memory = 0
            hw->dst_addr = dst_addr;
            hw->xfer_size = size;
            hw->completion_addr = (uint64_t)(void *)comp;
            hw->priv = 1;
            comp->status = 0;
    
            // performs task on work queue
            movdir64b(wq_regs[task_counter%wq_idx], hw);
            task_counter++;
    
            // waiting for completion
            unsigned long timeout = (msec_timeout * 1000000UL) * 3;
            int r = 1;
            unsigned long t = 0;
    
            timeout += rdtsc();
            while (comp->status == 0) {
                if (!r) {
                    t = rdtsc();
                    if (t >= timeout) {
                        break;
                    }
                }
    
                umonitor((uint8_t *)comp);
                if (comp->status != 0)
                    break;
                r = umwait(timeout, 0);
            }
            
            // if status == page fault
            if ((comp->status & DSA_COMP_STATUS_MASK) == (DSA_COMP_STATUS_MASK & DSA_COMP_PAGE_FAULT_NOBOF)) {
                // adjust addresses and size
                size -= comp->bytes_completed;
                src_addr += comp->bytes_completed;
                dst_addr += comp->bytes_completed;
                resolve_page_fault(comp->fault_addr, comp->status);
                
                // do memset 0 on CPU - this is the fastest way to get rid of page faults
                std::memset((void *)dst_addr, 0, size);
                // if operation == memfill then nothing more is needed
                // if operation == memcpy then we try to do memcpy again with adjusted arguments
                if (opcode == DSA_OPCODE_MEMFILL) {
                    break;
                }
            } else {
                // if success then get out from infinite loop
                break;
            }
            
        } while (true);
        
        free(hw);
        free(comp);

        return dest;
    }

    DSA_Devices_Container::DSA_Devices_Container() {
    }
    
    // DSA_Devices_Container is a singleton and this method should be used to get instance
    DSA_Devices_Container& DSA_Devices_Container::getInstance() {
        static DSA_Devices_Container instance;
        return instance;
    }

    // DSA_Devices_Container is a singleton
    DSA_Devices_Container::DSA_Devices_Container(DSA_Devices_Container const&) = delete;
    void DSA_Devices_Container::operator=(DSA_Devices_Container const&) = delete;
    
    DSA_Devices_Container::STATUS DSA_Devices_Container::initialize() {
        // prevents from initialization more than once
        if (!is_initialized) {
            is_initialized = true;
        } else {
            return STATUS::ALREADY_INITIALIZED;
        }
        
        for (int i = 0; i < MAX_NO_OF_DEVICES; i++) {
            this->devices[i] = nullptr;
        }
        for (int i = 0; i < MAX_NO_OF_NUMA_NODES; i++) {
            this->devices_by_numa_node[i] = nullptr;
        }
        numnodes = 0;
        dev_idx = 0;
        current_status = STATUS::OK;
        initialize_status = STATUS::OK;
        
        int got_nodes = 0;
        long long free_node_sizes;
        int max_node = numa_max_node();
        numnodes = numa_num_configured_nodes();
        for (int a = 0; a <= max_node; a++) {
            if (numa_node_size(a, &free_node_sizes) > 0) {
                got_nodes++;
            }
        }
        if(got_nodes != numnodes) {
            return STATUS::INVALID_NUMA_NODES;
        }

        struct accfg_ctx *ctx;
        int rc = accfg_new(&ctx);
        if (rc < 0) {
             return STATUS::INVALID_ACCFG_CTX;
        }

        struct accfg_device *device;
        accfg_device_foreach(ctx, device) {
            // create Device object for each available DSA
            devices[dev_idx] = new Device(device);
            if (devices[dev_idx]->is_initialize_error()) {
                delete devices[dev_idx];
                continue;
            }
            int numa_node = devices[dev_idx]->get_numa_node();
            devices_by_numa_node[numa_node] = devices[dev_idx];
            dev_idx++;
        }

        return STATUS::OK;
    }
    
    // gets dest numa node and perform memfill on DSA with this numa node
    void *DSA_Devices_Container::memfill_on_DSA( void *dest, const void *src, std::size_t size ) {
        
        current_status = STATUS::OK;
        
        int status[1];
        status[0]=-1;
        // get numa node of memory pointed by dest
        move_pages(0, 1, &dest, NULL, status, 0);
        int dest_numa_node = status[0];

        if (devices_by_numa_node[dest_numa_node] == nullptr) {
            if (devices[0] == nullptr) {
                current_status = STATUS::MEMFILL_FAILED;
                return nullptr;
            }
            dest_numa_node = devices[0]->get_numa_node();
        }

        void *ret = devices_by_numa_node[dest_numa_node]->memfill(dest, src, size);
        if (ret == nullptr) {
            current_status = STATUS::MEMFILL_FAILED;
        } else {
            current_status = STATUS::OK;
        }
        return ret;
    }

    // gets dest numa node and perform memcpy on DSA with this numa node
    void *DSA_Devices_Container::memcpy_on_DSA( void *dest, const void *src, std::size_t size ) {

        current_status = STATUS::OK;
        
        int status[1];
        status[0]=-1;
        // get numa node of memory pointed by dest
        move_pages(0, 1, &dest, NULL, status, 0);
        int dest_numa_node = status[0];

        if (devices_by_numa_node[dest_numa_node] == nullptr) {
            if (devices[0] == nullptr || devices[0]->is_initialize_error()) {
                current_status = STATUS::MEMCPY_FAILED;
                return nullptr;
            }
            dest_numa_node = devices[0]->get_numa_node();
        }

        void *ret = devices_by_numa_node[dest_numa_node]->memcpy(dest, src, size);
        if (ret == nullptr) {
            current_status = STATUS::MEMCPY_FAILED;
        } else {
            current_status = STATUS::OK;
        }
        return ret;
    }
    
    // writes to memcpy_task_map small blocks of memory
    void DSA_Devices_Container::prepare_to_memcpy( void *dest, const void *src, std::size_t size, void *original_dest ) {
        bool element_updated = false;
        // check if there is existing block in memcpy_task_map which could be extended
        for (const auto& [key_dest, tuple] : memcpy_task_map) {
            if ((char *)key_dest + std::get<1>(tuple) == dest && 
                (char *)std::get<0>(tuple) + std::get<1>(tuple) == src &&
                std::get<2>(tuple) == original_dest) {
                // if there is block with the same original destination, increase entry in map
                memcpy_task_map[key_dest] = std::make_tuple(std::get<0>(tuple), std::get<1>(tuple) + size, original_dest);
                element_updated = true;
                break;
            }
        } 
        // if there are no blocks in memcpy_task_map, create new entity
        if (!element_updated) {
            memcpy_task_map[dest] = std::make_tuple(src, size, original_dest);
        }
    }
    
    // do one memcpy from many adjacent blocks in memcpy_task_map
    void DSA_Devices_Container::do_remaining_memcpy(void *original_dest) {
        // vector is needed here because of errors when we try to
        // do this in 'for (const auto& [key_dest, tuple] : memcpy_task_map)' loop
        std::vector<void *> to_erase;
        for (const auto& [key_dest, tuple] : memcpy_task_map) {
            // do only for block with the same original dest address
            if (std::get<2>(tuple) == original_dest) {
                this->memcpy(key_dest, std::get<0>(tuple), std::get<1>(tuple));
                to_erase.push_back(key_dest);
            }
        }
        for (auto &element : to_erase) {
            memcpy_task_map.erase(element);
        }
    }

    // wrapper (do memcpy on DSA if possible. If not then use std::memcpy)
    void *DSA_Devices_Container::memcpy( void *dest, const void *src, std::size_t size ) {
        if (initialize_status != STATUS::OK) {
            return std::memcpy(dest, src, size );
        }
        void *ret = memcpy_on_DSA(dest, src, size );
        if (current_status == STATUS::MEMCPY_FAILED) {
            return std::memcpy(dest, src, size );
        }
        return ret;
    }
    
    // wrapper (do memfill on DSA if possible. If not then use std::memset)
    void *DSA_Devices_Container::memfill( void *dest, const void *src, std::size_t size ) {
        if (initialize_status != STATUS::OK) {
            return std::memset(dest, 0, size );
        }
        void *ret = memfill_on_DSA(dest, src, size );
        if (current_status == STATUS::MEMFILL_FAILED) {
            return std::memset(dest, 0, size );
        }
        return ret;
    }
    
    // deletes all device objects
    DSA_Devices_Container::~DSA_Devices_Container() {
        for (int i = dev_idx-1; i >= 0; i--) {
            delete devices[i];
        }   
    }
}

#endif

