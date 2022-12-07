#include "tscore/ink_config.h"
#if TS_USE_DSA

#include "shared/IDSA.h"

#include <linux/idxd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cstring>
#include <numa.h>

#define DSA_MIN_SIZE 131072

namespace IDSA
{
// used in common method, performs task on work queue
static inline void
movdir64b(volatile void *portal, void *desc)
{
  asm volatile("sfence\t\n"
               ".byte 0x66, 0x0f, 0x38, 0xf8, 0x02\t\n"
               :
               : "a"(portal), "d"(desc));
}

// used in common method, for clearing status to allow next iterations after block on fault
static inline void
resolve_page_fault(uint64_t addr, uint8_t status)
{
  uint8_t *addr_u8 = (uint8_t *)addr;
  *addr_u8         = ~(*addr_u8);

  if (!(status & 0x80))
    *addr_u8 = ~(*addr_u8);
}

const unsigned long Device::dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR; // descriptor flags

Device::Device(struct accfg_device *dev)
{
  initialize_error   = false;
  this->task_counter = 0;
  d                  = dev;

  struct accfg_wq *wq;

  wq_idx = 0;

  char path[PATH_MAX];
  accfg_wq_foreach(d, wq)
  {
    // get file path which is connected to work queue in device d
    if (accfg_wq_get_user_dev_path(wq, path, PATH_MAX) < 0) {
      initialize_error = true;
      break;
    }
    fds[wq_idx]     = open(path, O_RDWR);
    wq_regs[wq_idx] = mmap(NULL, 0x1000, PROT_WRITE, MAP_SHARED | MAP_POPULATE, fds[wq_idx], 0);
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

Device::~Device()
{
  for (int i = 0; i < wq_idx; i++) {
    munmap(wq_regs[i], 0x1000);
    close(fds[i]);
  }
}

bool
Device::is_initialize_error()
{
  return initialize_error;
}

int
Device::get_numa_node()
{
  return numa_node;
}

void *
Device::memcpy(void *dest, const void *src, std::size_t size)
{
  uint64_t src_addr = (uint64_t)src;
  uint64_t dst_addr = (uint64_t)dest;

  // there must be aligned_alloc, if we are using malloc it won't work
  struct dsa_completion_record *comp =
    (struct dsa_completion_record *)aligned_alloc(accfg_device_get_compl_size(d), sizeof(struct dsa_completion_record));

  if (comp == nullptr) {
    return nullptr;
  }

  struct dsa_hw_desc *hw = (dsa_hw_desc *)malloc(sizeof(struct dsa_hw_desc));
  if (hw == nullptr) {
    return nullptr;
  }

  std::memset(comp, 0, sizeof(struct dsa_completion_record));
  std::memset(hw, 0, sizeof(struct dsa_hw_desc));

  do {
    hw->flags           = dflags;
    hw->opcode          = DSA_OPCODE_MEMMOVE;
    hw->src_addr        = src_addr;
    hw->dst_addr        = dst_addr;
    hw->xfer_size       = size;
    hw->completion_addr = (uint64_t)(void *)comp;
    hw->priv            = 1;
    comp->status        = 0;

    // performs task on work queue
    movdir64b(wq_regs[task_counter % wq_idx], hw);

    // waiting for operation complete
    while (comp->status == 0)
      ;

    // if status == page fault
    if ((comp->status & DSA_COMP_STATUS_MASK) == (DSA_COMP_STATUS_MASK & DSA_COMP_PAGE_FAULT_NOBOF)) {
      // adjust addresses and size
      size -= comp->bytes_completed;
      src_addr += comp->bytes_completed;
      dst_addr += comp->bytes_completed;
      resolve_page_fault(comp->fault_addr, comp->status);
    } else {
      // if success then get out from infinite loop
      break;
    }

  } while (true);

  free(hw);
  free(comp);
  task_counter++;

  return dest;
}

DSA_Devices_Container::DSA_Devices_Container() {}

// DSA_Devices_Container is a singleton and this method should be used to get instance
DSA_Devices_Container &
DSA_Devices_Container::getInstance()
{
  static DSA_Devices_Container instance;
  return instance;
}

DSA_Devices_Container::STATUS
DSA_Devices_Container::initialize()
{
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
  numnodes          = 0;
  dev_idx           = 0;
  current_status    = STATUS::OK;
  initialize_status = STATUS::OK;

  int got_nodes = 0;
  long long free_node_sizes;
  int max_node = numa_max_node();
  numnodes     = numa_num_configured_nodes();
  for (int a = 0; a <= max_node; a++) {
    if (numa_node_size64(a, &free_node_sizes) > 0) {
      got_nodes++;
    }
  }
  if (got_nodes != numnodes) {
    return STATUS::INVALID_NUMA_NODES;
  }

  struct accfg_ctx *ctx;
  int rc = accfg_new(&ctx);
  if (rc < 0) {
    return STATUS::INVALID_ACCFG_CTX;
  }

  struct accfg_device *device;
  accfg_device_foreach(ctx, device)
  {
    // create Device object for each available DSA
    if (strncmp(accfg_device_get_devname(device), "dsa", 3)) {
      continue;
    }
    devices[dev_idx] = new Device(device);
    if (devices[dev_idx]->is_initialize_error()) {
      delete devices[dev_idx];
      continue;
    }
    int numa_node                   = devices[dev_idx]->get_numa_node();
    devices_by_numa_node[numa_node] = devices[dev_idx];
    dev_idx++;
  }

  return STATUS::OK;
}

static int dest_numa_node = 0;

// gets dest numa node and perform memcpy on DSA with this numa node
void *
DSA_Devices_Container::memcpy_on_DSA(void *dest, const void *src, std::size_t size)
{
  current_status = STATUS::OK;

  dest_numa_node++;
  dest_numa_node %= dev_idx;

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

// wrapper (do memcpy on DSA if possible. If not then use std::memcpy)
void *
DSA_Devices_Container::memcpy(void *dest, const void *src, std::size_t size)
{
  if (initialize_status != STATUS::OK || size < DSA_MIN_SIZE) {
    return std::memcpy(dest, src, size);
  }
  void *ret = memcpy_on_DSA(dest, src, size);
  if (current_status == STATUS::MEMCPY_FAILED) {
    return std::memcpy(dest, src, size);
  }
  return ret;
}

// deletes all device objects
DSA_Devices_Container::~DSA_Devices_Container()
{
  for (int i = dev_idx - 1; i >= 0; i--) {
    delete devices[i];
  }
}
} // namespace IDSA

#endif
